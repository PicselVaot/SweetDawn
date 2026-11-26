/*
 * ipc/ipc_server.c — Serveur local sur socket Unix
 *
 * Organisation du fichier :
 *   1. Outils         : signaux, erreurs, socket orphelin, mode non bloquant.
 *   2. Réponses       : mise en file d'une réponse JSON pour un client.
 *   3. Requêtes       : validation de l'enveloppe, appel du gestionnaire.
 *   4. Entrées/sorties: lecture et écriture non bloquantes d'un client.
 *   5. Web            : HTTP minimal pour la page de contrôle (GET /,
 *                       POST /api, GET /events).
 *   6. API publique   : init, set_handler, set_tick, run, stop, cleanup.
 *
 * Choix de conception :
 *   - Tous les sockets sont non bloquants : un client lent ou muet ne
 *     peut jamais figer le service ni gêner les autres clients.
 *   - Chaque client a un tampon d'entrée (les lignes peuvent arriver en
 *     morceaux, ou plusieurs d'un coup) et un tampon de sortie (une
 *     réponse peut ne partir qu'en partie).
 *   - Un client traite une requête à la fois : tant que sa réponse n'est
 *     pas entièrement envoyée, ses lignes suivantes attendent dans son
 *     tampon d'entrée. Les réponses arrivent donc dans l'ordre.
 *   - Un client qui envoie "subscribe" reçoit ensuite, en plus des
 *     réponses, les événements diffusés par ipc_server_broadcast().
 *   - Un client web est un client comme un autre : il passe par la même
 *     boucle, le même process_message() et le même gestionnaire. Seule
 *     l'enveloppe change (en-têtes HTTP au lieu d'une ligne JSON).
 */

#define _POSIX_C_SOURCE 200809L

#include "ipc/ipc_server.h"
#include "ipc/ipc_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/*
 * Attente maximale de poll(). Elle garantit que la tâche périodique
 * (tick) tourne au moins une fois par seconde, même sans client.
 */
#define IPC_POLL_TIMEOUT_MS 1000

/* Nombre d'octets lus au plus à chaque recv(). */
#define IPC_READ_SIZE 1024

/* Web : taille maximale d'une requête HTTP, en-têtes et corps compris. */
#define IPC_HTTP_MAX_REQUEST 8192

/* Web : connexion fermée après ce délai sans requête complète. */
#define IPC_HTTP_IDLE_S 30

/* Web : nombre maximal de flux /events ouverts en même temps. */
#define IPC_HTTP_MAX_SSE 8

/*
 * Web : commentaire envoyé sur chaque flux /events à cet intervalle,
 * pour que les proxys (tailscale serve...) ne le coupent pas.
 */
#define IPC_HTTP_HEARTBEAT_S 25

/*
 * État d'un client connecté.
 *
 * input  : octets reçus pas encore traités (une ligne partielle, ou
 *          plusieurs lignes arrivées d'un coup).
 * output : messages en cours d'envoi (une réponse, et pour un abonné
 *          des événements à la suite), alloué à la taille nécessaire ;
 *          output_offset indique ce qui est déjà parti.
 *          output_length == 0 : rien en attente.
 */
struct IpcClient {
    int fd;            /* -1 : emplacement libre */
    int subscribed;    /* reçoit les événements (commande "subscribe") */

    int http;          /* client web (sinon client IPC)                 */
    int sse;           /* flux GET /events ouvert                       */
    int close_after;   /* fermer dès que la sortie est entièrement partie */
    time_t idle_since; /* web : début de l'attente de la requête suivante */

    /* Assez grand pour une requête HTTP ; un client IPC reste limité à
       IPC_SERVER_MAX_MESSAGE (voir read_client). */
    char input[IPC_HTTP_MAX_REQUEST + 1];
    size_t input_length;

    char *output;
    size_t output_length;
    size_t output_offset;
};

typedef struct IpcClient IpcClient;

/*
 * Au-delà, le client ne lit plus ce qu'on lui envoie (bloqué, ou trop
 * lent pour suivre les événements) : il est déconnecté plutôt que de
 * laisser sa file grossir sans fin.
 */
#define IPC_MAX_PENDING_OUTPUT (256 * 1024)

/* ------------------------------------------------------------------------- */
/* Outils                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Positionné par SIGINT (Ctrl+C) ou SIGTERM (systemctl stop, kill).
 * La boucle le consulte à chaque tour et s'arrête proprement.
 */
static volatile sig_atomic_t stop_requested = 0;

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void set_error(
    char *message,
    size_t capacity,
    const char *description)
{
    if (message != NULL && capacity > 0) {
        snprintf(message, capacity, "%s", description);
    }
}

/*
 * Indique si path désigne un socket Unix que plus personne n'écoute.
 *
 * Deux conditions, toutes deux nécessaires avant de supprimer quoi que
 * ce soit :
 *   - le fichier est bien un socket (jamais un fichier ordinaire) ;
 *   - se connecter dessus est refusé (ECONNREFUSED) : aucun serveur
 *     n'est derrière.
 */
static int socket_is_stale(const char *path)
{
    struct stat info;
    struct sockaddr_un address;
    int fd;
    int result;
    int saved_errno;

    if (lstat(path, &info) != 0 || !S_ISSOCK(info.st_mode)) {
        return 0;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) {
        return 0;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);

    result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    saved_errno = errno;

    close(fd);

    return result != 0 && saved_errno == ECONNREFUSED;
}

