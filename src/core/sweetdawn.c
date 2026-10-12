/*
 * core/sweetdawn.c — Cœur applicatif de SweetDawn
 *
 * Ce module coordonne les autres sans implémenter lui-même le
 * transport Bluetooth (aml/transport.c) ni le format des trames
 * (aml/protocol.c).
 *
 * Organisation du fichier :
 *   1. Temps            : horloge monotone, remplaçable par les tests.
 *   2. Supervision      : reconnexion avec attente croissante, abandon
 *                         d'une liaison morte.
 *   3. Échanges         : une requête, une réponse ; lecture du statut
 *                         et des paramètres.
 *   4. API publique     : sweetdawn_request, init, connect, refresh,
 *                         tick, accesseurs.
 *
 * Tout s'exécute dans un seul fil : le service appelle sweetdawn_tick()
 * depuis la boucle du serveur IPC (voir app/service.c).
 */

#define _POSIX_C_SOURCE 200809L

#include "core/sweetdawn.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "aml/protocol.h"

#define REQUEST_BUFFER_SIZE AML_PROTOCOL_MAX_FRAME_SIZE
#define RESPONSE_BUFFER_SIZE AML_PROTOCOL_MAX_FRAME_SIZE

/* Délai maximal entre l'envoi d'une requête et sa réponse. */
#define AML_RESPONSE_TIMEOUT_MS 5000

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

/* ------------------------------------------------------------------------- */
/* Temps                                                                     */
/* ------------------------------------------------------------------------- */

/* Millisecondes sur l'horloge monotone du système. */
static int64_t horloge_systeme_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;

    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Heure courante, via l'horloge injectée si elle existe. */
static int64_t maintenant(const SweetDawn *app)
{
    return app->clock_ms != NULL
           ? app->clock_ms()
           : horloge_systeme_ms();
}

/* ------------------------------------------------------------------------- */
/* Supervision de la liaison                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Programme la prochaine tentative de connexion.
 *
 * L'attente double après chaque échec, de BACKOFF_MIN à BACKOFF_MAX :
 * un AML éteint pour la nuit n'est pas sondé en continu, et un AML qui
 * revient est retrouvé en quelques secondes.
 */
static void programmer_reconnexion(SweetDawn *app)
{
    if (app->connect_backoff_ms <= 0) {
        app->connect_backoff_ms = SWEETDAWN_BACKOFF_MIN_MS;
    } else if (app->connect_backoff_ms >= SWEETDAWN_BACKOFF_MAX_MS / 2) {
        app->connect_backoff_ms = SWEETDAWN_BACKOFF_MAX_MS;
    } else {
        app->connect_backoff_ms *= 2;
    }

    app->next_connect_ms = maintenant(app) + app->connect_backoff_ms;
}

/*
 * Ferme la liaison actuelle et prévoit d'en ouvrir une nouvelle.
 *
 * L'état mémorisé est conservé : il reste consultable, mais son âge
 * (sweetdawn_state_age_ms) indique qu'il n'est plus rafraîchi.
 */
static void abandonner_liaison(SweetDawn *app)
{
    aml_disconnect(&app->connection);

    app->connected = false;
    app->consecutive_failures = 0;

    programmer_reconnexion(app);
}

/*
 * Tire les conséquences d'un échec d'échange.
 *
 *   - liaison perdue ou erreur système : on abandonne tout de suite ;
 *   - simple délai dépassé ou trame illisible : on tolère quelques
 *     échecs de suite avant de conclure que la liaison est morte
 *     (une liaison Bluetooth peut devenir silencieuse sans se fermer).
 */
static void traiter_echec_liaison(SweetDawn *app, int code)
{
    if (code == AML_ERR_DISCONNECTED || code == AML_ERR_IO) {
        fprintf(stderr, "SweetDawn : liaison perdue, reconnexion prévue.\n");
        abandonner_liaison(app);
        return;
    }

    app->consecutive_failures++;

    if (app->consecutive_failures >= SWEETDAWN_MAX_FAILURES) {
        fprintf(stderr,
                "SweetDawn : %d échecs consécutifs, "
                "la liaison est considérée comme morte.\n",
                app->consecutive_failures);
        abandonner_liaison(app);
    }
}

