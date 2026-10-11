/*
 * sweetdawn.c — Cœur applicatif de SweetDawn
 *
 * Ce module coordonne les modules existants sans implémenter
 * lui-même le transport Bluetooth ni le parseur du protocole.
 */

#include "sweetdawn.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "protocol.h"

#define REQUEST_BUFFER_SIZE AML_PROTOCOL_MAX_FRAME_SIZE
#define RESPONSE_BUFFER_SIZE AML_PROTOCOL_MAX_FRAME_SIZE
#define AML_RESPONSE_TIMEOUT_MS 5000

#define REQUEST_STATUS_CODE 0x00
#define REQUEST_STATUS_ARGUMENT 0x16
#define RESPONSE_STATUS_CODE 0xF0

#define REQUEST_SETTINGS_CODE 0x12
#define RESPONSE_SETTINGS_CODE 0xE2

/*
 * Ajoute un message d'erreur si un tampon valide est fourni.
 */
static void definir_erreur(
    char *message,
    size_t capacite,
    const char *texte
)
{
    if (message == NULL || capacite == 0)
        return;

    snprintf(message, capacite, "%s", texte);
}

/*
 * Construit et envoie une requête, puis attend une réponse.
 *
 * Cette fonction centralise les échanges synchrones. Elle ne
 * présume pas de la signification de la réponse reçue.
 */
static int envoyer_requete(
    SweetDawn *app,
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

    if (app == NULL ||
        !app->connected ||
        response == NULL ||
        response_length == NULL) {
        fprintf(stderr,
                "SweetDawn : arguments invalides pour une requête AML.\n");
        return -1;
    }

    if (protocol_build_request(
            request_code,
            arguments,
            arguments_length,
            request,
            sizeof(request),
            &request_length) != 0) {
        fprintf(stderr,
                "SweetDawn : impossible de construire la requête 0x%02X.\n",
                request_code);
        return -1;
    }

    if (aml_send(
            &app->connection,
            request,
            request_length) != 0) {
        fprintf(stderr,
                "SweetDawn : échec de l'envoi de la requête 0x%02X.\n",
                request_code);
        return -1;
    }

    if (aml_receive(
            &app->connection,
            response,
            response_capacity,
            response_length,
            AML_RESPONSE_TIMEOUT_MS) != 0) {
        fprintf(stderr,
                "SweetDawn : aucune réponse à la requête 0x%02X.\n",
                request_code);
        return -1;
    }

    return 0;
}

/*
 * Interroge l'autorisation de l'AML005.
 *
 * La valeur est renvoyée à l'appelant afin de ne pas la confondre
 * avec l'état des paramètres généraux.
 */
static int demander_statut(
    SweetDawn *app,
    uint8_t *autorisation
)
{
    const uint8_t argument = REQUEST_STATUS_ARGUMENT;
    uint8_t response[RESPONSE_BUFFER_SIZE];
    size_t response_length = 0;

    if (autorisation == NULL)
        return -1;

    if (envoyer_requete(
            app,
            REQUEST_STATUS_CODE,
            &argument,
            sizeof(argument),
            response,
            sizeof(response),
            &response_length) != 0) {
        return -1;
    }

    if (response_length < 9 ||
        response[0] != AML_FRAME_START_1 ||
        response[1] != AML_FRAME_START_2 ||
        response[2] != 0x0C ||
        response[7] != RESPONSE_STATUS_CODE) {
        fprintf(stderr,
                "SweetDawn : réponse de statut AML005 inattendue.\n");
        return -1;
    }

    *autorisation = response[8];

    return 0;
}

/*
 * Lit les paramètres généraux et actualise l'état mémorisé.
 *
 * L'état n'est modifié qu'après validation de la réponse.
 */
