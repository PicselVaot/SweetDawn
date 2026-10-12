#ifndef SWEETDAWN_REGISTRY_H
#define SWEETDAWN_REGISTRY_H

/*
 * command/registry.h — Point d'entrée de l'arbre des commandes
 *
 * Le registre n'est qu'une racine nommée « AML » sous laquelle sont
 * rangées les commandes de premier niveau, plus une recherche par
 * chemin du type "Light/Brightness".
 */

#include "command/command.h"

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
 * Recherche une commande par chemin, segments séparés par '/'.
 * Le préfixe « AML/ » est facultatif. Les alias sont acceptés à chaque
 * niveau. Retourne NULL si un segment est introuvable ou vide.
 *
 * Exemples :
 *   Light
 *   Light/Brightness
 *   AML/Alarm/1/Days
 */
Command *registry_find_path(
    const CommandRegistry *registry,
    const char *path
);

/* Libère les ressources internes du registre. */
void registry_clear(CommandRegistry *registry);

#endif