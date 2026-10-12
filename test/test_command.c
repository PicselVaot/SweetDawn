/*
 * test/test_command.c — Tests du moteur de commandes (command/command,
 * command/registry)
 *
 * Vérifie la construction d'un petit arbre, les alias et leurs
 * collisions, la recherche par chemin, la validation des valeurs et
 * l'exécution d'un handler.
 */

#include "command/command.h"
#include "command/registry.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int handler_test(
    const Command *command,
    int argc,
    const char *const argv[],
    void *context
)
{
    int *compteur = context;

    assert(command != NULL);
    assert(argc == 1);
    assert(strcmp(argv[0], "75") == 0);

    (*compteur)++;

    return 0;
}

int main(void)
{
    CommandRegistry registry;

    Command light;
    Command brightness;
    Command duplicate;
    Command feedback;
    Command days;
    Command scene;

    int compteur = 0;
    char erreur[128];

    const char *arguments[] = {"75"};

    const char *scenes[] = {
        "Off",
        "Random",
        "Rainbow",
        "Rainbow Slow",
        "Fusion",
        "Pulse",
        "Wave",
        "Chill",
        "Action",
        "Forest",
        "Summer"
    };

    CommandValueSpec spec_brightness = {
        .type = COMMAND_VALUE_INTEGER,
        .has_range = true,
        .minimum = 0,
        .maximum = 100
    };

    CommandValueSpec spec_feedback = {
        .type = COMMAND_VALUE_BOOL
    };

    CommandValueSpec spec_days = {
        .type = COMMAND_VALUE_INTEGER,
        .has_range = true,
        .minimum = 0,
        .maximum = 127
    };

    CommandValueSpec spec_scene = {
        .type = COMMAND_VALUE_STRING,
        .choices = scenes,
        .choice_count = sizeof(scenes) / sizeof(scenes[0])
    };

    assert(registry_init(&registry) == 0);

    assert(command_init(
        &light,
        "Light",
        "Contrôle de la lumière",
        NULL
    ) == 0);

    assert(command_init(
        &brightness,
        "Brightness",
        "Luminosité de la lumière",
        handler_test
    ) == 0);

    assert(command_init(
        &duplicate,
        "Brightness",
        "Commande en doublon",
        NULL
    ) == 0);

    assert(command_init(
        &feedback,
        "Feedback",
        "Retour sonore",
        NULL
    ) == 0);

    assert(command_init(
        &days,
        "Days",
        "Jours de l'alarme",
        NULL
    ) == 0);

    assert(command_init(
        &scene,
        "Scene",
        "Scène lumineuse",
        NULL
    ) == 0);

    /* Alias et collisions. */
    assert(command_add_alias(&light, "lumiere") == 0);
    assert(command_add_alias(&light, "lumiere") == -1);

    assert(registry_add(&registry, &light) == 0);

    assert(command_add_child(&light, &brightness) == 0);
    assert(command_add_alias(&brightness, "bright") == 0);

    /* Un nom déjà utilisé par une commande sœur est refusé. */
    assert(command_add_child(&light, &duplicate) == -1);

    /* Un alias ne peut pas entrer en collision avec un frère. */
    assert(command_init(
        &duplicate,
        "Intensite",
        "Commande de test",
        NULL
    ) == 0);

    assert(command_add_child(&light, &duplicate) == 0);
    assert(command_add_alias(&duplicate, "bright") == -1);

    /* Recherche directe et recherche par chemin. */
    assert(registry_find(&registry, "Light") == &light);
    assert(registry_find(&registry, "lumiere") == &light);

    assert(registry_find_path(
        &registry,
        "Light/Brightness"
    ) == &brightness);

    assert(registry_find_path(
        &registry,
        "lumiere/bright"
    ) == &brightness);

    assert(registry_find_path(
        &registry,
        "AML/Light/Brightness"
    ) == &brightness);

    assert(registry_find_path(
        &registry,
        "Light/Unknown"
    ) == NULL);

    assert(registry_find_path(&registry, "") == NULL);
    assert(registry_find_path(&registry, "/Light") == NULL);
    assert(registry_find_path(&registry, "Light/") == NULL);
    assert(registry_find_path(&registry, "Light//Brightness") == NULL);

    /* Validation de la luminosité : entier de 0 à 100. */
    assert(command_set_value_spec(
        &brightness,
        &spec_brightness
    ) == 0);

    assert(command_validate_value(
        &brightness, "0", erreur, sizeof(erreur)
    ) == 0);

    assert(command_validate_value(
        &brightness, "100", erreur, sizeof(erreur)
    ) == 0);

    assert(command_validate_value(
        &brightness, "75", erreur, sizeof(erreur)
    ) == 0);

    assert(command_validate_value(
        &brightness, "-1", erreur, sizeof(erreur)
    ) == -1);

    assert(command_validate_value(
        &brightness, "101", erreur, sizeof(erreur)
    ) == -1);

    assert(command_validate_value(
        &brightness, "12.5", erreur, sizeof(erreur)
    ) == -1);

    assert(command_validate_value(
        &brightness, "abc", erreur, sizeof(erreur)
    ) == -1);

    /* Validation d'un booléen. */
    assert(command_set_value_spec(
        &feedback,
        &spec_feedback
    ) == 0);

    assert(command_validate_value(
        &feedback, "true", erreur, sizeof(erreur)
    ) == 0);

    assert(command_validate_value(
        &feedback, "false", erreur, sizeof(erreur)
    ) == 0);

    assert(command_validate_value(
        &feedback, "1", erreur, sizeof(erreur)
    ) == -1);

    /* Validation du masque des jours : entier de 0 à 127. */
    assert(command_set_value_spec(
        &days,
        &spec_days
    ) == 0);

    assert(command_validate_value(
        &days, "0", erreur, sizeof(erreur)
    ) == 0);

    assert(command_validate_value(
        &days, "42", erreur, sizeof(erreur)
    ) == 0);

    assert(command_validate_value(
        &days, "127", erreur, sizeof(erreur)
    ) == 0);

    assert(command_validate_value(
        &days, "128", erreur, sizeof(erreur)
    ) == -1);

    /* Validation des scènes Mood. */
    assert(command_set_value_spec(
        &scene,
        &spec_scene
    ) == 0);

    assert(command_validate_value(
        &scene, "Rainbow", erreur, sizeof(erreur)
    ) == 0);

    assert(command_validate_value(
        &scene, "Rainbow Slow", erreur, sizeof(erreur)
    ) == 0);

    assert(command_validate_value(
        &scene, "Unknown", erreur, sizeof(erreur)
    ) == -1);

    /* Exécution d'un handler. */
    assert(command_execute(
        &brightness,
        1,
        arguments,
        &compteur
    ) == 0);

    assert(compteur == 1);

    /* Le conteneur nettoie aussi ses descendants. */
    registry_clear(&registry);

    /*
     * Ces commandes ne sont pas enregistrées dans le registre :
     * on libère donc séparément leurs ressources internes.
     */
    command_clear(&feedback);
    command_clear(&days);
    command_clear(&scene);

    puts("Tous les tests command sont passes.");

    return 0;
}