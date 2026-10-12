#ifndef SWEETDAWN_H
#define SWEETDAWN_H

/*
 * core/sweetdawn.h — API principale de SweetDawn
 *
 * Ce module coordonne la configuration, la connexion AML005,
 * l'état mémorisé et le registre des commandes.
 *
 * Cycle de vie :
 *
 *   SweetDawn app;
 *   sweetdawn_init(&app, "config/aml.json", NULL, err, sizeof(err));
 *   for (;;) {
 *       sweetdawn_tick(&app);           // connexion, relecture, reprise
 *       ... lire sweetdawn_get_state(&app) ...
 *   }
 *   sweetdawn_cleanup(&app);
 *
 * Il n'est pas nécessaire d'appeler sweetdawn_connect() ni
 * sweetdawn_refresh() soi-même : sweetdawn_tick() s'en charge.
 *
 * Attention : app->device pointe à l'intérieur de app->config. Une
 * structure SweetDawn ne doit donc pas être copiée ni déplacée après
 * sweetdawn_init().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aml/transport.h"
#include "command/aml_commands.h"
#include "core/config.h"
#include "core/state.h"

#define SWEETDAWN_ERROR_SIZE 256

/*
 * Réglages de la supervision de la liaison (en millisecondes).
 *
 * REFRESH_INTERVAL : période de relecture de l'état, qui sert aussi
 *                    de sonde : une liaison morte est détectée au plus
 *                    tard à la relecture suivante.
 * REFRESH_RETRY    : délai avant de retenter une relecture échouée.
 * BACKOFF_MIN/MAX  : attente entre deux tentatives de connexion ;
 *                    elle double après chaque échec jusqu'au maximum.
 * MAX_FAILURES     : échecs consécutifs (délais dépassés) après lesquels
 *                    la liaison est abandonnée puis rétablie.
 */
#define SWEETDAWN_REFRESH_INTERVAL_MS 60000
#define SWEETDAWN_REFRESH_RETRY_MS     5000
#define SWEETDAWN_BACKOFF_MIN_MS       1000
#define SWEETDAWN_BACKOFF_MAX_MS      30000
#define SWEETDAWN_MAX_FAILURES            3

typedef struct {
    AppConfig config;            /* tous les appareils du fichier JSON   */
    const AmlConfig *device;     /* celui qui a été sélectionné          */

    AmlConnection connection;    /* liaison Bluetooth                    */
    AmlState state;              /* dernières valeurs lues               */
    AmlCommandTree commands;     /* commandes disponibles                */

    bool initialized;            /* sweetdawn_init() a réussi            */
    bool connected;              /* la liaison est ouverte               */

    /* Dernière autorisation de contrôle lue (22 = accordée). */
    uint8_t authorization;

    /*
     * Supervision de la liaison. Les dates sont des millisecondes
     * sur l'horloge monotone (voir clock_ms).
     */
    int64_t last_refresh_ms;   /* -1 tant que l'état n'a jamais été lu */
    int64_t next_refresh_ms;
    int64_t next_connect_ms;
    int connect_backoff_ms;
    int consecutive_failures;
    unsigned connect_count;    /* connexions réussies depuis le démarrage */

    /*
     * Source de temps remplaçable pour les tests.
     * NULL : horloge monotone du système.
     */
    int64_t (*clock_ms)(void);
} SweetDawn;

/*
 * Initialise le cœur de l'application.
 *
 * Charge la configuration, sélectionne l'appareil, initialise
 * l'état et construit l'arbre des commandes.
 *
 * config_path :
 *   Chemin du fichier JSON de configuration.
 *
 * device_name :
 *   Nom de l'appareil à sélectionner.
 *   NULL ou chaîne vide sélectionne le premier appareil.
 *
 * Retour :
 *   0  : initialisation réussie.
 *  -1  : erreur.
 */
int sweetdawn_init(
    SweetDawn *app,
    const char *config_path,
    const char *device_name,
    char *error_message,
    size_t error_capacity
);

/* Valeur de expected_code pour une commande qui n'a pas de réponse. */
#define SWEETDAWN_NO_RESPONSE (-1)

/*
 * Envoie une requête à l'AML et attend sa réponse.
 *
 * request_code  : code de la commande (AML_REQ_* dans aml/protocol.h).
 * arguments     : données de la commande, sans l'enveloppe de trame
 *                 (NULL si arguments_length vaut 0).
 * expected_code : code de la réponse attendue (AML_RSP_*), ou
 *                 SWEETDAWN_NO_RESPONSE si l'AML ne répond pas à cette
 *                 commande : la fonction revient dès l'envoi réussi.
 * timeout_ms    : délai d'attente de la réponse ; 0 = délai par défaut
 *                 (5 s).
 * response      : reçoit la trame de réponse complète ; les données
 *                 commencent à response[AML_FRAME_DATA_OFFSET].
 *                 Peut être NULL avec SWEETDAWN_NO_RESPONSE.
 *
 * Retour :
 *   0                    : succès ;
 *   AML_ERR_DISCONNECTED : pas de liaison en ce moment ;
 *   AML_ERR_TIMEOUT      : pas de réponse dans le délai ;
 *   autre AML_ERR_*      : erreur de transport ou de trame.
 *
 * En cas d'échec, la supervision de la liaison est mise à jour
 * automatiquement (voir sweetdawn_tick).
 */
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
);

/* Établit la connexion Bluetooth avec l'appareil sélectionné. */
int sweetdawn_connect(SweetDawn *app);

/*
 * Interroge les informations actuellement prises en charge
 * et met à jour l'état mémorisé.
 */
int sweetdawn_refresh(SweetDawn *app);

/*
 * À appeler régulièrement (au moins une fois par seconde).
 *
 * Elle s'occupe de tout ce qui concerne la durée de vie de la liaison :
 *   - tenter de se (re)connecter, avec une attente croissante entre
 *     deux échecs ;
 *   - relire l'état à intervalle régulier, ce qui détecte aussi une
 *     liaison devenue silencieuse ;
 *   - abandonner la liaison après SWEETDAWN_MAX_FAILURES échecs
 *     consécutifs afin d'en rétablir une neuve.
 *
 * Elle ne fait rien d'autre : elle est sans effet si rien n'est dû.
 */
void sweetdawn_tick(SweetDawn *app);

/* Indique si la liaison avec l'AML005 est actuellement ouverte. */
bool sweetdawn_is_connected(const SweetDawn *app);

/*
 * Âge de l'état mémorisé en millisecondes,
 * ou -1 s'il n'a jamais été lu avec succès.
 */
int64_t sweetdawn_state_age_ms(const SweetDawn *app);

/* Retourne l'état mémorisé, ou NULL si l'application est invalide. */
const AmlState *sweetdawn_get_state(const SweetDawn *app);

/* Retourne le registre des commandes, ou NULL si invalide. */
const CommandRegistry *sweetdawn_get_commands(const SweetDawn *app);

/* Ferme la connexion et libère les ressources internes. */
void sweetdawn_cleanup(SweetDawn *app);

#endif /* SWEETDAWN_H */