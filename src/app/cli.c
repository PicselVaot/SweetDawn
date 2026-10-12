/*
 * app/cli.c — Programme bin/sweetdawn : diagnostic ponctuel
 *
 * Charge la configuration, sélectionne un AML005, établit la
 * connexion Bluetooth, récupère son statut et ses paramètres,
 * puis ferme proprement la connexion.
 *
 * Chaque trame envoyée (TX) et reçue (RX) est affichée en hexadécimal :
 * c'est l'outil à lancer pour vérifier le matériel ou étudier une
 * nouvelle commande du protocole.
 *
 * Ce programme parle directement à l'AML, sans passer par le service.
 * Ne pas le lancer pendant que bin/sweetdawn-service tourne : l'AML
 * n'accepte qu'une liaison à la fois.
 *
 * Usage : ./bin/sweetdawn [nom_appareil]
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aml/transport.h"
#include "core/config.h"
#include "aml/protocol.h"
#include "core/state.h"

#define CONFIG_PATH "config/aml.json"
#define ERROR_MESSAGE_SIZE 256
#define REQUEST_BUFFER_SIZE AML_PROTOCOL_MAX_FRAME_SIZE
#define RESPONSE_BUFFER_SIZE AML_PROTOCOL_MAX_FRAME_SIZE

/* Délai maximal d'attente d'une réponse. */
#define RESPONSE_TIMEOUT_MS 5000

/* Affiche une trame en hexadécimal, précédée d'une étiquette. */
static void afficher_trame(
    const char *label,
    const uint8_t *data,
    size_t length
)
{
    printf("%s (%zu octets) :", label, length);

    for (size_t i = 0; i < length; i++)
        printf(" %02X", data[i]);

    printf("\n");
}

/*
 * Construit une requête, l'envoie, puis attend une trame en retour.
 *
 * Contrairement au service, on ne vérifie pas ici que la trame reçue
 * est bien la réponse attendue : c'est fait par l'appelant, qui affiche
 * une erreur explicite si ce n'est pas le cas.
 */
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

    afficher_trame("TX", request, request_length);

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
            RESPONSE_TIMEOUT_MS) != 0) {
        fprintf(stderr, "Erreur : aucune réponse à la requête 0x%02X.\n",
                request_code);
        return -1;
    }

    afficher_trame("RX", response, *response_length);

    return 0;
}

/* Demande le statut et affiche l'octet d'autorisation reçu. */
static int demander_statut(AmlConnection *connection)
{
    const uint8_t argument = AML_STATUS_ARG;
    uint8_t response[RESPONSE_BUFFER_SIZE];
    size_t response_length = 0;

    printf("\n--- Demande du statut ---\n");

    if (envoyer_requete(
            connection,
            AML_REQ_STATUS,
            &argument,
            sizeof(argument),
            response,
            sizeof(response),
            &response_length) != 0) {
        return -1;
    }

    /* Format de la réponse : voir aml/protocol.h. */
    if (response_length < AML_FRAME_DATA_OFFSET + 1 ||
        response[AML_FRAME_CODE_OFFSET] != AML_RSP_STATUS) {
        fprintf(stderr, "Erreur : réponse de statut AML005 inattendue.\n");
        return -1;
    }

    uint8_t autorisation = response[AML_FRAME_DATA_OFFSET];

    printf("Autorisation AML : %u (0x%02X)\n",
           autorisation,
           autorisation);

    return 0;
}

/* Demande les paramètres généraux et les enregistre dans state. */
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
            AML_REQ_READ_SETTINGS,
            NULL,
            0,
            response,
            sizeof(response),
            &response_length) != 0) {
        return -1;
    }

    /*
     * La réponse de paramètres contient :
     * FE EF 0C 0D AB BB 08 E2 display date time feedback fade XX 55 0D 0A
     */
    if (response_length < AML_FRAME_DATA_OFFSET + 6 ||
        response[AML_FRAME_CODE_OFFSET] != AML_RSP_READ_SETTINGS) {
        fprintf(stderr, "Erreur : réponse de paramètres AML005 inattendue.\n");
        return -1;
    }

    const uint8_t *data = &response[AML_FRAME_DATA_OFFSET];

    state_update_settings(state, data[0], data[1], data[2], data[3], data[4]);

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