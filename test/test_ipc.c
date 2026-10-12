/*
 * test/test_ipc.c — Tests du serveur IPC (ipc/ipc_server)
 *
 * Le serveur tourne dans un processus enfant (fork) ; le test joue le
 * client à travers un vrai socket Unix dans /tmp. Vérifie notamment :
 *   - le remplacement d'un socket orphelin, mais jamais d'un socket
 *     actif ni d'un fichier ordinaire ;
 *   - l'appel régulier de la tâche périodique ;
 *   - plusieurs requêtes dans un seul envoi, une requête en plusieurs
 *     morceaux, et le rejet propre d'un JSON invalide.
 */

#define _POSIX_C_SOURCE 200809L

#include "ipc/ipc_server.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_SOCKET "/tmp/sweetdawn-test-ipc.sock"

static int connect_client(const char *path)
{
    struct sockaddr_un address;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;

    snprintf(
        address.sun_path,
        sizeof(address.sun_path),
        "%s",
        path);

    if (connect(
            fd,
            (struct sockaddr *)&address,
            sizeof(address)) != 0) {

        close(fd);
        return -1;
    }

    return fd;
}

static int read_line(int fd, char *buffer, size_t capacity)
{
    size_t used = 0;

    while (used + 1 < capacity) {
        char c;

        ssize_t n = read(fd, &c, 1);

        if (n == 1) {
            if (c == '\n') {
                buffer[used] = '\0';
                return 0;
            }

            buffer[used++] = c;

        } else if (n < 0 && errno == EINTR) {
            continue;

        } else {
            return -1;
        }
    }

    return -1;
}

static int response_has(
    const char *response,
    const char *id,
    const char *error_code)
{
    char id_fragment[64];

    snprintf(
        id_fragment,
        sizeof(id_fragment),
        "\"id\":%s",
        id);

    return
        strstr(response, "\"type\":\"response\"") != NULL &&
        strstr(response, id_fragment) != NULL &&
        strstr(response, error_code) != NULL;
}

static int check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "ECHEC: %s\n", message);
        return -1;
    }

    printf("OK: %s\n", message);

    return 0;
}

/*
 * Tests de la robustesse du serveur : socket orphelin et tâche périodique.
 */

#define STALE_SOCKET "/tmp/sweetdawn-test-stale.sock"

