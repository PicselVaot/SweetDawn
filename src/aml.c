/*
 * aml.c — Transport Bluetooth RFCOMM pour l'AML005
 *
 * Ce module ne connaît pas la signification des commandes.
 * Il assure uniquement le transport des octets entre le programme
 * et l'AML005.
 *
 * La construction des requêtes est effectuée par protocol.c.
 * Les données reçues sont analysées par le parseur du protocole.
 */

#include "aml.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/time.h>

/* Taille du bloc lu à chaque appel de recv(). */
#define AML_RX_BUFFER_SIZE 256

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
        return -1;
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
        return -1;
    }

    /* Créer un socket Bluetooth RFCOMM en mode connecté. */
    socket_fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);

    if (socket_fd < 0) {
        perror("socket Bluetooth");
        return -1;
    }

    /* Sélectionner l'adaptateur local défini dans la configuration. */
    local.rc_family = AF_BLUETOOTH;
    local.rc_channel = 0;

    if (str2ba(config->blt_mac, &local.rc_bdaddr) != 0) {
        fprintf(stderr, "Adresse Bluetooth locale invalide : %s\n",
                config->blt_mac);
        close(socket_fd);
        return -1;
    }

    if (bind(socket_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind Bluetooth");
        close(socket_fd);
        return -1;
    }

    /* Définir l'adresse de l'AML005 et son canal RFCOMM. */
    remote.rc_family = AF_BLUETOOTH;
    remote.rc_channel = (uint8_t)config->aml_chn;

    if (str2ba(config->aml_mac, &remote.rc_bdaddr) != 0) {
        fprintf(stderr, "Adresse Bluetooth de l'AML invalide : %s\n",
                config->aml_mac);
        close(socket_fd);
        return -1;
    }

    printf("Connexion à l'AML005 \"%s\" (%s), canal %d...\n",
           config->name,
           config->aml_mac,
           config->aml_chn);
    fflush(stdout);

    /* Établir la liaison avec l'AML005. */
    if (connect(socket_fd, (struct sockaddr *)&remote, sizeof(remote)) < 0) {
        perror("connect AML005");
        close(socket_fd);
        return -1;
    }

    connection->socket_fd = socket_fd;

    printf("Connexion Bluetooth établie.\n");

    return 0;
}

/*
 * Envoie une trame complète.
 *
 * send() peut transmettre moins d'octets que demandé. La boucle
 * continue donc jusqu'à ce que toute la trame ait été envoyée.
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
        return -1;
    }

    while (sent < length) {
        ssize_t count = send(
            connection->socket_fd,
            data + sent,
            length - sent,
            0
        );

        if (count < 0) {
            if (errno == EINTR)
                continue;

            perror("send Bluetooth");
            return -1;
        }

        if (count == 0) {
            fprintf(stderr, "Envoi Bluetooth interrompu.\n");
            return -1;
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
 * select() est utilisé pour appliquer un délai d'attente sans imposer
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
    struct timeval start_time;
    struct timeval now;
    long long elapsed_ms;
    long long remaining_ms;

    if (connection == NULL ||
        connection->socket_fd < 0 ||
        output == NULL ||
        output_length == NULL) {
        fprintf(stderr, "aml_receive : arguments invalides.\n");
        return -1;
    }

    /* Enregistrer le début de l'attente pour calculer le délai restant. */
    if (gettimeofday(&start_time, NULL) < 0) {
        perror("gettimeofday");
        return -1;
    }

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
            return -1;
        }

        /* Calculer le délai restant, sauf si l'attente est illimitée. */
        if (timeout_ms >= 0) {
            if (gettimeofday(&now, NULL) < 0) {
                perror("gettimeofday");
                return -1;
            }

            elapsed_ms =
                ((long long)now.tv_sec - start_time.tv_sec) * 1000LL
                + ((long long)now.tv_usec - start_time.tv_usec) / 1000LL;

            remaining_ms = (long long)timeout_ms - elapsed_ms;

            if (remaining_ms <= 0) {
                fprintf(stderr, "Délai de réception AML005 dépassé.\n");
                return -1;
            }
        }

        /* Préparer select() pour attendre des données sur le socket. */
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(connection->socket_fd, &read_fds);

        struct timeval timeout;
        struct timeval *timeout_ptr = NULL;

        if (timeout_ms >= 0) {
            timeout.tv_sec = (time_t)(remaining_ms / 1000LL);
            timeout.tv_usec = (suseconds_t)((remaining_ms % 1000LL) * 1000LL);
            timeout_ptr = &timeout;
        }

        int ready = select(
            connection->socket_fd + 1,
            &read_fds,
            NULL,
            NULL,
            timeout_ptr
        );

        if (ready < 0) {
            if (errno == EINTR)
                continue;

            perror("select Bluetooth");
            return -1;
        }

        if (ready == 0) {
            fprintf(stderr, "Délai de réception AML005 dépassé.\n");
            return -1;
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
            return -1;
        }

        if (received < 0) {
            if (errno == EINTR)
                continue;

            perror("recv Bluetooth");
            return -1;
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
            return -1;
        }
    }
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
}