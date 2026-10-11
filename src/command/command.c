#include "command.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void definir_erreur(
    char *message,
    size_t capacite,
    const char *texte
)
{
    if (message == NULL || capacite == 0) {
        return;
    }

    snprintf(message, capacite, "%s", texte);
}

static int reserver_aliases(Command *command)
{
    size_t capacite;
    const char **nouveaux;

    if (command->alias_count < command->alias_capacity) {
        return 0;
    }

    capacite = command->alias_capacity == 0
        ? 4
        : command->alias_capacity * 2;

    nouveaux = realloc(
        command->aliases,
        capacite * sizeof(*nouveaux)
    );

    if (nouveaux == NULL) {
        return -1;
    }

    command->aliases = nouveaux;
    command->alias_capacity = capacite;

    return 0;
}

static int reserver_enfants(Command *command)
{
    size_t capacite;
    Command **nouveaux;

    if (command->child_count < command->child_capacity) {
        return 0;
    }

    capacite = command->child_capacity == 0
        ? 4
        : command->child_capacity * 2;

    nouveaux = realloc(
        command->children,
        capacite * sizeof(*nouveaux)
    );

    if (nouveaux == NULL) {
        return -1;
    }

    command->children = nouveaux;
    command->child_capacity = capacite;

    return 0;
}

/* Vérifie si name correspond au nom ou à un alias. */
static bool nom_correspond(
    const Command *command,
    const char *name
)
{
    size_t i;

    if (command == NULL || name == NULL) {
        return false;
    }

    if (strcmp(command->name, name) == 0) {
        return true;
    }

    for (i = 0; i < command->alias_count; i++) {
        if (strcmp(command->aliases[i], name) == 0) {
            return true;
        }
    }

    return false;
}

/*
 * Vérifie que deux commandes ne partagent aucun identifiant :
 * ni nom, ni alias.
 */
static bool identifiants_en_collision(
    const Command *a,
    const Command *b
)
{
    size_t i;

    if (nom_correspond(b, a->name)) {
        return true;
    }

    for (i = 0; i < a->alias_count; i++) {
        if (nom_correspond(b, a->aliases[i])) {
            return true;
        }
    }

    return false;
}

int command_init(
    Command *command,
    const char *name,
    const char *description,
    CommandHandler handler
)
{
    if (command == NULL || name == NULL || name[0] == '\0') {
        return -1;
    }

    memset(command, 0, sizeof(*command));

    command->name = name;
    command->description = description;
    command->handler = handler;
    command->value_spec.type = COMMAND_VALUE_NONE;

    return 0;
}

int command_set_value_spec(
    Command *command,
    const CommandValueSpec *spec
)
{
    if (command == NULL || spec == NULL) {
        return -1;
    }

    if (spec->type < COMMAND_VALUE_NONE ||
        spec->type > COMMAND_VALUE_STRING) {
        return -1;
    }

    if (spec->has_range) {
        if (spec->type != COMMAND_VALUE_INTEGER &&
            spec->type != COMMAND_VALUE_NUMBER) {
            return -1;
        }

        if (!isfinite(spec->minimum) ||
            !isfinite(spec->maximum) ||
            spec->minimum > spec->maximum) {
            return -1;
        }
    }

    if (spec->choice_count > 0 && spec->choices == NULL) {
        return -1;
    }

    if (spec->choice_count > 0 &&
        spec->type != COMMAND_VALUE_STRING) {
        return -1;
    }

    command->value_spec = *spec;

    return 0;
}

int command_add_alias(
    Command *command,
    const char *alias
)
{
    size_t i;

    if (command == NULL || alias == NULL || alias[0] == '\0') {
        return -1;
    }

    /* Pas de doublon avec le nom ou les alias existants. */
    if (nom_correspond(command, alias)) {
        return -1;
    }

    /*
     * Si la commande est déjà enregistrée, son alias ne doit
     * pas entrer en collision avec une commande sœur.
     */
    if (command->parent != NULL) {
        for (i = 0; i < command->parent->child_count; i++) {
            Command *sibling = command->parent->children[i];

            if (sibling != command &&
                nom_correspond(sibling, alias)) {
                return -1;
            }
        }
    }

    if (reserver_aliases(command) != 0) {
        return -1;
    }

    command->aliases[command->alias_count++] = alias;

    return 0;
}

int command_add_child(
    Command *parent,
    Command *child
)
{
    size_t i;

    if (parent == NULL || child == NULL || parent == child) {
        return -1;
    }

    if (child->parent != NULL) {
        return -1;
    }

    /*
     * Refuse d'insérer un ancêtre sous l'un de ses descendants :
     * cela créerait un cycle dans l'arbre.
     */
    for (Command *ancestor = parent;
         ancestor != NULL;
         ancestor = ancestor->parent) {
        if (ancestor == child) {
            return -1;
        }
    }

    /* Vérifie les collisions avec tous les enfants existants. */
    for (i = 0; i < parent->child_count; i++) {
        if (identifiants_en_collision(
                child,
                parent->children[i])) {
            return -1;
        }
    }

    if (reserver_enfants(parent) != 0) {
        return -1;
    }

    parent->children[parent->child_count++] = child;
    child->parent = parent;

    return 0;
}