/* ------------------------------------------------------------------------- */
/* Échanges avec l'AML                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Construit et envoie une requête, puis attend SA réponse.
 *
 * C'est l'implémentation de sweetdawn_request() : voir core/sweetdawn.h
 * pour le détail des paramètres.
 *
 * Toute trame reçue entre-temps qui n'est pas la réponse attendue
 * (réponse tardive d'un échange précédent, message spontané) est
 * écartée au lieu d'être prise pour la réponse.
 *
 * En cas d'échec, la supervision de la liaison est mise à jour :
 * l'appelant n'a pas à s'en préoccuper.
 */
static int envoyer_requete(
    SweetDawn *app,
    uint8_t request_code,
    int expected_code,
    int timeout_ms,
    const uint8_t *arguments,
    size_t arguments_length,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_length
)
{
    uint8_t request[REQUEST_BUFFER_SIZE];
    size_t request_length = 0;
    int64_t deadline;
    size_t discarded;
    int code;

    if (app == NULL || !app->initialized) {
        fprintf(stderr,
                "SweetDawn : arguments invalides pour une requête AML.\n");
        return AML_ERR_GENERIC;
    }

    if (!app->connected)
        return AML_ERR_DISCONNECTED;

    if (expected_code != SWEETDAWN_NO_RESPONSE &&
        (response == NULL || response_length == NULL)) {
        fprintf(stderr,
                "SweetDawn : tampon de réponse manquant.\n");
        return AML_ERR_GENERIC;
    }

    if (timeout_ms <= 0)
        timeout_ms = AML_RESPONSE_TIMEOUT_MS;

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
        return AML_ERR_GENERIC;
    }

    /*
     * Repartir d'une liaison propre. Si l'AML envoie un jour des
     * messages spontanés, ce message permettra de le remarquer.
     */
    discarded = aml_flush(&app->connection);

    if (discarded > 0) {
        fprintf(stderr,
                "SweetDawn : %zu octet(s) non sollicité(s) ignoré(s) "
                "avant la requête 0x%02X.\n",
                discarded, request_code);
    }

    code = aml_send(&app->connection, request, request_length);

    if (code != 0) {
        fprintf(stderr,
                "SweetDawn : échec de l'envoi de la requête 0x%02X.\n",
                request_code);
        traiter_echec_liaison(app, code);
        return code;
    }

    /* Commande sans réponse : l'envoi réussi suffit. */
    if (expected_code == SWEETDAWN_NO_RESPONSE)
        return 0;

    deadline = maintenant(app) + timeout_ms;

    for (;;) {
        int64_t remaining = deadline - maintenant(app);

        if (remaining <= 0) {
            code = AML_ERR_TIMEOUT;
            break;
        }

        code = aml_receive(
            &app->connection,
            response,
            response_capacity,
            response_length,
            (int)remaining
        );

        if (code != 0)
            break;

        if (*response_length > AML_FRAME_CODE_OFFSET &&
            response[2] == AML_FRAME_TYPE_RESPONSE &&
            response[AML_FRAME_CODE_OFFSET] == (uint8_t)expected_code) {
            app->consecutive_failures = 0;
            return 0;
        }

        fprintf(stderr,
                "SweetDawn : trame inattendue ignorée "
                "(code 0x%02X, attendu 0x%02X).\n",
                *response_length > AML_FRAME_CODE_OFFSET
                    ? response[AML_FRAME_CODE_OFFSET]
                    : 0,
                expected_code);
    }

    fprintf(stderr,
            "SweetDawn : aucune réponse exploitable à la requête 0x%02X.\n",
            request_code);

    traiter_echec_liaison(app, code);

    return code;
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
    const uint8_t argument = AML_STATUS_ARG;
    uint8_t response[RESPONSE_BUFFER_SIZE];
    size_t response_length = 0;
    int code;

    if (autorisation == NULL)
        return AML_ERR_GENERIC;

    code = envoyer_requete(
        app,
        AML_REQ_STATUS,
        AML_RSP_STATUS,
        0,
        &argument,
        sizeof(argument),
        response,
        sizeof(response),
        &response_length
    );

    if (code != 0)
        return code;

    /* Une seule donnée attendue : l'octet d'autorisation. */
    if (response_length < AML_FRAME_DATA_OFFSET + 1) {
        fprintf(stderr,
                "SweetDawn : réponse de statut AML005 trop courte.\n");
        return AML_ERR_PROTOCOL;
    }

    *autorisation = response[AML_FRAME_DATA_OFFSET];

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
    int code;

    code = envoyer_requete(
        app,
        AML_REQ_READ_SETTINGS,
        AML_RSP_READ_SETTINGS,
        0,
        NULL,
        0,
        response,
        sizeof(response),
        &response_length
    );

    if (code != 0)
        return code;

    /* Cinq données attendues, suivies au moins du checksum. */
    if (response_length < AML_FRAME_DATA_OFFSET + 6) {
        fprintf(stderr,
                "SweetDawn : réponse de paramètres AML005 trop courte.\n");
        return AML_ERR_PROTOCOL;
    }

    const uint8_t *data = &response[AML_FRAME_DATA_OFFSET];

    state_update_settings(
        &app->state,
        data[0],   /* display  */
        data[1],   /* date     */
        data[2],   /* time     */
        data[3],   /* feedback */
        data[4]    /* fade     */
    );

    return 0;
}

