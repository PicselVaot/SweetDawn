/*
 * aml/transport.c — Transport Bluetooth RFCOMM pour l'AML005
 *
 * Ce module ne connaît pas la signification des commandes.
 * Il assure uniquement le transport des octets entre le programme
 * et l'AML005.
 *
 * La construction des requêtes est effectuée par aml/protocol.c.
 * Les données reçues sont analysées par le parseur du protocole.
 */

#define _POSIX_C_SOURCE 200809L

#include "aml/transport.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>

/* Taille du bloc lu à chaque appel de recv(). */
#define AML_RX_BUFFER_SIZE 256

/*
 * Millisecondes sur l'horloge monotone.
 *
 * L'horloge murale n'est pas utilisée : sur un Raspberry Pi sans
 * horloge matérielle, une synchronisation NTP peut la faire sauter
 * et fausser les délais d'attente.
 */
static long long monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;

    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

/* Indique si errno décrit une liaison fermée ou perdue. */
static int errno_means_disconnected(int error)
{
    return error == EPIPE ||
           error == ECONNRESET ||
           error == ENOTCONN ||
           error == ECONNABORTED ||
           error == ETIMEDOUT ||
           error == EHOSTDOWN ||
           error == EHOSTUNREACH;
}

/*
 * connect() borné dans le temps.
 *
 * Le socket passe brièvement en mode non bloquant : sans cela,
 * connect() vers un appareil éteint peut attendre très longtemps.
 * Le mode d'origine est rétabli avant le retour.
 *
 * errno est positionné en cas d'échec (ETIMEDOUT si délai dépassé).
 */
static int connect_with_timeout(
    int socket_fd,
    const struct sockaddr *address,
    socklen_t address_length,
    int timeout_ms)
{
    int flags = fcntl(socket_fd, F_GETFL, 0);

    if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;

    int result = connect(socket_fd, address, address_length);

    if (result < 0 && errno == EINPROGRESS) {
        struct pollfd pfd = { .fd = socket_fd, .events = POLLOUT };
        int ready;

        do {
            ready = poll(&pfd, 1, timeout_ms);
        } while (ready < 0 && errno == EINTR);

        if (ready == 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        if (ready < 0)
            return -1;

        /* La connexion est terminée : lire son résultat réel. */
        int error = 0;
        socklen_t error_length = sizeof(error);

        if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR,
                       &error, &error_length) < 0)
            return -1;

        if (error != 0) {
            errno = error;
            return -1;
        }

        result = 0;
    }

    if (result < 0)
        return -1;

    return fcntl(socket_fd, F_SETFL, flags);
}

/*
 * Ouvre une connexion RFCOMM vers l'appareil configuré.
 *
 * Le socket est explicitement lié à l'adresse Bluetooth locale
 * définie dans la configuration. Cela évite que Linux utilise
 * un autre adaptateur Bluetooth.
 */
