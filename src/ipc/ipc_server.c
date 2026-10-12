/*
 * ipc/ipc_server.c — Serveur local sur socket Unix
 *
 * Organisation du fichier :
 *   1. Outils         : signaux, erreurs, socket orphelin, mode non bloquant.
 *   2. Réponses       : mise en file d'une réponse JSON pour un client.
 *   3. Requêtes       : validation de l'enveloppe, appel du gestionnaire.
 *   4. Entrées/sorties: lecture et écriture non bloquantes d'un client.
 *   5. API publique   : init, set_handler, set_tick, run, stop, cleanup.
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
 */

#define _POSIX_C_SOURCE 200809L

#include "ipc/ipc_server.h"
#include "ipc/ipc_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/*
 * Attente maximale de poll(). Elle garantit que la tâche périodique
 * (tick) tourne au moins une fois par seconde, même sans client.
 */
#define IPC_POLL_TIMEOUT_MS 1000

/* Nombre d'octets lus au plus à chaque recv(). */
#define IPC_READ_SIZE 1024

/*
 * État d'un client connecté.
 *
 * input  : octets reçus pas encore traités (une ligne partielle, ou
 *          plusieurs lignes arrivées d'un coup).
 * output : réponse en cours d'envoi, allouée à sa taille exacte (la
 *          liste des commandes, par exemple, dépasse largement une
 *          requête) ; output_offset indique ce qui est déjà parti.
 *          output_length == 0 : aucune réponse en attente.
 */
typedef struct {
    int fd;            /* -1 : emplacement libre */

    char input[IPC_SERVER_MAX_MESSAGE + 1];
    size_t input_length;

    char *output;
    size_t output_length;
    size_t output_offset;
} IpcClient;

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

    size_t length = strlen(serialized);
    char *output = malloc(length + 1);

    if (output == NULL) {
        json_object_put(response);
        return -1;
    }

    free(client->output);
    client->output = output;

    memcpy(client->output, serialized, length);
    client->output[length] = '\n';
    client->output_length = length + 1;
    client->output_offset = 0;

    json_object_put(response);

    return 0;
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
    /* Tampon plein sans '\n' : message trop long, on coupe. */
    if (client->input_length >= sizeof(client->input) - 1) {
        return -1;
    }

    size_t capacity =
        sizeof(client->input) - 1 - client->input_length;

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

    return 0;
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
    IpcClient clients[IPC_SERVER_MAX_CLIENTS];
    struct pollfd poll_fds[IPC_SERVER_MAX_CLIENTS + 1];

    if (server == NULL || server->listen_fd < 0) {
        return -1;
    }

    for (size_t i = 0; i < IPC_SERVER_MAX_CLIENTS; i++) {
        memset(&clients[i], 0, sizeof(clients[i]));
        clients[i].fd = -1;
    }

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
                int processed =
                    process_buffered_message(server, &clients[i]);

                if (processed < 0) {
                    client_reset(&clients[i]);
                }
            }
        }

        nfds_t count = 1;

        poll_fds[0].fd = server->listen_fd;
        poll_fds[0].events = POLLIN;
        poll_fds[0].revents = 0;

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
        if (poll_fds[0].revents & POLLIN) {
            for (;;) {
                int fd = accept(server->listen_fd, NULL, NULL);

                if (fd < 0) {
                    if (errno == EINTR) {
                        continue;
                    }

                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }

                    server->running = 0;
                    break;
                }

                if (set_nonblocking(fd) != 0) {
                    close(fd);
                    continue;
                }

                size_t slot;

                for (slot = 0;
                     slot < IPC_SERVER_MAX_CLIENTS;
                     slot++) {
                    if (clients[slot].fd < 0) {
                        clients[slot].fd = fd;
                        break;
                    }
                }

                /* Plus de place : le client est refusé. */
                if (slot == IPC_SERVER_MAX_CLIENTS) {
                    close(fd);
                }
            }
        }

        /* Servir chaque client signalé par poll(). */
        for (nfds_t p = 1; p < count; p++) {
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

    server->running = 0;

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

    if (server->owns_socket &&
        server->socket_path[0] != '\0') {
        unlink(server->socket_path);
    }

    server->owns_socket = 0;
    server->socket_path[0] = '\0';
    server->running = 0;
}