/* Crée un socket Unix puis l'abandonne sans le supprimer, comme un crash. */
static int create_orphan_socket(const char *path)
{
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int test_socket_orphelin(void)
{
    IpcServer server;
    IpcServer other;
    char error[IPC_SERVER_ERROR_SIZE] = {0};
    int failures = 0;

    unlink(STALE_SOCKET);

    /* 1. Un socket orphelin est remplace : le service peut redemarrer. */
    failures += check(
        create_orphan_socket(STALE_SOCKET) == 0,
        "creation d'un socket orphelin") != 0;

    int result = ipc_server_init(&server, STALE_SOCKET, error, sizeof(error));

    failures += check(
        result == 0,
        "un socket orphelin ne bloque pas le demarrage") != 0;

    if (result == 0) {
        /* 2. Un socket vivant n'est jamais supprime par un second serveur. */
        int second = ipc_server_init(
            &other, STALE_SOCKET, error, sizeof(error));

        failures += check(
            second != 0,
            "un second serveur est refuse si le socket est actif") != 0;

        int fd = connect_client(STALE_SOCKET);

        failures += check(
            fd >= 0,
            "le socket actif est reste utilisable") != 0;

        if (fd >= 0) {
            close(fd);
        }

        ipc_server_cleanup(&server);
    }

    /* 3. Un fichier ordinaire n'est jamais supprime. */
    unlink(STALE_SOCKET);

    FILE *file = fopen(STALE_SOCKET, "w");

    if (file != NULL) {
        fputs("important\n", file);
        fclose(file);

        int refused = ipc_server_init(
            &server, STALE_SOCKET, error, sizeof(error));

        failures += check(
            refused != 0,
            "un fichier ordinaire est refuse") != 0;

        failures += check(
            access(STALE_SOCKET, F_OK) == 0,
            "et il n'a pas ete supprime") != 0;
    } else {
        failures++;
    }

    unlink(STALE_SOCKET);

    return failures;
}

static int tick_count = 0;

static void count_tick(void *context)
{
    IpcServer *server = context;

    tick_count++;

    if (tick_count >= 2) {
        ipc_server_stop(server);
    }
}

static int test_tick(void)
{
    IpcServer server;
    char error[IPC_SERVER_ERROR_SIZE] = {0};
    int failures = 0;

    unlink(STALE_SOCKET);

    if (ipc_server_init(&server, STALE_SOCKET, error, sizeof(error)) != 0) {
        fprintf(stderr, "ECHEC: init pour le test du tick : %s\n", error);
        return 1;
    }

    tick_count = 0;
    ipc_server_set_tick(&server, count_tick, &server);

    int result = ipc_server_run(&server);

    failures += check(
        result == 0 && tick_count == 2,
        "le tick est appele a chaque tour, meme sans client") != 0;

    ipc_server_cleanup(&server);

    return failures;
}

int main(void)
{
    int early_failures = test_socket_orphelin() + test_tick();

    unlink(TEST_SOCKET);

    pid_t child = fork();

    if (child < 0) {
        perror("fork");
        return 1;
    }

    if (child == 0) {
        IpcServer server;
        char error[IPC_SERVER_ERROR_SIZE] = {0};

        if (ipc_server_init(
                &server,
                TEST_SOCKET,
                error,
                sizeof(error)) != 0) {

            fprintf(stderr, "Initialisation IPC : %s\n", error);
            _exit(2);
        }

        int result = ipc_server_run(&server);

        ipc_server_cleanup(&server);

        _exit(result == 0 ? 0 : 3);
    }

    int fd = -1;

    for (int attempt = 0; attempt < 100; attempt++) {
        fd = connect_client(TEST_SOCKET);

        if (fd >= 0) {
            break;
        }

        struct timespec delay = {
            .tv_sec = 0,
            .tv_nsec = 20000000
        };

        nanosleep(&delay, NULL);
    }

    if (fd < 0) {
        fprintf(stderr, "Impossible de se connecter au serveur IPC\n");

        kill(child, SIGTERM);
        waitpid(child, NULL, 0);

        return 1;
    }

    int failures = early_failures;

    /*
     * Deux requêtes dans le même envoi.
     * Les deux réponses doivent être reçues dans le bon ordre.
     */
    const char *two_requests =
        "{\"type\":\"request\",\"id\":1,"
        "\"command\":\"status\",\"args\":[]}\n"
        "{\"type\":\"request\",\"id\":2,"
        "\"command\":\"settings\",\"args\":[]}\n";

    if (write(
            fd,
            two_requests,
            strlen(two_requests)) != (ssize_t)strlen(two_requests)) {

        failures++;
        fprintf(stderr, "ECHEC: envoi de deux requetes\n");

    } else {
        char line[8192];

        if (read_line(fd, line, sizeof(line)) != 0 ||
            !response_has(
                line,
                "1",
                "COMMAND_NOT_IMPLEMENTED")) {

            failures++;
            fprintf(stderr, "ECHEC: premiere reponse a deux requetes\n");

        } else {
            failures += check(
                1,
                "deux requetes dans un seul envoi") != 0;
        }

        if (read_line(fd, line, sizeof(line)) != 0 ||
            !response_has(
                line,
                "2",
                "COMMAND_NOT_IMPLEMENTED")) {

            failures++;
            fprintf(stderr, "ECHEC: seconde reponse a deux requetes\n");

        } else {
            failures += check(
                1,
                "seconde requete conservee et traitee") != 0;
        }
    }

    /*
     * Une requête envoyée en deux fragments.
     */
    const char *part1 =
        "{\"type\":\"request\",\"id\":3,";

    const char *part2 =
        "\"command\":\"status\",\"args\":[]}\n";

    if (write(fd, part1, strlen(part1)) != (ssize_t)strlen(part1)) {
        failures++;
    }

    struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = 50000000
    };

    nanosleep(&delay, NULL);

    if (write(fd, part2, strlen(part2)) != (ssize_t)strlen(part2)) {
        failures++;
    }

    char line[8192];

    if (read_line(fd, line, sizeof(line)) != 0 ||
        !response_has(
            line,
            "3",
            "COMMAND_NOT_IMPLEMENTED")) {

        failures++;
        fprintf(stderr, "ECHEC: requete fragmentee\n");

    } else {
        failures += check(
            1,
            "requete fragmentee sur plusieurs envois") != 0;
    }

    /*
     * Un message JSON invalide doit recevoir une erreur explicite.
     */
    const char *invalid = "{pas-du-json}\n";

    if (write(fd, invalid, strlen(invalid)) !=
            (ssize_t)strlen(invalid) ||
        read_line(fd, line, sizeof(line)) != 0 ||
        !response_has(line, "null", "INVALID_JSON")) {

        failures++;
        fprintf(stderr, "ECHEC: rejet JSON invalide\n");

    } else {
        failures += check(
            1,
            "JSON invalide rejete proprement") != 0;
    }

    close(fd);

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);

    unlink(TEST_SOCKET);

    if (failures != 0) {
        fprintf(stderr, "%d test(s) en echec\n", failures);
        return 1;
    }

    printf("Tous les tests IPC sont passes.\n");

    return 0;
}