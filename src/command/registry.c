/*
 * command/registry.c — Racine de l'arbre et recherche par chemin
 */

#include "command/registry.h"

#include <stdlib.h>
#include <string.h>

int registry_init(CommandRegistry *registry)
{
    if (registry == NULL) {
        return -1;
    }

    memset(registry, 0, sizeof(*registry));

    return command_init(
        &registry->root,
        "AML",
        "Racine des commandes AML",
        NULL
    );
}

int registry_add(
    CommandRegistry *registry,
    Command *command
)
{
    if (registry == NULL || command == NULL) {
        return -1;
    }

    return command_add_child(&registry->root, command);
}

Command *registry_find(
    const CommandRegistry *registry,
    const char *name
)
{
    if (registry == NULL || name == NULL) {
        return NULL;
    }

    return command_find_child(&registry->root, name);
}

Command *registry_find_path(
    const CommandRegistry *registry,
    const char *path
)
{
    const char *segment;
    const char *separator;
    Command *current;

    if (registry == NULL || path == NULL || path[0] == '\0') {
        return NULL;
    }

    current = (Command *)&registry->root;
    segment = path;

    while (*segment != '\0') {
        size_t longueur;
        char *nom;

        separator = strchr(segment, '/');

        longueur = separator == NULL
            ? strlen(segment)
            : (size_t)(separator - segment);

        if (longueur == 0) {
            return NULL;
        }

        /*
         * Le premier segment peut désigner la racine AML.
         */
        if (current == &registry->root &&
            strlen(registry->root.name) == longueur &&
            strncmp(segment, registry->root.name, longueur) == 0) {
            if (separator == NULL) {
                return NULL;
            }

            segment = separator + 1;

            if (*segment == '\0') {
                return NULL;
            }

            continue;
        }

        /* Isoler le segment courant dans une chaîne terminée par '\0'. */
        nom = malloc(longueur + 1);

        if (nom == NULL) {
            return NULL;
        }

        memcpy(nom, segment, longueur);
        nom[longueur] = '\0';

        current = command_find_child(current, nom);

        free(nom);

        if (current == NULL) {
            return NULL;
        }

        if (separator == NULL) {
            return current;
        }

        segment = separator + 1;

        if (*segment == '\0') {
            return NULL;
        }
    }

    return NULL;
}

void registry_clear(CommandRegistry *registry)
{
    if (registry == NULL) {
        return;
    }

    command_clear(&registry->root);
}