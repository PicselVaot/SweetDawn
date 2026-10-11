#ifndef SWEETDAWN_H
#define SWEETDAWN_H

/*
 * sweetdawn.h — API principale de SweetDawn
 *
 * Ce module coordonne la configuration, la connexion AML005,
 * l'état mémorisé et le registre des commandes.
 *
 * Les interfaces CLI, GUI et MQTT utiliseront cette API commune.
 */

#include <stdbool.h>
#include <stddef.h>

#include "aml.h"
#include "command/aml_commands.h"
#include "config.h"
#include "state.h"

#define SWEETDAWN_ERROR_SIZE 256

typedef struct {
    AppConfig config;
    const AmlConfig *device;

    AmlConnection connection;
    AmlState state;
    AmlCommandTree commands;

    bool initialized;
    bool connected;
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

/* Établit la connexion Bluetooth avec l'appareil sélectionné. */
int sweetdawn_connect(SweetDawn *app);

/*
 * Interroge les informations actuellement prises en charge
 * et met à jour l'état mémorisé.
 */
int sweetdawn_refresh(SweetDawn *app);

/* Retourne l'état mémorisé, ou NULL si l'application est invalide. */
const AmlState *sweetdawn_get_state(const SweetDawn *app);

/* Retourne le registre des commandes, ou NULL si invalide. */
const CommandRegistry *sweetdawn_get_commands(const SweetDawn *app);

/* Ferme la connexion et libère les ressources internes. */
void sweetdawn_cleanup(SweetDawn *app);

#endif /* SWEETDAWN_H */