int aml_connect(AmlConnection *connection, const AmlConfig *config)
{
    struct sockaddr_rc local = {0};
    struct sockaddr_rc remote = {0};
    int socket_fd;

    if (connection == NULL || config == NULL) {
        fprintf(stderr, "aml_connect : arguments invalides.\n");
        return AML_ERR_GENERIC;
    }

    /* Initialiser l'objet avant toute opération susceptible d'échouer. */
    connection->socket_fd = -1;
    protocol_parser_init(&connection->parser);

    /* Vérifier les paramètres de configuration indispensables. */
    if (config->blt_mac[0] == '\0' ||
        config->aml_mac[0] == '\0' ||
        config->aml_chn < 1 ||
        config->aml_chn > 30) {
        fprintf(stderr, "Configuration Bluetooth de l'AML invalide.\n");
        return AML_ERR_GENERIC;
    }

    /* Créer un socket Bluetooth RFCOMM en mode connecté. */
    socket_fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);

    if (socket_fd < 0) {
        perror("socket Bluetooth");
        return AML_ERR_IO;
    }

    /* Sélectionner l'adaptateur local défini dans la configuration. */
    local.rc_family = AF_BLUETOOTH;
    local.rc_channel = 0;

    if (str2ba(config->blt_mac, &local.rc_bdaddr) != 0) {
        fprintf(stderr, "Adresse Bluetooth locale invalide : %s\n",
                config->blt_mac);
        close(socket_fd);
        return AML_ERR_GENERIC;
    }

    if (bind(socket_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind Bluetooth");
        close(socket_fd);
        return AML_ERR_IO;
    }

    /* Définir l'adresse de l'AML005 et son canal RFCOMM. */
    remote.rc_family = AF_BLUETOOTH;
    remote.rc_channel = (uint8_t)config->aml_chn;

    if (str2ba(config->aml_mac, &remote.rc_bdaddr) != 0) {
        fprintf(stderr, "Adresse Bluetooth de l'AML invalide : %s\n",
                config->aml_mac);
        close(socket_fd);
        return AML_ERR_GENERIC;
    }

    printf("Connexion à l'AML005 \"%s\" (%s), canal %d...\n",
           config->name,
           config->aml_mac,
           config->aml_chn);
    fflush(stdout);

    /* Établir la liaison avec l'AML005, sans attendre indéfiniment. */
    if (connect_with_timeout(
            socket_fd,
            (struct sockaddr *)&remote,
            sizeof(remote),
            AML_CONNECT_TIMEOUT_MS) < 0) {
        perror("connect AML005");
        close(socket_fd);
        return AML_ERR_IO;
    }

    connection->socket_fd = socket_fd;

    printf("Connexion Bluetooth établie.\n");
    fflush(stdout);

    return 0;
}

/*
 * Envoie une trame complète.
 *
 * send() peut transmettre moins d'octets que demandé. La boucle
 * continue donc jusqu'à ce que toute la trame ait été envoyée.
 *
 * MSG_NOSIGNAL est indispensable : sans lui, écrire sur une liaison
 * fermée par l'AML enverrait SIGPIPE et tuerait le service.
 */
int aml_send(
    AmlConnection *connection,
    const uint8_t *data,
    size_t length)
{
    size_t sent = 0;

    if (connection == NULL ||
        connection->socket_fd < 0 ||
        (length > 0 && data == NULL)) {
        fprintf(stderr, "aml_send : connexion ou données invalides.\n");
        return AML_ERR_GENERIC;
    }

    while (sent < length) {
        ssize_t count = send(
            connection->socket_fd,
            data + sent,
            length - sent,
            MSG_NOSIGNAL
        );

        if (count < 0) {
            if (errno == EINTR)
                continue;

            int saved_errno = errno;

            perror("send Bluetooth");

            return errno_means_disconnected(saved_errno)
                   ? AML_ERR_DISCONNECTED
                   : AML_ERR_IO;
        }

        if (count == 0) {
            fprintf(stderr, "Envoi Bluetooth interrompu.\n");
            return AML_ERR_DISCONNECTED;
        }

        sent += (size_t)count;
    }

    return 0;
}

/*
 * Attend la prochaine trame complète.
 *
 * Les octets reçus sont d'abord ajoutés au parseur. Celui-ci conserve
 * les fragments incomplets et permet d'extraire plusieurs trames
 * successives si elles arrivent dans une seule lecture Bluetooth.
 *
 * poll() est utilisé pour appliquer un délai d'attente sans imposer
 * de timeout permanent au socket.
 */
