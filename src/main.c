/*
 * main.c — Point d'entrée de SweetDawn
 *
 * Charge la configuration, sélectionne un AML005, établit la
 * connexion Bluetooth, récupère son statut et ses paramètres,
 * puis ferme proprement la connexion.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aml.h"
#include "config.h"
#include "protocol.h"
#include "state.h"

#define CONFIG_PATH "config/aml.json"
#define ERROR_MESSAGE_SIZE 256
#define REQUEST_BUFFER_SIZE AML_PROTOCOL_MAX_FRAME_SIZE
#define RESPONSE_BUFFER_SIZE AML_PROTOCOL_MAX_FRAME_SIZE

#define REQUEST_STATUS_CODE 0x00
#define REQUEST_STATUS_ARGUMENT 0x16
#define RESPONSE_STATUS_CODE 0xF0

#define REQUEST_SETTINGS_CODE 0x12
#define RESPONSE_SETTINGS_CODE 0xE2

static int envoyer_requete(
    AmlConnection *connection,
    uint8_t request_code,
    const uint8_t *arguments,
    size_t arguments_length,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_length
)
{
    uint8_t request[REQUEST_BUFFER_SIZE];
    size_t request_length = 0;

    if (protocol_build_request(
            request_code,
            arguments,
            arguments_length,
            request,
            sizeof(request),
            &request_length) != 0) {
        fprintf(stderr, "Erreur : impossible de construire la requête 0x%02X.\n",
                request_code);
        return -1;
    }

    printf("TX (%zu octets) :", request_length);
    for (size_t i = 0; i < request_length; i++) {
        printf(" %02X", request[i]);
    }
    printf("\n");

    if (aml_send(connection, request, request_length) != 0) {
        fprintf(stderr, "Erreur : échec de l'envoi de la requête 0x%02X.\n",
                request_code);
        return -1;
    }

    if (aml_receive(
            connection,
            response,
            response_capacity,
            response_length,
            5000) != 0) {
        fprintf(stderr, "Erreur : aucune réponse à la requête 0x%02X.\n",
                request_code);
        return -1;
    }

    printf("RX (%zu octets) :", *response_length);
    for (size_t i = 0; i < *response_length; i++) {
        printf(" %02X", response[i]);
    }
    printf("\n");

    return 0;
}

static int demander_statut(AmlConnection *connection)
{
    const uint8_t argument = REQUEST_STATUS_ARGUMENT;
    uint8_t response[RESPONSE_BUFFER_SIZE];
    size_t response_length = 0;

    printf("\n--- Demande du statut ---\n");

    if (envoyer_requete(
            connection,
            REQUEST_STATUS_CODE,
            &argument,
            sizeof(argument),
            response,
            sizeof(response),
            &response_length) != 0) {
        return -1;
    }

    /*
     * Les positions des champs sont celles observées dans la réponse
     * réelle validée lors du test de communication.
     */
    if (response_length < 9 ||
        response[0] != AML_FRAME_START_1 ||
        response[1] != AML_FRAME_START_2 ||
        response[7] != RESPONSE_STATUS_CODE) {
        fprintf(stderr, "Erreur : réponse de statut AML005 inattendue.\n");
        return -1;
    }

    printf("Autorisation AML : %u (0x%02X)\n",
           response[8],
           response[8]);

    return 0;
}

static int demander_parametres(
    AmlConnection *connection,
    AmlState *state
)
{
    uint8_t response[RESPONSE_BUFFER_SIZE];
    size_t response_length = 0;

    printf("\n--- Demande des paramètres ---\n");

    if (envoyer_requete(
            connection,
            REQUEST_SETTINGS_CODE,
            NULL,
            0,
            response,
            sizeof(response),
            &response_length) != 0) {
        return -1;
    }

    /*
     * La réponse de paramètres validée contient :
     * FE EF 0C 0D AB BB 08 E2 display date time feedback fade ...
     */
    if (response_length < 14 ||
        response[0] != AML_FRAME_START_1 ||
        response[1] != AML_FRAME_START_2 ||
        response[7] != RESPONSE_SETTINGS_CODE) {
        fprintf(stderr, "Erreur : réponse de paramètres AML005 inattendue.\n");
        return -1;
    }

    state_update_settings(
        state,
        response[8],
        response[9],
        response[10],
        response[11],
        response[12]
    );

    return 0;
}

int main(int argc, char *argv[])
{
    AppConfig app_config;
    AmlConnection connection = { .socket_fd = -1 };
    AmlState state;
    char error_message[ERROR_MESSAGE_SIZE] = {0};
    const AmlConfig *device;
    int result = 1;

    if (argc > 2) {
        fprintf(stderr, "Usage : %s [nom_appareil]\n", argv[0]);
        return 2;
    }

    if (config_load(
            CONFIG_PATH,
            &app_config,
            error_message,
            sizeof(error_message)) != 0) {
        fprintf(stderr, "Erreur de configuration : %s\n", error_message);
        return 1;
    }

    device = config_select(
        &app_config,
        argc == 2 ? argv[1] : NULL
    );

    if (device == NULL) {
        fprintf(stderr, "Erreur : appareil \"%s\" introuvable dans %s.\n",
                argv[1],
                CONFIG_PATH);
        return 1;
    }

    printf("=== SweetDawn ===\n");
    printf("Appareil sélectionné : %s\n", device->name);

    state_init(&state);

    if (aml_connect(&connection, device) != 0) {
        fprintf(stderr, "Erreur : connexion à l'AML005 impossible.\n");
        goto cleanup;
    }

    if (demander_statut(&connection) != 0) {
        goto cleanup;
    }

    if (demander_parametres(&connection, &state) != 0) {
        goto cleanup;
    }

    printf("\n--- État mémorisé ---\n");
    state_print(&state);

    result = 0;

cleanup:
    aml_disconnect(&connection);

    if (result == 0) {
        printf("\nCommunication terminée avec succès.\n");
    } else {
        fprintf(stderr, "\nCommunication interrompue.\n");
    }

    return result;
}