/* Passe un descripteur en mode non bloquant (O_NONBLOCK). */
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Secondes sur l'horloge monotone (délais du web). */
static time_t monotonic_seconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }

    return now.tv_sec;
}

/* Ferme la connexion d'un client et libère son emplacement. */
static void client_reset(IpcClient *client)
{
    if (client->fd >= 0) {
        close(client->fd);
    }

    free(client->output);

    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

/* ------------------------------------------------------------------------- */
/* Réponses                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Ajoute des octets à la suite de ce que le client doit encore recevoir.
 * Ils partiront avec write_client().
 *
 * Retour : 0, ou -1 si la mémoire manque ou si le client a déjà trop
 * de messages en retard (il doit alors être déconnecté).
 */
static int client_append_raw(
    IpcClient *client,
    const void *data,
    size_t length)
{
    size_t total = client->output_length + length;

    if (total > IPC_MAX_PENDING_OUTPUT) {
        return -1;
    }

    char *output = realloc(client->output, total > 0 ? total : 1);

    if (output == NULL) {
        return -1;
    }

    memcpy(output + client->output_length, data, length);

    client->output = output;
    client->output_length = total;

    return 0;
}

/* Ajoute une ligne IPC : le texte suivi de '\n'. */
static int client_append(IpcClient *client, const char *text, size_t length)
{
    if (client_append_raw(client, text, length) != 0) {
        return -1;
    }

    return client_append_raw(client, "\n", 1);
}

/*
 * Met en file une réponse HTTP complète : ligne d'état, en-têtes, corps.
 * extra : en-têtes supplémentaires, chacun terminé par "\r\n" (ou NULL).
 * client->close_after doit être fixé avant l'appel.
 */
static int http_queue(
    IpcClient *client,
    const char *status,
    const char *content_type,
    const char *extra,
    const void *body,
    size_t body_length)
{
    char head[512];

    int length = snprintf(
        head,
        sizeof(head),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "%s%s\r\n",
        status,
        content_type,
        body_length,
        extra != NULL ? extra : "",
        client->close_after ? "Connection: close\r\n" : ""
    );

    if (length < 0 || (size_t)length >= sizeof(head)) {
        return -1;
    }

    if (client_append_raw(client, head, (size_t)length) != 0) {
        return -1;
    }

    return body_length > 0
           ? client_append_raw(client, body, body_length)
           : 0;
}

/* Réponse HTTP courte en texte brut (erreurs du transport web). */
static int http_simple(IpcClient *client, const char *status, int close)
{
    if (close) {
        client->close_after = 1;
    }

    return http_queue(
        client,
        status,
        "text/plain; charset=utf-8",
        "Cache-Control: no-store\r\n",
        status,
        strlen(status)
    );
}

/*
 * Construit la réponse JSON et la place dans le tampon de sortie du
 * client. Elle sera réellement envoyée par write_client(), quand le
 * socket sera prêt.
 *
 * Retour : 0, ou -1 si la réponse n'a pas pu être construite.
 */
static int queue_response(
    IpcClient *client,
    struct json_object *request,
    int success,
    struct json_object *result,
    const char *error_code,
    const char *error_message)
{
    struct json_object *response = ipc_protocol_response(
        request,
        success,
        result,
        error_code,
        error_message
    );

    if (response == NULL) {
        return -1;
    }

    const char *serialized = json_object_to_json_string_ext(
        response,
        JSON_C_TO_STRING_PLAIN
    );

    if (serialized == NULL) {
        json_object_put(response);
        return -1;
    }

    int queued;

    if (client->http) {
        /* Toujours 200 : le succès ou l'échec est dans le JSON ("ok"). */
        queued = http_queue(
            client,
            "200 OK",
            "application/json",
            "Cache-Control: no-store\r\n",
            serialized,
            strlen(serialized)
        );
    } else {
        queued = client_append(client, serialized, strlen(serialized));
    }

    json_object_put(response);

    return queued;
}

/* Raccourci : met en file une réponse d'erreur { ok: false, error }. */
static int send_error_response(
    IpcClient *client,
    struct json_object *request,
    const char *code,
    const char *message)
{
    return queue_response(
        client,
        request,
        0,
        NULL,
        code,
        message
    );
}

/* ------------------------------------------------------------------------- */
/* Requêtes                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Traite une ligne reçue : vérifie l'enveloppe de la requête, appelle le
 * gestionnaire, puis met la réponse en file.
 *
 * Champs exigés : type = "request", id (entier ou chaîne),
 * command (chaîne non vide), args (tableau). Toute erreur produit une
 * réponse d'erreur : le client n'est jamais laissé sans réponse.
 *
 * Retour : 0, ou -1 si aucune réponse n'a pu être préparée (le client
 * est alors déconnecté par l'appelant).
 */
static int process_message(
    IpcServer *server,
    IpcClient *client,
    const char *message,
    size_t message_length)
{
    struct json_object *request = NULL;
    struct json_object *type = NULL;
    struct json_object *id = NULL;
    struct json_object *command = NULL;
    struct json_object *args = NULL;
    struct json_object *result_object = NULL;

    int queued = -1;

    if (ipc_protocol_parse(
            message,
            message_length,
            &request) != 0) {
        return send_error_response(
            client,
            NULL,
            "INVALID_JSON",
            "Le message JSON est invalide"
        );
    }

    if (!json_object_object_get_ex(request, "type", &type) ||
        !json_object_is_type(type, json_type_string)) {
        queued = send_error_response(
            client,
            request,
            "INVALID_REQUEST",
            "Le champ type doit etre une chaine"
        );
        goto done;
    }

    if (strcmp(json_object_get_string(type), "request") != 0) {
        queued = send_error_response(
            client,
            request,
            "UNSUPPORTED_TYPE",
            "Seuls les messages request sont acceptes"
        );
        goto done;
    }

    if (!json_object_object_get_ex(request, "id", &id) ||
        !(json_object_is_type(id, json_type_int) ||
          json_object_is_type(id, json_type_string))) {
        queued = send_error_response(
            client,
            request,
            "INVALID_REQUEST",
            "Le champ id doit etre un entier ou une chaine"
        );
        goto done;
    }

    if (!json_object_object_get_ex(request, "command", &command) ||
        !json_object_is_type(command, json_type_string) ||
        json_object_get_string_len(command) == 0) {
        queued = send_error_response(
            client,
            request,
            "INVALID_REQUEST",
            "Le champ command doit etre une chaine non vide"
        );
        goto done;
    }

    if (!json_object_object_get_ex(request, "args", &args) ||
        !json_object_is_type(args, json_type_array)) {
        queued = send_error_response(
            client,
            request,
            "INVALID_REQUEST",
            "Le champ args doit etre un tableau JSON"
        );
        goto done;
    }

    /*
     * Abonnement aux événements : géré ici, car il concerne la
     * connexion du client et non une commande de l'application.
     */
    if (strcmp(json_object_get_string(command), "subscribe") == 0) {
        /* Une réponse HTTP se termine : le web s'abonne avec /events. */
        if (client->http) {
            queued = send_error_response(
                client,
                request,
                "UNSUPPORTED_COMMAND",
                "Depuis le web, utiliser GET /events"
            );
            goto done;
        }

        client->subscribed = 1;

        struct json_object *subscribed = json_object_new_object();

        json_object_object_add(subscribed, "subscribed",
                               json_object_new_boolean(1));
        queued = queue_response(client, request, 1, subscribed, NULL, NULL);
        json_object_put(subscribed);
        goto done;
    }

    if (server->handler == NULL) {
        queued = send_error_response(
            client,
            request,
            "COMMAND_NOT_IMPLEMENTED",
            "Aucun gestionnaire de commandes n'est configure"
        );
        goto done;
    }

    /*
     * Valeurs par défaut, si le gestionnaire échoue sans préciser
     * pourquoi.
     */
    char error_code[IPC_SERVER_ERROR_CODE_SIZE] = "COMMAND_FAILED";
    char error_message[IPC_SERVER_ERROR_SIZE] =
        "La commande a echoue";

    int handled = server->handler(
        json_object_get_string(command),
        args,
        &result_object,
        error_code,
        sizeof(error_code),
        error_message,
        sizeof(error_message),
        server->handler_context
    );

    if (handled == 0) {
        queued = queue_response(
            client,
            request,
            1,
            result_object,
            NULL,
            NULL
        );
    } else {
        queued = send_error_response(
            client,
            request,
            error_code,
            error_message
        );
    }

done:
    if (result_object != NULL) {
        json_object_put(result_object);
    }

    json_object_put(request);

    return queued;
}

/*
 * Traite la première ligne complète du tampon d'entrée et conserve les
 * éventuelles lignes suivantes pour plus tard.
 *
 * Un '\r' final est toléré (clients qui envoient "\r\n").
 *
 * Retour :
 *   1 : une ligne a été traitée ;
 *   0 : pas encore de ligne complète ;
 *  -1 : erreur, le client doit être déconnecté.
 */
static int process_buffered_message(
    IpcServer *server,
    IpcClient *client)
{
    char *newline = memchr(
        client->input,
        '\n',
        client->input_length
    );

    if (newline == NULL) {
        return 0;
    }

    size_t line_length =
        (size_t)(newline - client->input);

    if (line_length > 0 &&
        client->input[line_length - 1] == '\r') {
        line_length--;
    }

    char message[IPC_SERVER_MAX_MESSAGE + 1];

    memcpy(message, client->input, line_length);
    message[line_length] = '\0';

    size_t consumed =
        (size_t)(newline - client->input) + 1;

    size_t remaining =
        client->input_length - consumed;

    memmove(
        client->input,
        client->input + consumed,
        remaining
    );

    client->input_length = remaining;

    if (process_message(
            server,
            client,
            message,
            line_length) != 0) {
        return -1;
    }

    return 1;
}

/* ------------------------------------------------------------------------- */
/* Entrées / sorties d'un client                                             */
/* ------------------------------------------------------------------------- */

/*
 * Lit ce qui est disponible sur le socket du client.
 *
 * Retour : 0, ou -1 si le client s'est déconnecté, a provoqué une
 * erreur, ou a envoyé une ligne plus longue que IPC_SERVER_MAX_MESSAGE.
 */
static int read_client(IpcClient *client)
{
    size_t limit = client->http
                   ? IPC_HTTP_MAX_REQUEST
                   : IPC_SERVER_MAX_MESSAGE;

    /* Tampon plein sans fin de message : message trop long, on coupe. */
    if (client->input_length >= limit) {
        return -1;
    }

    size_t capacity = limit - client->input_length;

    if (capacity > IPC_READ_SIZE) {
        capacity = IPC_READ_SIZE;
    }

    ssize_t count = recv(
        client->fd,
        client->input + client->input_length,
        capacity,
        0
    );

    if (count == 0) {
        return -1;
    }

    if (count < 0) {
        if (errno == EINTR ||
            errno == EAGAIN ||
            errno == EWOULDBLOCK) {
            return 0;
        }

        return -1;
    }

    client->input_length += (size_t)count;

    return 0;
}

/*
 * Envoie autant que possible de la réponse en attente.
 *
 * Si le socket est plein (EAGAIN), on s'arrête : la suite partira au
 * prochain tour de boucle. MSG_NOSIGNAL évite que l'écriture vers un
 * client disparu ne tue le service (SIGPIPE).
 *
 * Retour : 0, ou -1 si le client doit être déconnecté.
 */
static int write_client(IpcClient *client)
{
    while (client->output_offset < client->output_length) {
        ssize_t count = send(
            client->fd,
            client->output + client->output_offset,
            client->output_length - client->output_offset,
            MSG_NOSIGNAL
        );

        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }

            return -1;
        }

        if (count == 0) {
            return -1;
        }

        client->output_offset += (size_t)count;
    }

    /* Réponse entièrement partie : le client peut envoyer la suivante. */
    free(client->output);
    client->output = NULL;
    client->output_length = 0;
    client->output_offset = 0;

    /* Web : "Connection: close" ou erreur de transport. */
    if (client->close_after) {
        return -1;
    }

    if (client->http) {
        client->idle_since = monotonic_seconds();
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Web                                                                       */
/* ------------------------------------------------------------------------- */

/*
 * HTTP volontairement minimal : juste ce qu'il faut à la page de
 * contrôle et à un navigateur. Pas de fichiers, pas de chunked, pas de
 * HTTPS (confié à tailscale serve). Une requête = en-têtes, puis un
 * corps dont la taille est donnée par Content-Length.
 */

/*
 * Cherche un en-tête dans headers (texte terminé par '\0', première
 * ligne = ligne de requête). Retourne sa valeur, sans espaces autour,
 * et sa longueur dans *length ; NULL s'il est absent.
 */
static const char *http_header(
    const char *headers,
    const char *name,
    size_t *length)
{
    size_t name_length = strlen(name);
    const char *line = strstr(headers, "\r\n");

    while (line != NULL) {
        line += 2;

        if (strncasecmp(line, name, name_length) == 0 &&
            line[name_length] == ':') {
            const char *value = line + name_length + 1;

            while (*value == ' ' || *value == '\t') {
                value++;
            }

            const char *stop = strstr(value, "\r\n");
            size_t count = stop != NULL
                           ? (size_t)(stop - value)
                           : strlen(value);

            while (count > 0 &&
                   (value[count - 1] == ' ' || value[count - 1] == '\t')) {
                count--;
            }

            *length = count;
            return value;
        }

        line = strstr(line, "\r\n");
    }

    return NULL;
}

/* L'en-tête name vaut-il exactement value (casse ignorée) ? */
static int http_header_is(
    const char *headers,
    const char *name,
    const char *value)
{
    size_t length = 0;
    const char *found = http_header(headers, name, &length);

    return found != NULL &&
           length == strlen(value) &&
           strncasecmp(found, value, length) == 0;
}

/*
 * Protection contre les requêtes envoyées par un AUTRE site ouvert dans
 * le navigateur (CSRF, DNS rebinding) : si le navigateur indique une
 * origine, elle doit être ce serveur lui-même. Les clients hors
 * navigateur (curl, scripts) n'envoient pas d'Origin et passent.
 */
static int http_origin_allowed(const char *headers)
{
    size_t origin_length = 0;
    const char *origin = http_header(headers, "Origin", &origin_length);

    if (origin == NULL) {
        return 1;
    }

    /* "http://hote:port" -> "hote:port" */
    for (size_t i = 0; i + 3 <= origin_length; i++) {
        if (memcmp(origin + i, "://", 3) == 0) {
            origin += i + 3;
            origin_length -= i + 3;
            break;
        }
    }

    /* Derrière tailscale serve, l'hôte d'origine peut être dans
       X-Forwarded-Host plutôt que dans Host. */
    static const char *const NAMES[] = { "Host", "X-Forwarded-Host" };

    for (size_t i = 0; i < 2; i++) {
        size_t host_length = 0;
        const char *host = http_header(headers, NAMES[i], &host_length);

        if (host != NULL &&
            host_length == origin_length &&
            strncasecmp(host, origin, host_length) == 0) {
            return 1;
        }
    }

    return 0;
}

/* Nombre de flux /events ouverts. */
static size_t count_sse(const IpcServer *server)
{
    size_t count = 0;

    for (size_t i = 0; i < IPC_SERVER_MAX_CLIENTS; i++) {
        if (server->clients[i].fd >= 0 && server->clients[i].sse) {
            count++;
        }
    }

    return count;
}

/* GET / : la page embarquée, ou 304 si le navigateur l'a déjà. */
static int http_send_page(
    IpcServer *server,
    IpcClient *client,
    const char *headers)
{
    if (server->page == NULL) {
        return http_simple(client, "404 Not Found", 0);
    }

    char extra[160];

    snprintf(
        extra,
        sizeof(extra),
        "Content-Encoding: gzip\r\n"
        "Cache-Control: no-cache\r\n"
        "ETag: %s\r\n",
        server->page_etag
    );

    if (http_header_is(headers, "If-None-Match", server->page_etag)) {
        return http_queue(
            client,
            "304 Not Modified",
            "text/html; charset=utf-8",
            extra,
            NULL,
            0
        );
    }

    return http_queue(
        client,
        "200 OK",
        "text/html; charset=utf-8",
        extra,
        server->page,
        server->page_length
    );
}

/*
 * GET /events : la réponse ne se termine jamais. Le client devient un
 * abonné : ipc_server_broadcast() lui écrit chaque événement sous la
 * forme "data: {...}\n\n" (format Server-Sent Events).
 */
static int http_open_events(IpcServer *server, IpcClient *client)
{
    static const char HEAD[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-store\r\n"
        "X-Accel-Buffering: no\r\n"
        "\r\n"
        "retry: 3000\n\n";

    if (count_sse(server) >= IPC_HTTP_MAX_SSE) {
        return http_simple(client, "503 Service Unavailable", 1);
    }

    client->sse = 1;
    client->subscribed = 1;

    return client_append_raw(client, HEAD, sizeof(HEAD) - 1);
}

/*
 * Traite la première requête HTTP complète du tampon d'entrée.
 *
 * Retour : 1 si une requête a été traitée, 0 si elle n'est pas encore
 * complète, -1 si le client doit être déconnecté.
 */
static int http_process(IpcServer *server, IpcClient *client)
{
    size_t header_end = 0;

    for (size_t i = 0; i + 4 <= client->input_length; i++) {
        if (memcmp(client->input + i, "\r\n\r\n", 4) == 0) {
            header_end = i + 4;
            break;
        }
    }

    if (header_end == 0) {
        return 0;
    }

    /* Copie des en-têtes, terminée par '\0', pour les lire facilement. */
    char headers[IPC_HTTP_MAX_REQUEST + 1];

    memcpy(headers, client->input, header_end - 2);
    headers[header_end - 2] = '\0';

    char method[8] = "";
    char path[64] = "";
    char version[10] = "";

    if (sscanf(headers, "%7s %63s %9s", method, path, version) != 3 ||
        strncmp(version, "HTTP/1.", 7) != 0) {
        return http_simple(client, "400 Bad Request", 1) == 0 ? 1 : -1;
    }

    /* Pas de corps découpé (chunked) : inutile pour cette page. */
    if (http_header(headers, "Transfer-Encoding", &(size_t){0}) != NULL) {
        return http_simple(client, "411 Length Required", 1) == 0 ? 1 : -1;
    }

    size_t body_length = 0;
    size_t value_length = 0;
    const char *value = http_header(headers, "Content-Length", &value_length);

    if (value != NULL) {
        for (size_t i = 0; i < value_length; i++) {
            if (value[i] < '0' || value[i] > '9' ||
                body_length > IPC_HTTP_MAX_REQUEST) {
                return http_simple(client, "400 Bad Request", 1) == 0
                       ? 1 : -1;
            }

            body_length = body_length * 10 + (size_t)(value[i] - '0');
        }
    }

    if (body_length > IPC_HTTP_MAX_REQUEST - header_end) {
        return http_simple(client, "413 Content Too Large", 1) == 0
               ? 1 : -1;
    }

    if (client->input_length < header_end + body_length) {
        return 0;
    }

    /* Corps complet : on le copie puis on retire la requête du tampon. */
    char body[IPC_HTTP_MAX_REQUEST + 1];

    memcpy(body, client->input + header_end, body_length);
    body[body_length] = '\0';

    size_t consumed = header_end + body_length;

    memmove(
        client->input,
        client->input + consumed,
        client->input_length - consumed
    );
    client->input_length -= consumed;

    /* Connexion gardée ouverte en HTTP/1.1, sauf demande contraire. */
    if (strcmp(version, "HTTP/1.1") != 0 ||
        http_header_is(headers, "Connection", "close")) {
        client->close_after = 1;
    }

    char *query = strchr(path, '?');

    if (query != NULL) {
        *query = '\0';
    }

    int queued;

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        queued = strcmp(method, "GET") == 0
                 ? http_send_page(server, client, headers)
                 : http_simple(client, "405 Method Not Allowed", 0);
    } else if (strcmp(path, "/events") == 0) {
        queued = strcmp(method, "GET") == 0
                 ? http_open_events(server, client)
                 : http_simple(client, "405 Method Not Allowed", 0);
    } else if (strcmp(path, "/api") == 0) {
        size_t type_length = 0;
        const char *type = http_header(headers, "Content-Type", &type_length);

        if (strcmp(method, "POST") != 0) {
            queued = http_simple(client, "405 Method Not Allowed", 0);
        } else if (!http_origin_allowed(headers)) {
            queued = http_simple(client, "403 Forbidden", 0);
        } else if (type == NULL || type_length < 16 ||
                   strncasecmp(type, "application/json", 16) != 0) {
            /*
             * Exiger application/json oblige un autre site à demander
             * la permission au préalable (CORS), ce qu'on ne fait jamais.
             */
            queued = http_simple(client, "415 Unsupported Media Type", 0);
        } else {
            queued = process_message(server, client, body, body_length);
        }
    } else {
        queued = http_simple(client, "404 Not Found", 0);
    }

    return queued == 0 ? 1 : -1;
}

/*
 * Entretien des clients web, une fois par tour de boucle : fermeture des
 * connexions inactives, battement de cœur des flux /events.
 */
static void http_maintain(IpcServer *server, time_t *last_heartbeat)
{
    time_t now = monotonic_seconds();
    int heartbeat = now - *last_heartbeat >= IPC_HTTP_HEARTBEAT_S;

    if (heartbeat) {
        *last_heartbeat = now;
    }

    for (size_t i = 0; i < IPC_SERVER_MAX_CLIENTS; i++) {
        IpcClient *client = &server->clients[i];

        if (client->fd < 0 || !client->http) {
            continue;
        }

        if (client->sse) {
            if (heartbeat &&
                client_append_raw(client, ": ping\n\n", 8) != 0) {
                client_reset(client);
            }
        } else if (client->output_length == 0 &&
                   now - client->idle_since > IPC_HTTP_IDLE_S) {
            client_reset(client);
        }
    }
}

/*
 * Accepte tous les clients en attente sur listen_fd.
 * Retour : 0, ou -1 si le socket d'écoute est hors d'usage.
 */
static int accept_clients(IpcClient *clients, int listen_fd, int http)
{
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == EMFILE || errno == ENFILE) {
                return 0;
            }

            return -1;
        }

        if (set_nonblocking(fd) != 0) {
            close(fd);
            continue;
        }

        size_t slot;

        for (slot = 0; slot < IPC_SERVER_MAX_CLIENTS; slot++) {
            if (clients[slot].fd < 0) {
                clients[slot].fd = fd;
                clients[slot].http = http;
                clients[slot].idle_since = monotonic_seconds();
                break;
            }
        }

        /* Plus de place : le client est refusé. */
        if (slot == IPC_SERVER_MAX_CLIENTS) {
            close(fd);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* API publique                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Crée le socket d'écoute, le rend accessible au propriétaire et à son
 * groupe uniquement (0660), puis installe les gestionnaires de SIGINT
 * et SIGTERM.
 */
int ipc_server_init(
    IpcServer *server,
    const char *socket_path,
    char *error_message,
    size_t error_capacity)
{
    struct sockaddr_un address;
    struct sigaction action;

    if (server == NULL ||
        socket_path == NULL ||
        socket_path[0] == '\0') {
        set_error(
            error_message,
            error_capacity,
            "Arguments invalides"
        );
        return -1;
    }

    memset(server, 0, sizeof(*server));
    server->listen_fd = -1;
    server->http_fd = -1;

    if (strlen(socket_path) >= sizeof(server->socket_path) ||
        strlen(socket_path) >= sizeof(address.sun_path)) {
        set_error(
            error_message,
            error_capacity,
            "Chemin du socket trop long"
        );
        return -1;
    }

    snprintf(
        server->socket_path,
        sizeof(server->socket_path),
        "%s",
        socket_path
    );

    server->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (server->listen_fd < 0) {
        set_error(error_message, error_capacity, strerror(errno));
        return -1;
    }

    if (set_nonblocking(server->listen_fd) != 0) {
        set_error(error_message, error_capacity, strerror(errno));
        ipc_server_cleanup(server);
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;

    snprintf(
        address.sun_path,
        sizeof(address.sun_path),
        "%s",
        socket_path
    );

    /*
     * Ne supprime jamais un socket qui répond : un échec de bind ne
     * donne aucun droit de le supprimer. Seul un socket orphelin, laissé
     * par un arrêt brutal (kill -9, coupure de courant), est remplacé ;
     * sans cela le service ne pourrait plus redémarrer seul.
     */
    if (bind(
            server->listen_fd,
            (struct sockaddr *)&address,
            sizeof(address)) != 0) {
        int bind_errno = errno;

        if (bind_errno == EADDRINUSE &&
            socket_is_stale(socket_path) &&
            unlink(socket_path) == 0 &&
            bind(
                server->listen_fd,
                (struct sockaddr *)&address,
                sizeof(address)) == 0) {
            fprintf(
                stderr,
                "IPC : ancien socket orphelin remplace : %s\n",
                socket_path
            );
        } else {
            set_error(
                error_message,
                error_capacity,
                strerror(bind_errno)
            );
            ipc_server_cleanup(server);
            return -1;
        }
    }

    server->owns_socket = 1;

    if (chmod(socket_path, 0660) != 0 ||
        listen(
            server->listen_fd,
            IPC_SERVER_MAX_CLIENTS) != 0) {
        set_error(error_message, error_capacity, strerror(errno));
        ipc_server_cleanup(server);
        return -1;
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        set_error(error_message, error_capacity, strerror(errno));
        ipc_server_cleanup(server);
        return -1;
    }

    stop_requested = 0;
    server->running = 1;

    return 0;
}

void ipc_server_set_handler(
    IpcServer *server,
    IpcRequestHandler handler,
    void *context)
{
    if (server == NULL) {
        return;
    }

    server->handler = handler;
    server->handler_context = context;
}

/* FNV-1a 64 bits : empreinte de la page pour l'en-tête ETag. */
static uint64_t fnv1a(const unsigned char *data, size_t length)
{
    uint64_t hash = 0xcbf29ce484222325ULL;

    for (size_t i = 0; i < length; i++) {
        hash ^= data[i];
        hash *= 0x100000001b3ULL;
    }

    return hash;
}

void ipc_server_set_page(
    IpcServer *server,
    const unsigned char *gzip_page,
    size_t length)
{
    if (server == NULL) {
        return;
    }

    server->page = gzip_page;
    server->page_length = gzip_page != NULL ? length : 0;

    snprintf(
        server->page_etag,
        sizeof(server->page_etag),
        "\"%016llx\"",
        (unsigned long long)fnv1a(gzip_page, server->page_length)
    );
}

/*
 * Ouvre le socket TCP du web. address : "port" (toutes les interfaces,
 * IPv4 et IPv6) ou "hote:port" ; "[::1]:port" pour une adresse IPv6.
 */
int ipc_server_listen_http(
    IpcServer *server,
    const char *address,
    char *error_message,
    size_t error_capacity)
{
    char buffer[128];
    const char *host = NULL;
    const char *port;

    if (server == NULL || address == NULL || address[0] == '\0' ||
        strlen(address) >= sizeof(buffer)) {
        set_error(error_message, error_capacity, "Adresse web invalide");
        return -1;
    }

    snprintf(buffer, sizeof(buffer), "%s", address);

    char *colon = strrchr(buffer, ':');

    if (colon == NULL) {
        port = buffer;
    } else {
        *colon = '\0';
        port = colon + 1;
        host = buffer;

        size_t host_length = strlen(buffer);

        if (host_length >= 2 && buffer[0] == '[' &&
            buffer[host_length - 1] == ']') {
            buffer[host_length - 1] = '\0';
            host = buffer + 1;
        }

        if (host[0] == '\0') {
            host = NULL;
        }
    }

    struct addrinfo hints;
    struct addrinfo *results = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    int code = getaddrinfo(host, port, &hints, &results);

    if (code != 0) {
        set_error(error_message, error_capacity, gai_strerror(code));
        return -1;
    }

    int last_errno = 0;

    /*
     * Sans hôte : IPv6 d'abord, en double pile (IPv4 accepté aussi) ;
     * IPv4 seul si l'IPv6 est indisponible.
     */
    for (int pass = 0; pass < 2 && server->http_fd < 0; pass++) {
        for (struct addrinfo *ai = results;
             ai != NULL && server->http_fd < 0;
             ai = ai->ai_next) {
            if ((pass == 0) != (ai->ai_family == AF_INET6)) {
                continue;
            }

            int fd = socket(ai->ai_family, ai->ai_socktype,
                            ai->ai_protocol);

            if (fd < 0) {
                last_errno = errno;
                continue;
            }

            int yes = 1;
            int no = 0;

            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

            if (ai->ai_family == AF_INET6 && host == NULL) {
                setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
            }

            if (bind(fd, ai->ai_addr, ai->ai_addrlen) != 0 ||
                listen(fd, IPC_SERVER_MAX_CLIENTS) != 0 ||
                set_nonblocking(fd) != 0) {
                last_errno = errno;
                close(fd);
                continue;
            }

            server->http_fd = fd;
        }
    }

    freeaddrinfo(results);

    if (server->http_fd < 0) {
        set_error(
            error_message,
            error_capacity,
            last_errno == EACCES
                ? "Permission refusee (port < 1024 : voir "
                  "AmbientCapabilities dans le service systemd)"
                : strerror(last_errno)
        );
        return -1;
    }

    return 0;
}

void ipc_server_set_tick(
    IpcServer *server,
    IpcTickHandler tick,
    void *context)
{
    if (server == NULL) {
        return;
    }

    server->tick = tick;
    server->tick_context = context;
}

/*
 * Boucle principale. Chaque tour :
 *
 *   1. appelle la tâche périodique (entretien du Bluetooth) ;
 *   2. traite les requêtes déjà reçues par les clients qui n'ont pas de
 *      réponse en attente ;
 *   3. attend (1 s au plus) qu'un socket soit prêt :
 *        - le socket d'écoute : nouveaux clients à accepter ;
 *        - un client sans réponse en attente : on surveille la lecture ;
 *        - un client avec une réponse en attente : on surveille l'écriture ;
 *   4. lit, écrit ou déconnecte les clients concernés.
 *
 * Retourne 0 après une demande d'arrêt (signal ou ipc_server_stop()).
 */
int ipc_server_run(IpcServer *server)
{
    /* static : 32 clients x 8 Ko, trop pour la pile. Une seule boucle
       tourne à la fois dans le service. */
    static IpcClient clients[IPC_SERVER_MAX_CLIENTS];
    struct pollfd poll_fds[IPC_SERVER_MAX_CLIENTS + 2];
    time_t last_heartbeat = monotonic_seconds();

    if (server == NULL || server->listen_fd < 0) {
        return -1;
    }

    for (size_t i = 0; i < IPC_SERVER_MAX_CLIENTS; i++) {
        memset(&clients[i], 0, sizeof(clients[i]));
        clients[i].fd = -1;
    }

    /* Rend les clients accessibles à ipc_server_broadcast(). */
    server->clients = clients;

    while (server->running && !stop_requested) {
        /* Tâches périodiques du service, avant d'attendre les clients. */
        if (server->tick != NULL) {
            server->tick(server->tick_context);

            if (!server->running || stop_requested) {
                break;
            }
        }

        /*
         * Traite les lignes déjà reçues dès qu'aucune réponse
         * n'attend d'être envoyée.
         */
        for (size_t i = 0; i < IPC_SERVER_MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0 &&
                clients[i].output_length == 0) {
                int processed = clients[i].http
                    ? http_process(server, &clients[i])
                    : process_buffered_message(server, &clients[i]);

                if (processed < 0) {
                    client_reset(&clients[i]);
                }
            }
        }

        if (server->http_fd >= 0) {
            http_maintain(server, &last_heartbeat);
        }

        nfds_t count = 1;

        poll_fds[0].fd = server->listen_fd;
        poll_fds[0].events = POLLIN;
        poll_fds[0].revents = 0;

        /* Le socket web, s'il existe, suit toujours celui de l'IPC. */
        nfds_t first_client = 1;

        if (server->http_fd >= 0) {
            poll_fds[1].fd = server->http_fd;
            poll_fds[1].events = POLLIN;
            poll_fds[1].revents = 0;
            count = first_client = 2;
        }

        for (size_t i = 0; i < IPC_SERVER_MAX_CLIENTS; i++) {
            if (clients[i].fd < 0) {
                continue;
            }

            poll_fds[count].fd = clients[i].fd;
            poll_fds[count].events =
                clients[i].output_length > 0 ? POLLOUT : POLLIN;
            poll_fds[count].revents = 0;
            count++;
        }

        int ready = poll(
            poll_fds,
            count,
            IPC_POLL_TIMEOUT_MS
        );

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }

            server->running = 0;
            break;
        }

        if (ready == 0) {
            continue;
        }

        /* Accepter tous les nouveaux clients en attente. */
        if ((poll_fds[0].revents & POLLIN) &&
            accept_clients(clients, server->listen_fd, 0) != 0) {
            server->running = 0;
            break;
        }

        /* Le web en panne n'arrête pas le service : on le ferme seulement. */
        if (first_client == 2 && (poll_fds[1].revents & POLLIN) &&
            accept_clients(clients, server->http_fd, 1) != 0) {
            fprintf(stderr, "Web : socket d'ecoute hors d'usage, ferme.\n");
            close(server->http_fd);
            server->http_fd = -1;
        }

        /* Servir chaque client signalé par poll(). */
        for (nfds_t p = first_client; p < count; p++) {
            size_t slot;

            for (slot = 0;
                 slot < IPC_SERVER_MAX_CLIENTS;
                 slot++) {
                if (clients[slot].fd == poll_fds[p].fd) {
                    break;
                }
            }

            if (slot == IPC_SERVER_MAX_CLIENTS) {
                continue;
            }

            short events = poll_fds[p].revents;

            if (events & (POLLERR | POLLNVAL)) {
                client_reset(&clients[slot]);
                continue;
            }

            if ((events & POLLIN) &&
                clients[slot].output_length == 0) {
                if (read_client(&clients[slot]) != 0) {
                    client_reset(&clients[slot]);
                    continue;
                }
            }

            if ((events & POLLOUT) &&
                clients[slot].output_length > 0) {
                if (write_client(&clients[slot]) != 0) {
                    client_reset(&clients[slot]);
                    continue;
                }
            }

            /* Le client a raccroché et il n'y a plus rien à lire. */
            if ((events & POLLHUP) && !(events & POLLIN)) {
                client_reset(&clients[slot]);
            }
        }
    }

    for (size_t i = 0; i < IPC_SERVER_MAX_CLIENTS; i++) {
        client_reset(&clients[i]);
    }

    server->clients = NULL;
    server->running = 0;

    return 0;
}