int aml_receive(
    AmlConnection *connection,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length,
    int timeout_ms)
{
    uint8_t buffer[AML_RX_BUFFER_SIZE];
    long long deadline_ms = 0;

    if (connection == NULL ||
        connection->socket_fd < 0 ||
        output == NULL ||
        output_length == NULL) {
        fprintf(stderr, "aml_receive : arguments invalides.\n");
        return AML_ERR_GENERIC;
    }

    /* Fixer l'échéance une fois pour toutes si l'attente est bornée. */
    if (timeout_ms >= 0)
        deadline_ms = monotonic_ms() + timeout_ms;

    for (;;) {
        /*
         * Vérifier d'abord si une trame complète est déjà présente
         * dans le parseur. C'est possible si la lecture précédente
         * avait reçu plusieurs trames.
         */
        int result = protocol_parser_next(
            &connection->parser,
            output,
            output_capacity,
            output_length
        );

        if (result == 1)
            return 0;

        if (result < 0) {
            fprintf(stderr, "Impossible d'extraire une trame AML005.\n");

            /*
             * Le parseur ne consomme pas une trame trop grande pour
             * le tampon de sortie : sans remise à zéro, chaque appel
             * suivant échouerait de la même façon.
             */
            protocol_parser_init(&connection->parser);

            return AML_ERR_PROTOCOL;
        }

        /* Calculer le délai restant, sauf si l'attente est illimitée. */
        int wait_ms = -1;

        if (timeout_ms >= 0) {
            long long remaining_ms = deadline_ms - monotonic_ms();

            if (remaining_ms <= 0)
                return AML_ERR_TIMEOUT;

            wait_ms = remaining_ms > 1000000 ? 1000000 : (int)remaining_ms;
        }

        struct pollfd pfd = {
            .fd = connection->socket_fd,
            .events = POLLIN
        };

        int ready = poll(&pfd, 1, wait_ms);

        if (ready < 0) {
            if (errno == EINTR)
                continue;

            perror("poll Bluetooth");
            return AML_ERR_IO;
        }

        if (ready == 0)
            continue; /* L'échéance sera constatée en haut de boucle. */

        if ((pfd.revents & (POLLERR | POLLNVAL)) &&
            !(pfd.revents & POLLIN)) {
            fprintf(stderr, "Liaison Bluetooth en erreur.\n");
            return AML_ERR_DISCONNECTED;
        }

        /* Lire les octets disponibles sur la liaison RFCOMM. */
        ssize_t received = recv(
            connection->socket_fd,
            buffer,
            sizeof(buffer),
            0
        );

        if (received == 0) {
            fprintf(stderr, "Connexion fermée par l'AML005.\n");
            return AML_ERR_DISCONNECTED;
        }

        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;

            int saved_errno = errno;

            perror("recv Bluetooth");

            return errno_means_disconnected(saved_errno)
                   ? AML_ERR_DISCONNECTED
                   : AML_ERR_IO;
        }

        /*
         * Accumuler les octets reçus. Le parseur les conservera
         * jusqu'à ce qu'une ou plusieurs trames soient complètes.
         */
        if (protocol_parser_feed(
                &connection->parser,
                buffer,
                (size_t)received) < 0) {
            fprintf(stderr, "Tampon de réception AML005 saturé.\n");
            protocol_parser_init(&connection->parser);
            return AML_ERR_PROTOCOL;
        }
    }
}

/*
 * Jette les données en attente de lecture.
 *
 * Le socket est lu en mode non bloquant (MSG_DONTWAIT) : la fonction
 * ne peut donc pas rester bloquée.
 */
size_t aml_flush(AmlConnection *connection)
{
    uint8_t buffer[AML_RX_BUFFER_SIZE];
    size_t discarded;

    if (connection == NULL)
        return 0;

    discarded = connection->parser.length;
    protocol_parser_init(&connection->parser);

    if (connection->socket_fd < 0)
        return discarded;

    for (;;) {
        ssize_t count = recv(
            connection->socket_fd,
            buffer,
            sizeof(buffer),
            MSG_DONTWAIT
        );

        if (count > 0) {
            discarded += (size_t)count;
            continue;
        }

        if (count < 0 && errno == EINTR)
            continue;

        /* 0 (fermeture), EAGAIN (plus rien) ou erreur : on s'arrête. */
        break;
    }

    return discarded;
}

/*
 * Ferme la connexion si elle est ouverte.
 *
 * La fonction peut être appelée même après une erreur de connexion.
 */
void aml_disconnect(AmlConnection *connection)
{
    if (connection == NULL)
        return;

    if (connection->socket_fd >= 0) {
        close(connection->socket_fd);
        connection->socket_fd = -1;
    }

    protocol_parser_init(&connection->parser);
}
