#ifndef SWEETDAWN_COMMAND_H
#define SWEETDAWN_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

/* Types de valeurs acceptées par une commande. */
typedef enum {
    COMMAND_VALUE_NONE,
    COMMAND_VALUE_BOOL,
    COMMAND_VALUE_INTEGER,
    COMMAND_VALUE_NUMBER,
    COMMAND_VALUE_STRING
} CommandValueType;

/*
 * Contraintes applicables à une valeur.
 *
 * minimum et maximum sont inclusifs.
 * choices contient des chaînes empruntées : elles doivent
 * rester valides pendant toute la durée de vie de la commande.
 */
typedef struct {
    CommandValueType type;

    bool has_range;
    double minimum;
    double maximum;

    const char *const *choices;
    size_t choice_count;
} CommandValueSpec;

struct Command;

/*
 * Fonction exécutée lorsqu'une commande est appelée.
 *
 * command : commande exécutée
 * argc    : nombre d'arguments
 * argv    : arguments reçus
 * context : contexte applicatif
 *
 * Retourne 0 en cas de succès, -1 en cas d'erreur.
 */
typedef int (*CommandHandler)(
    const struct Command *command,
    int argc,
    const char *const argv[],
    void *context
);

typedef struct Command {
    const char *name;
    const char *description;

    const char **aliases;
    size_t alias_count;
    size_t alias_capacity;

    struct Command **children;
    size_t child_count;
    size_t child_capacity;

    struct Command *parent;

    CommandValueSpec value_spec;

    CommandHandler handler;

    /* Pointeur emprunté vers l'état associé à la commande. */
    void *state;
} Command;

/* Initialise une commande. */
int command_init(
    Command *command,
    const char *name,
    const char *description,
    CommandHandler handler
);

/* Définit le type et les contraintes de la valeur. */
int command_set_value_spec(
    Command *command,
    const CommandValueSpec *spec
);

/* Ajoute un alias, en rejetant les collisions. */
int command_add_alias(
    Command *command,
    const char *alias
);

/* Ajoute une sous-commande. */
int command_add_child(
    Command *parent,
    Command *child
);

/* Recherche un enfant direct par nom ou alias. */
Command *command_find_child(
    const Command *parent,
    const char *name
);

/*
 * Valide une valeur textuelle selon value_spec.
 *
 * Retourne 0 si la valeur est valide, -1 sinon.
 * error_message peut être NULL.
 */
int command_validate_value(
    const Command *command,
    const char *value,
    char *error_message,
    size_t error_capacity
);

/* Exécute une commande possédant un handler. */
int command_execute(
    const Command *command,
    int argc,
    const char *const argv[],
    void *context
);

/*
 * Libère les tableaux internes et les descendants.
 * Ne libère pas les objets Command eux-mêmes.
 */
void command_clear(Command *command);

#endif