/* ------------------------------------------------------------------------- */
/* API publique                                                              */
/* ------------------------------------------------------------------------- */

int sweetdawn_request(
    SweetDawn *app,
    uint8_t request_code,
    const uint8_t *arguments,
    size_t arguments_length,
    int expected_code,
    int timeout_ms,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_length
)
{
    return envoyer_requete(
        app,
        request_code,
        expected_code,
        timeout_ms,
        arguments,
        arguments_length,
        response,
        response_capacity,
        response_length
    );
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
    app->last_refresh_ms = -1;

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
    int code;

    if (app == NULL || !app->initialized || app->device == NULL) {
        fprintf(stderr,
                "SweetDawn : l'application n'est pas initialisée.\n");
        return AML_ERR_GENERIC;
    }

    if (app->connected)
        return 0;

    code = aml_connect(&app->connection, app->device);

    if (code != 0)
        return code;

    app->connected = true;
    app->consecutive_failures = 0;

    /* Relire l'état dès le prochain passage de sweetdawn_tick(). */
    app->next_refresh_ms = 0;

    return 0;
}

/*
 * Actualise les informations actuellement prises en charge.
 *
 * Les échecs sont déjà pris en compte par la supervision de la liaison
 * (voir envoyer_requete) ; ici on ne règle que la date de la prochaine
 * relecture.
 */
int sweetdawn_refresh(SweetDawn *app)
{
    uint8_t autorisation = 0;
    int code;

    if (app == NULL || !app->initialized || !app->connected) {
        fprintf(stderr,
                "SweetDawn : connexion requise pour actualiser l'état.\n");
        return AML_ERR_GENERIC;
    }

    code = demander_statut(app, &autorisation);

    if (code == 0) {
        app->authorization = autorisation;
        code = demander_parametres(app);
    }

    if (code != 0) {
        app->next_refresh_ms = maintenant(app) + SWEETDAWN_REFRESH_RETRY_MS;
        return code;
    }

    app->last_refresh_ms = maintenant(app);
    app->next_refresh_ms = app->last_refresh_ms + SWEETDAWN_REFRESH_INTERVAL_MS;

    /* La liaison fonctionne : la prochaine panne repart d'un délai court. */
    app->connect_backoff_ms = 0;

    return 0;
}

/*
 * Entretient la liaison : reconnexion et relecture périodique.
 */
void sweetdawn_tick(SweetDawn *app)
{
    int64_t now;

    if (app == NULL || !app->initialized)
        return;

    now = maintenant(app);

    if (!app->connected) {
        if (now < app->next_connect_ms)
            return;

        if (sweetdawn_connect(app) != 0) {
            /* Dater l'échec après la tentative : elle peut durer. */
            programmer_reconnexion(app);
            return;
        }

        app->connect_count++;

        if (app->connect_count > 1)
            printf("SweetDawn : liaison rétablie.\n");

        sweetdawn_refresh(app);

        return;
    }

    if (now >= app->next_refresh_ms)
        sweetdawn_refresh(app);
}

bool sweetdawn_is_connected(const SweetDawn *app)
{
    return app != NULL && app->initialized && app->connected;
}

int64_t sweetdawn_state_age_ms(const SweetDawn *app)
{
    if (app == NULL || !app->initialized || app->last_refresh_ms < 0)
        return -1;

    return maintenant(app) - app->last_refresh_ms;
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