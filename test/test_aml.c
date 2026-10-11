/*
 * test_aml.c — Test d'intégration Bluetooth de l'AML005
 *
 * Ce test vérifie le transport implémenté dans aml.c :
 *   1. Chargement de la configuration.
 *   2. Connexion Bluetooth à l'AML005.
 *   3. Envoi de la requête de statut.
 *   4. Vérification de l'autorisation de contrôle.
 *   5. Envoi de la requête de paramètres.
 *   6. Affichage des paramètres reçus.
 *
 * Les octets des deux requêtes proviennent des échanges validés
 * sur le matériel réel.
 */

#include <stdint.h>
#include <stdio.h>

#include "aml.h"
#include "config.h"
#include "protocol.h"

/* Affiche une trame sous forme hexadécimale pour le diagnostic. */
static void print_frame(const char *label,
                        const uint8_t *data,
                        size_t length)
{
    printf("%s (%zu octets) : ", label, length);

    for (size_t i = 0; i < length; i++)
        printf("%02X%s", data[i], i + 1 < length ? " " : "");

    putchar('\n');
}

/*
 * Envoie une requête puis attend une réponse complète.
 *
 * La construction de la requête est laissée à protocol.c.
 * Cette fonction ne connaît pas le détail des commandes.
 */
static int send_and_receive(
    AmlConnection *connection,
    const char *name,
    uint8_t request_code,
    const uint8_t *arguments,
    size_t arguments_length,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_length)
{
    uint8_t request[AML_PROTOCOL_MAX_FRAME_SIZE];
    size_t request_length = 0;

    /* Construire la trame de requête. */
    if (protocol_build_request(
            request_code,
            arguments,
            arguments_length,
            request,
            sizeof(request),
            &request_length) < 0) {
        fprintf(stderr, "Impossible de construire la requête %s.\n", name);
        return -1;
    }

    print_frame("TX", request, request_length);

    /* Envoyer la trame à l'AML005. */
    if (aml_send(connection, request, request_length) < 0) {
        fprintf(stderr, "Échec de l'envoi de la requête %s.\n", name);
        return -1;
    }

    /* Attendre une trame de réponse pendant cinq secondes au maximum. */
    if (aml_receive(
            connection,
            response,
            response_capacity,
            response_length,
            5000) < 0) {
        fprintf(stderr, "Échec de réception de la réponse %s.\n", name);
        return -1;
    }

    print_frame("RX", response, *response_length);

    return 0;
}

int main(void)
{
    AppConfig app_config;
    const AmlConfig *device;
    AmlConnection connection = {
        .socket_fd = -1
    };

    uint8_t response[AML_PROTOCOL_MAX_FRAME_SIZE];
    size_t response_length = 0;
    char error[256];
    int exit_status = 1;

    /* Charger la configuration des appareils AML. */
    if (config_load("config/aml.json", &app_config,
                    error, sizeof(error)) < 0) {
        fprintf(stderr, "Erreur de configuration : %s\n", error);
        goto cleanup;
    }

    /* En l'absence de nom explicite, sélectionner le premier appareil. */
    device = config_select(&app_config, NULL);

    if (device == NULL) {
        fprintf(stderr, "Aucun appareil AML configuré.\n");
        goto cleanup;
    }

    /*
     * Requête de statut :
     * code 0x00, paramètre 0x16.
     */
    const uint8_t status_arguments[] = {0x16};

    /*
     * Requête de paramètres :
     * code 0x12, aucun paramètre métier.
     */
    printf("=== Test de communication AML005 ===\n\n");

    if (aml_connect(&connection, device) < 0)
        goto cleanup;

    printf("\n--- Demande du statut ---\n");

    if (send_and_receive(
            &connection,
            "statut",
            0x00,
            status_arguments,
            sizeof(status_arguments),
            response,
            sizeof(response),
            &response_length) < 0) {
        goto cleanup;
    }

    /*
     * Une réponse de statut observée commence par le code 0xF0
     * à l'offset 7, et contient l'autorisation à l'offset 8.
     */
    if (response_length < 10 ||
        response[2] != 0x0C ||
        response[7] != 0xF0) {
        fprintf(stderr, "Réponse de statut inattendue.\n");
        goto cleanup;
    }

    printf("Autorisation AML : %u (0x%02X)\n",
           response[8], response[8]);

    if (response[8] != 22) {
        fprintf(stderr, "Autorisation de contrôle non accordée.\n");
        goto cleanup;
    }

    printf("\n--- Demande des paramètres ---\n");

    if (send_and_receive(
            &connection,
            "paramètres",
            0x12,
            NULL,
            0,
            response,
            sizeof(response),
            &response_length) < 0) {
        goto cleanup;
    }

    /* Vérifier que la réponse correspond à la commande attendue. */
    if (response_length < 14 ||
        response[2] != 0x0C ||
        response[7] != 0xE2) {
        fprintf(stderr, "Réponse de paramètres inattendue.\n");
        goto cleanup;
    }

    printf("\nParamètres reçus :\n");
    printf("  display  = %u (0x%02X)\n", response[8], response[8]);
    printf("  date     = %u (0x%02X)\n", response[9], response[9]);
    printf("  time     = %u (0x%02X)\n", response[10], response[10]);
    printf("  feedback = %u (0x%02X)\n", response[11], response[11]);
    printf("  fade     = %u (0x%02X)\n", response[12], response[12]);

    printf("\nTest terminé avec succès.\n");
    exit_status = 0;

cleanup:
    /* Toujours fermer la connexion, y compris en cas d'échec. */
    aml_disconnect(&connection);

    return exit_status;
}