#ifndef SWEETDAWN_COMMAND_H
#define SWEETDAWN_COMMAND_H

/*
 * command/command.h — Briques de l'arbre des commandes
 *
 * Une commande est un nœud d'arbre :
 *   - un nom (et éventuellement des alias) ;
 *   - des enfants (ex. « Light » contient « Brightness ») ;
 *   - une description du type de valeur acceptée et de ses limites
 *     (ex. un entier entre 0 et 100), utilisée pour valider ce que
 *     l'utilisateur saisit avant de l'envoyer à l'AML ;
 *   - éventuellement une fonction à exécuter (handler).
 *
 * Ce module ne connaît pas l'AML : l'arbre réel est construit par
 * command/aml_commands.c. L'objectif est que CLI, GUI et MQTT
 * partagent la même liste de commandes et les mêmes validations.
 *
 * Mémoire : les objets Command ne sont jamais alloués par ce module ;
 * l'appelant les fournit. Seuls les tableaux internes (alias, enfants)
 * sont alloués, et libérés par command_clear(). Les chaînes (noms,
 * descriptions, alias, choix) sont empruntées, jamais copiées : elles
 * doivent vivre aussi longtemps que la commande (en pratique, des
 * littéraux).
 */

#include <stdbool.h>
#include <stddef.h>

/* Types de valeurs acceptées par une commande. */
typedef enum {
    COMMAND_VALUE_NONE,     /* aucune valeur (action ou groupe)   */
    COMMAND_VALUE_BOOL,     /* "true" ou "false"                  */
    COMMAND_VALUE_INTEGER,  /* entier en base 10                  */
    COMMAND_VALUE_NUMBER,   /* nombre décimal (ex. 101.5)         */
    COMMAND_VALUE_STRING    /* texte, éventuellement dans choices */
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

    /* Autres noms acceptés (tableau dynamique). */
    const char **aliases;
    size_t alias_count;
    size_t alias_capacity;

    /* Sous-commandes (tableau dynamique de pointeurs). */
    struct Command **children;
    size_t child_count;
    size_t child_capacity;

    /* Nœud parent, NULL pour la racine ou une commande isolée. */
    struct Command *parent;

    CommandValueSpec value_spec;

    CommandHandler handler;

    /* Pointeur emprunté vers l'état associé à la commande. */
    void *state;
} Command;

/*
 * Initialise une commande sans enfant, sans alias ni valeur.
 * handler peut être NULL (groupe de commandes, ou commande pas encore
 * implémentée).
 */
int command_init(
    Command *command,
    const char *name,
    const char *description,
    CommandHandler handler
);

/*
 * Définit le type et les contraintes de la valeur.
 *
 * Refuse les combinaisons incohérentes : limites sur autre chose qu'un
 * nombre, minimum > maximum, liste de choix sur autre chose qu'une chaîne.
 */
int command_set_value_spec(
    Command *command,
    const CommandValueSpec *spec
);

/*
 * Ajoute un alias. Refusé s'il est déjà le nom ou un alias de cette
 * commande, ou d'une commande sœur (deux enfants d'un même parent ne
 * peuvent pas répondre au même nom).
 */
int command_add_alias(
    Command *command,
    const char *alias
);

/*
 * Ajoute child sous parent.
 *
 * Refusé si child a déjà un parent, si cela créerait un cycle, ou si
 * un nom ou alias de child est déjà utilisé par un enfant de parent.
 */
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