/*
 * Ajoute l'événement à la file de chaque client abonné. Il partira au
 * prochain tour de boucle, quand le socket du client sera prêt.
 */
int ipc_server_broadcast(IpcServer *server, struct json_object *event)
{
    if (server == NULL || event == NULL || server->clients == NULL) {
        return -1;
    }

    const char *serialized = json_object_to_json_string_ext(
        event,
        JSON_C_TO_STRING_PLAIN
    );

    if (serialized == NULL) {
        return -1;
    }

    size_t length = strlen(serialized);

    for (size_t i = 0; i < IPC_SERVER_MAX_CLIENTS; i++) {
        IpcClient *client = &server->clients[i];

        if (client->fd < 0 || !client->subscribed) {
            continue;
        }

        int queued = client->sse
            ? (client_append_raw(client, "data: ", 6) != 0 ||
               client_append_raw(client, serialized, length) != 0 ||
               client_append_raw(client, "\n\n", 2) != 0) ? -1 : 0
            : client_append(client, serialized, length);

        if (queued != 0) {
            client_reset(client);
        }
    }

    return 0;
}

void ipc_server_stop(IpcServer *server)
{
    if (server != NULL) {
        server->running = 0;
    }
}

void ipc_server_cleanup(IpcServer *server)
{
    if (server == NULL) {
        return;
    }

    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }

    if (server->http_fd >= 0) {
        close(server->http_fd);
        server->http_fd = -1;
    }

    if (server->owns_socket &&
        server->socket_path[0] != '\0') {
        unlink(server->socket_path);
    }

    server->owns_socket = 0;
    server->socket_path[0] = '\0';
    server->running = 0;
}