static int demander_parametres(
    SweetDawn *app
)
{
    uint8_t response[RESPONSE_BUFFER_SIZE];
    size_t response_length = 0;

    if (envoyer_requete(
            app,
            REQUEST_SETTINGS_CODE,
            NULL,
            0,
            response,
            sizeof(response),
            &response_length) != 0) {
        return -1;
    }

    if (response_length < 14 ||
        response[0] != AML_FRAME_START_1 ||
        response[1] != AML_FRAME_START_2 ||
        response[2] != 0x0C ||
        response[7] != RESPONSE_SETTINGS_CODE) {
        fprintf(stderr,
                "SweetDawn : réponse de paramètres AML005 inattendue.\n");
        return -1;
    }

    state_update_settings(
        &app->state,
        response[8],
        response[9],
        response[10],
        response[11],
        response[12]
    );

    return 0;
}

/*
 * Initialise les ressources du cœur applicatif.
 */
int sweetdawn_init(
    SweetDawn *app,
    const char *config_path,
    const char *device_name,
    char *error_message,
    size_t error_capacity
)
{
    const AmlConfig *device;

    if (app == NULL || config_path == NULL || config_path[0] == '\0') {
        definir_erreur(
            error_message,
            error_capacity,
            "Arguments invalides pour l'initialisation de SweetDawn."
        );
        return -1;
    }

    if (error_message != NULL && error_capacity > 0)
        error_message[0] = '\0';

    memset(app, 0, sizeof(*app));
    app->connection.socket_fd = -1;

    state_init(&app->state);

    if (config_load(
            config_path,
            &app->config,
            error_message,
            error_capacity) != 0) {
        return -1;
    }

    device = config_select(&app->config, device_name);

    if (device == NULL) {
        if (error_message != NULL && error_capacity > 0) {
            snprintf(
                error_message,
                error_capacity,
                "Appareil \"%s\" introuvable dans la configuration.",
                device_name != NULL ? device_name : ""
            );
        }
        return -1;
    }

    /*
     * Le pointeur désigne un élément de app->config.
     * Il reste valide tant que la structure app n'est pas déplacée.
     */
    app->device = device;

    if (aml_commands_init(&app->commands) != 0) {
        definir_erreur(
            error_message,
            error_capacity,
            "Impossible d'initialiser l'arbre des commandes AML."
        );
        return -1;
    }

    app->initialized = true;

    return 0;
}

/*
 * Établit la connexion avec l'appareil sélectionné.
 */
int sweetdawn_connect(SweetDawn *app)
{
    if (app == NULL || !app->initialized || app->device == NULL) {
        fprintf(stderr,
                "SweetDawn : l'application n'est pas initialisée.\n");
        return -1;
    }

    if (app->connected)
        return 0;

    if (aml_connect(&app->connection, app->device) != 0)
        return -1;

    app->connected = true;

    return 0;
}

/*
 * Actualise les informations actuellement prises en charge.
 */
int sweetdawn_refresh(SweetDawn *app)
{
    uint8_t autorisation;

    if (app == NULL || !app->initialized || !app->connected) {
        fprintf(stderr,
                "SweetDawn : connexion requise pour actualiser l'état.\n");
        return -1;
    }

    if (demander_statut(app, &autorisation) != 0)
        return -1;

    printf("Autorisation AML : %u (0x%02X)\n",
           (unsigned int)autorisation,
           (unsigned int)autorisation);

    if (demander_parametres(app) != 0)
        return -1;

    return 0;
}

/*
 * Fournit un accès en lecture à l'état applicatif.
 */
const AmlState *sweetdawn_get_state(const SweetDawn *app)
{
    if (app == NULL || !app->initialized)
        return NULL;

    return &app->state;
}

/*
 * Fournit un accès en lecture au registre des commandes.
 */
const CommandRegistry *sweetdawn_get_commands(const SweetDawn *app)
{
    if (app == NULL || !app->initialized)
        return NULL;

    return &app->commands.registry;
}

/*
 * Ferme la connexion et libère les ressources internes.
 *
 * La fonction peut être appelée après une initialisation partielle.
 */
void sweetdawn_cleanup(SweetDawn *app)
{
    if (app == NULL)
        return;

    aml_disconnect(&app->connection);

    if (app->initialized || app->commands.command_count > 0)
        aml_commands_clear(&app->commands);

    memset(app, 0, sizeof(*app));
    app->connection.socket_fd = -1;
}