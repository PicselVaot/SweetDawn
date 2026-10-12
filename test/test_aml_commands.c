/*
 * test/test_aml_commands.c — Tests de l'arbre des commandes AML005
 * (command/aml_commands)
 *
 * Vérifie que les groupes et commandes attendus existent, et que chaque
 * type de valeur (booléen, entier, nombre, choix) accepte et refuse ce
 * qu'il faut.
 */

#include "command/aml_commands.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static Command *chercher(
    AmlCommandTree *tree,
    const char *path
)
{
    CommandRegistry *registry = aml_commands_registry(tree);

    assert(registry != NULL);

    return registry_find_path(registry, path);
}

static void tester_structure(AmlCommandTree *tree)
{
    CommandRegistry *registry = aml_commands_registry(tree);

    assert(registry != NULL);
    assert(strcmp(registry->root.name, "AML") == 0);

    assert(chercher(tree, "Settings") != NULL);
    assert(chercher(tree, "Alarm") != NULL);
    assert(chercher(tree, "Light") != NULL);
    assert(chercher(tree, "Mood") != NULL);
    assert(chercher(tree, "Radio") != NULL);
    assert(chercher(tree, "Media Player") != NULL);

    assert(chercher(tree, "Settings/Display brightness") != NULL);
    assert(chercher(tree, "Settings/Display time") != NULL);
    assert(chercher(tree, "Settings/Time") != NULL);
    assert(chercher(tree, "Settings/Feedback") != NULL);
    assert(chercher(tree, "Settings/Fade") != NULL);

    assert(chercher(tree, "Alarm/Alarm 1/Enable") != NULL);
    assert(chercher(tree, "Alarm/Alarm 1/Days") != NULL);
    assert(chercher(tree, "Alarm/Alarm 1/Snooze") != NULL);
    assert(chercher(tree, "Alarm/Alarm 2/Tone") != NULL);

    assert(chercher(tree, "Light/Enable") != NULL);
    assert(chercher(tree, "Light/Brightness") != NULL);
    assert(chercher(tree, "Light/Sunset") != NULL);

    assert(chercher(tree, "Mood/Color") != NULL);
    assert(chercher(tree, "Mood/Scene") != NULL);
    assert(chercher(tree, "Mood/Timer duration") != NULL);

    assert(chercher(tree, "Radio/Set") != NULL);
    assert(chercher(tree, "Radio/Channel") != NULL);
    assert(chercher(tree, "Radio/Volume") != NULL);
    assert(chercher(tree, "Radio/<<") != NULL);
    assert(chercher(tree, "Radio/<") != NULL);
    assert(chercher(tree, "Radio/>") != NULL);
    assert(chercher(tree, "Radio/>>") != NULL);

    assert(chercher(tree, "Unknown") == NULL);
    assert(chercher(tree, "Light/Unknown") == NULL);

    puts("[OK] Structure et recherche des commandes");
}

static void tester_booleens(AmlCommandTree *tree)
{
    Command *command = chercher(tree, "Light/Enable");

    assert(command != NULL);

    assert(command_validate_value(command, "true", NULL, 0) == 0);
    assert(command_validate_value(command, "false", NULL, 0) == 0);

    assert(command_validate_value(command, "TRUE", NULL, 0) != 0);
    assert(command_validate_value(command, "1", NULL, 0) != 0);
    assert(command_validate_value(command, "yes", NULL, 0) != 0);
    assert(command_validate_value(command, "maybe", NULL, 0) != 0);

    puts("[OK] Validation des booléens");
}

static void tester_entiers(AmlCommandTree *tree)
{
    Command *brightness = chercher(tree, "Light/Brightness");
    Command *days = chercher(tree, "Alarm/Alarm 1/Days");
    Command *volume = chercher(tree, "Radio/Volume");
    Command *sunrise = chercher(tree, "Alarm/Alarm 1/Sunrise duration");

    assert(brightness != NULL);
    assert(days != NULL);
    assert(volume != NULL);
    assert(sunrise != NULL);

    assert(command_validate_value(brightness, "0", NULL, 0) == 0);
    assert(command_validate_value(brightness, "50", NULL, 0) == 0);
    assert(command_validate_value(brightness, "100", NULL, 0) == 0);
    assert(command_validate_value(brightness, "-1", NULL, 0) != 0);
    assert(command_validate_value(brightness, "101", NULL, 0) != 0);
    assert(command_validate_value(brightness, "50.5", NULL, 0) != 0);

    assert(command_validate_value(days, "0", NULL, 0) == 0);
    assert(command_validate_value(days, "42", NULL, 0) == 0);
    assert(command_validate_value(days, "127", NULL, 0) == 0);
    assert(command_validate_value(days, "-1", NULL, 0) != 0);
    assert(command_validate_value(days, "128", NULL, 0) != 0);

    assert(command_validate_value(volume, "1", NULL, 0) == 0);
    assert(command_validate_value(volume, "10", NULL, 0) == 0);
    assert(command_validate_value(volume, "0", NULL, 0) != 0);
    assert(command_validate_value(volume, "11", NULL, 0) != 0);

    assert(command_validate_value(sunrise, "2", NULL, 0) == 0);
    assert(command_validate_value(sunrise, "60", NULL, 0) == 0);
    assert(command_validate_value(sunrise, "1", NULL, 0) != 0);
    assert(command_validate_value(sunrise, "61", NULL, 0) != 0);

    puts("[OK] Validation des entiers et des bornes");
}

