#ifndef AML_COMMANDS_H
#define AML_COMMANDS_H

/*
 * command/aml_commands.h — Arbre complet des commandes de l'AML005
 *
 * Décrit tout ce que l'appareil sait faire, groupé par fonction :
 *
 *   Settings      luminosité et format d'affichage, heure, son, fondu
 *   Alarm         Alarm 1 et Alarm 2 : heure, jours, sonnerie, lever de soleil
 *   Light         éclairage principal, coucher de soleil
 *   Mood          éclairage d'ambiance : couleur, scènes, minuteur
 *   Radio         FM : fréquence, canaux, volume, arrêt programmé
 *   Media Player  utilisation comme haut-parleur
 *
 * État actuel : l'arbre décrit les commandes et valide les valeurs,
 * mais aucune commande n'a encore de handler. Les relier aux trames
 * du protocole est l'étape 4 du TODO.
 */

#include "command/registry.h"

#include <stddef.h>

/* Nombre total de commandes que l'arbre peut contenir (marge incluse). */
#define AML_COMMAND_CAPACITY 64

/*
 * Arbre de commandes AML005.
 *
 * Les objets Command sont stockés directement dans cette structure :
 * leur adresse reste donc valide pendant toute la durée de vie de l'arbre.
 */
typedef struct {
    CommandRegistry registry;
    Command commands[AML_COMMAND_CAPACITY];
    size_t command_count;
} AmlCommandTree;

/*
 * Initialise l'arbre complet des commandes AML005.
 *
 * Retourne 0 en cas de succès, -1 en cas d'erreur.
 */
int aml_commands_init(AmlCommandTree *tree);

/*
 * Libère les ressources internes de l'arbre.
 */
void aml_commands_clear(AmlCommandTree *tree);

/*
 * Retourne le registre associé à l'arbre.
 */
CommandRegistry *aml_commands_registry(AmlCommandTree *tree);

#endif