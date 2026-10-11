#ifndef AML_COMMANDS_H
#define AML_COMMANDS_H

#include "registry.h"

#include <stddef.h>

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