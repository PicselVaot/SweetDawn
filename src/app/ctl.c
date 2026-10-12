/*
 * app/ctl.c — Programme bin/sweetdawn-ctl : piloter l'AML depuis le terminal
 *
 * Envoie une commande au service (bin/sweetdawn-service) et affiche sa
 * réponse. Le service garde la liaison Bluetooth ouverte : la commande
 * s'exécute tout de suite, sans reconnexion.
 *
 * Usage : sweetdawn-ctl <commande> [valeur]
 *
 *   sweetdawn-ctl status
 *   sweetdawn-ctl Light                     lire l'état de la lampe
 *   sweetdawn-ctl Light/Enable true         allumer
 *   sweetdawn-ctl "Alarm/Alarm 1/Time" 07:30
 *   sweetdawn-ctl commands                  toutes les commandes
 *
 * Les noms qui contiennent un espace doivent être entre guillemets.
 * Le socket est celui par défaut (ipc/ipc_server.h), ou celui de la
 * variable d'environnement SWEETDAWN_SOCKET.
 *
 * Code de sortie : 0 succès, 1 commande refusée ou en échec,
 * 2 usage incorrect ou service injoignable.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <json-c/json.h>

#include "ipc/ipc_server.h"

/*
 * Attente maximale de la réponse : une recherche de station radio
 * peut à elle seule prendre 15 s côté service.
 */
#define CTL_TIMEOUT_S 20

/* Ouvre une connexion vers le socket du service, ou retourne -1. */
static int connecter(const char *path)
{
    struct sockaddr_un address;
    struct timeval timeout = { .tv_sec = CTL_TIMEOUT_S };
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return fd;
}

/* Envoie tout le texte, ou retourne -1. */
static int envoyer_tout(int fd, const char *text, size_t length)
{
    while (length > 0) {
        ssize_t count = send(fd, text, length, MSG_NOSIGNAL);

        if (count < 0 && errno == EINTR)
            continue;

        if (count <= 0)
            return -1;

        text += count;
        length -= (size_t)count;
    }

    return 0;
}

/* Lit une ligne de réponse (sans le '\n'), ou retourne -1. */
static int lire_ligne(int fd, char *buffer, size_t capacity)
{
    size_t used = 0;

    while (used + 1 < capacity) {
        ssize_t count = recv(fd, buffer + used, capacity - 1 - used, 0);

        if (count < 0 && errno == EINTR)
            continue;

        if (count <= 0)
            return -1;

        used += (size_t)count;

        char *newline = memchr(buffer, '\n', used);

        if (newline != NULL) {
            *newline = '\0';
            return 0;
        }
    }

    return -1;
}

/* Affiche le résultat d'une réponse réussie. */
static void afficher_resultat(struct json_object *result)
{
    if (result == NULL ||
        (json_object_is_type(result, json_type_object) &&
         json_object_object_length(result) == 0)) {
        printf("OK\n");
        return;
    }

    printf("%s\n", json_object_to_json_string_ext(
        result, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED));
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3 ||
        strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        fprintf(stderr,
                "Usage : %s <commande> [valeur]\n"
                "Exemples :\n"
                "  %s status\n"
                "  %s Light/Enable true\n"
                "  %s \"Alarm/Alarm 1/Time\" 07:30\n"
                "  %s commands\n",
                argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }

    const char *path = getenv(IPC_SOCKET_ENV);

    if (path == NULL || path[0] == '\0')
        path = IPC_DEFAULT_SOCKET;

    /* La valeur part en texte : le service la valide comme telle. */
    struct json_object *request = json_object_new_object();
    struct json_object *args = json_object_new_array();

    if (argc == 3)
        json_object_array_add(args, json_object_new_string(argv[2]));

    json_object_object_add(request, "type", json_object_new_string("request"));
    json_object_object_add(request, "id", json_object_new_int(1));
    json_object_object_add(request, "command", json_object_new_string(argv[1]));
    json_object_object_add(request, "args", args);

    char message[IPC_SERVER_MAX_MESSAGE + 2];
    int length = snprintf(message, sizeof(message), "%s\n",
                          json_object_to_json_string_ext(
                              request, JSON_C_TO_STRING_PLAIN));

    json_object_put(request);

    if (length < 0 || (size_t)length >= sizeof(message)) {
        fprintf(stderr, "Commande trop longue.\n");
        return 2;
    }

    int fd = connecter(path);

    if (fd < 0) {
        fprintf(stderr,
                "Service injoignable sur %s : %s\n"
                "Le service tourne-t-il ? (systemctl status sweetdawn)\n",
                path, strerror(errno));
        return 2;
    }

    char line[IPC_SERVER_MAX_MESSAGE + 1024];

    if (envoyer_tout(fd, message, (size_t)length) != 0 ||
        lire_ligne(fd, line, sizeof(line)) != 0) {
        fprintf(stderr, "Pas de réponse du service.\n");
        close(fd);
        return 2;
    }

    close(fd);

    struct json_object *response = json_tokener_parse(line);
    struct json_object *ok = NULL;
    struct json_object *result = NULL;
    struct json_object *error = NULL;
    int exit_code = 1;

    if (response == NULL || !json_object_object_get_ex(response, "ok", &ok)) {
        fprintf(stderr, "Réponse illisible : %s\n", line);
    } else if (json_object_get_boolean(ok)) {
        json_object_object_get_ex(response, "result", &result);
        afficher_resultat(result);
        exit_code = 0;
    } else {
        struct json_object *code = NULL;
        struct json_object *text = NULL;

        json_object_object_get_ex(response, "error", &error);
        json_object_object_get_ex(error, "code", &code);
        json_object_object_get_ex(error, "message", &text);

        fprintf(stderr, "Erreur %s : %s\n",
                code != NULL ? json_object_get_string(code) : "?",
                text != NULL ? json_object_get_string(text) : "");
    }

    if (response != NULL)
        json_object_put(response);

    return exit_code;
}
