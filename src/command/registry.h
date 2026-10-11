#ifndef SWEETDAWN_REGISTRY_H
#define SWEETDAWN_REGISTRY_H

#include "command.h"

typedef struct {
    Command root;
} CommandRegistry;

/* Initialise le registre et sa racine AML. */
int registry_init(CommandRegistry *registry);

/* Ajoute une commande à la racine. */
int registry_add(
    CommandRegistry *registry,
    Command *command
);

/* Recherche une commande de premier niveau. */
Command *registry_find(
    const CommandRegistry *registry,
    const char *name
);

/*
 * Recherche une commande par chemin.
 *
 * Exemples :
 *   Light
 *   Light/Brightness
 *   AML/Alarm/Alarm 1/Days
 */
Command *registry_find_path(
    const CommandRegistry *registry,
    const char *path
);

/* Libère les ressources internes du registre. */
void registry_clear(CommandRegistry *registry);

#endif