static void tester_nombres(AmlCommandTree *tree)
{
    Command *frequency = chercher(tree, "Radio/Set");

    assert(frequency != NULL);

    assert(command_validate_value(frequency, "87.5", NULL, 0) == 0);
    assert(command_validate_value(frequency, "99.9", NULL, 0) == 0);
    assert(command_validate_value(frequency, "108.0", NULL, 0) == 0);

    assert(command_validate_value(frequency, "87.4", NULL, 0) != 0);
    assert(command_validate_value(frequency, "108.1", NULL, 0) != 0);
    assert(command_validate_value(frequency, "radio", NULL, 0) != 0);

    puts("[OK] Validation de la fréquence radio");
}

static void tester_choix(AmlCommandTree *tree)
{
    Command *brightness = chercher(tree, "Settings/Display brightness");
    Command *display_time = chercher(tree, "Settings/Display time");
    Command *time_mode = chercher(tree, "Settings/Time");
    Command *snooze = chercher(tree, "Alarm/Alarm 1/Snooze");
    Command *tone = chercher(tree, "Alarm/Alarm 1/Tone");
    Command *scene = chercher(tree, "Mood/Scene");

    assert(brightness != NULL);
    assert(display_time != NULL);
    assert(time_mode != NULL);
    assert(snooze != NULL);
    assert(tone != NULL);
    assert(scene != NULL);

    assert(command_validate_value(brightness, "H", NULL, 0) == 0);
    assert(command_validate_value(brightness, "A", NULL, 0) == 0);
    assert(command_validate_value(brightness, "L", NULL, 0) == 0);
    assert(command_validate_value(brightness, "M", NULL, 0) != 0);

    assert(command_validate_value(display_time, "12", NULL, 0) == 0);
    assert(command_validate_value(display_time, "24", NULL, 0) == 0);
    assert(command_validate_value(display_time, "18", NULL, 0) != 0);

    assert(command_validate_value(time_mode, "auto", NULL, 0) == 0);
    assert(command_validate_value(time_mode, "manual", NULL, 0) == 0);
    assert(command_validate_value(time_mode, "automatic", NULL, 0) != 0);

    assert(command_validate_value(snooze, "1", NULL, 0) == 0);
    assert(command_validate_value(snooze, "5", NULL, 0) == 0);
    assert(command_validate_value(snooze, "30", NULL, 0) == 0);
    assert(command_validate_value(snooze, "3", NULL, 0) != 0);

    assert(command_validate_value(tone, "Buzzer", NULL, 0) == 0);
    assert(command_validate_value(tone, "Melody 1", NULL, 0) == 0);
    assert(command_validate_value(tone, "Unknown", NULL, 0) != 0);

    assert(command_validate_value(scene, "Off", NULL, 0) == 0);
    assert(command_validate_value(scene, "Rainbow", NULL, 0) == 0);
    assert(command_validate_value(scene, "Rainbow Slow", NULL, 0) == 0);
    assert(command_validate_value(scene, "Summer", NULL, 0) == 0);
    assert(command_validate_value(scene, "Unknown", NULL, 0) != 0);

    puts("[OK] Validation des listes de choix");
}

int main(void)
{
    AmlCommandTree tree;

    assert(aml_commands_init(&tree) == 0);

    assert(tree.command_count < AML_COMMAND_CAPACITY);

    tester_structure(&tree);
    tester_booleens(&tree);
    tester_entiers(&tree);
    tester_nombres(&tree);
    tester_choix(&tree);

    aml_commands_clear(&tree);

    puts("\nTous les tests de l'arbre AML005 sont réussis.");

    return 0;
}