Command *command_find_child(
    const Command *parent,
    const char *name
)
{
    size_t i;

    if (parent == NULL || name == NULL) {
        return NULL;
    }

    for (i = 0; i < parent->child_count; i++) {
        Command *child = parent->children[i];

        if (nom_correspond(child, name)) {
            return child;
        }
    }

    return NULL;
}

int command_validate_value(
    const Command *command,
    const char *value,
    char *error_message,
    size_t error_capacity
)
{
    char *end;
    long long entier;
    double nombre;

    if (error_message != NULL && error_capacity > 0) {
        error_message[0] = '\0';
    }

    if (command == NULL || value == NULL) {
        definir_erreur(
            error_message,
            error_capacity,
            "Commande ou valeur absente."
        );
        return -1;
    }

    switch (command->value_spec.type) {
    case COMMAND_VALUE_NONE:
        definir_erreur(
            error_message,
            error_capacity,
            "Cette commande n'accepte pas de valeur."
        );
        return -1;

    case COMMAND_VALUE_BOOL:
        if (strcmp(value, "true") == 0 ||
            strcmp(value, "false") == 0) {
            return 0;
        }

        definir_erreur(
            error_message,
            error_capacity,
            "Valeur booléenne attendue : true ou false."
        );
        return -1;

    case COMMAND_VALUE_INTEGER:
        if (value[0] == '\0') {
            definir_erreur(
                error_message,
                error_capacity,
                "Un entier est attendu."
            );
            return -1;
        }

        errno = 0;
        end = NULL;
        entier = strtoll(value, &end, 10);

        if (errno == ERANGE ||
            end == value ||
            *end != '\0') {
            definir_erreur(
                error_message,
                error_capacity,
                "Valeur entière invalide."
            );
            return -1;
        }

        nombre = (double)entier;

        if (command->value_spec.has_range &&
            (nombre < command->value_spec.minimum ||
             nombre > command->value_spec.maximum)) {
            definir_erreur(
                error_message,
                error_capacity,
                "L'entier est hors des limites autorisées."
            );
            return -1;
        }

        return 0;

    case COMMAND_VALUE_NUMBER:
        if (value[0] == '\0') {
            definir_erreur(
                error_message,
                error_capacity,
                "Un nombre est attendu."
            );
            return -1;
        }

        errno = 0;
        end = NULL;
        nombre = strtod(value, &end);

        if (errno == ERANGE ||
            end == value ||
            *end != '\0' ||
            !isfinite(nombre)) {
            definir_erreur(
                error_message,
                error_capacity,
                "Valeur numérique invalide."
            );
            return -1;
        }

        if (command->value_spec.has_range &&
            (nombre < command->value_spec.minimum ||
             nombre > command->value_spec.maximum)) {
            definir_erreur(
                error_message,
                error_capacity,
                "Le nombre est hors des limites autorisées."
            );
            return -1;
        }

        return 0;

    case COMMAND_VALUE_STRING:
        if (command->value_spec.choice_count == 0) {
            return 0;
        }

        for (size_t i = 0;
             i < command->value_spec.choice_count;
             i++) {
            const char *choice = command->value_spec.choices[i];

            if (choice != NULL && strcmp(value, choice) == 0) {
                return 0;
            }
        }

        definir_erreur(
            error_message,
            error_capacity,
            "La valeur ne fait pas partie des choix autorisés."
        );
        return -1;
    }

    definir_erreur(
        error_message,
        error_capacity,
        "Type de valeur inconnu."
    );

    return -1;
}

int command_execute(
    const Command *command,
    int argc,
    const char *const argv[],
    void *context
)
{
    if (command == NULL || argc < 0) {
        return -1;
    }

    if (argc > 0 && argv == NULL) {
        return -1;
    }

    if (command->handler == NULL) {
        return -1;
    }

    return command->handler(command, argc, argv, context);
}

void command_clear(Command *command)
{
    size_t i;

    if (command == NULL) {
        return;
    }

    for (i = 0; i < command->child_count; i++) {
        Command *child = command->children[i];

        if (child != NULL) {
            child->parent = NULL;
            command_clear(child);
        }
    }

    free(command->children);
    free(command->aliases);

    command->children = NULL;
    command->child_count = 0;
    command->child_capacity = 0;

    command->aliases = NULL;
    command->alias_count = 0;
    command->alias_capacity = 0;

    command->parent = NULL;
}