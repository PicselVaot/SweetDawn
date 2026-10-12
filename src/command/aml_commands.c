/*
 * command/aml_commands.c — Construction de l'arbre des commandes AML005
 *
 * Pour ajouter une commande : l'ajouter dans aml_commands_init() avec
 * ajouter_commande(), puis préciser sa valeur avec definir_booleen,
 * definir_entier, definir_nombre ou definir_choix. Penser à vérifier
 * que AML_COMMAND_CAPACITY reste suffisant.
 */

#include "command/aml_commands.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Valeurs autorisées                                                        */
/* ------------------------------------------------------------------------- */

static const char *const DISPLAY_BRIGHTNESS_VALUES[] = {
    "H", "A", "L"
};

static const char *const DISPLAY_TIME_VALUES[] = {
    "24", "12"
};

static const char *const TIME_MODE_VALUES[] = {
    "auto", "manual"
};

static const char *const SNOOZE_VALUES[] = {
    "1", "2", "5", "10", "20", "30"
};

static const char *const TONE_VALUES[] = {
    "Buzzer",
    "Radio",
    "Melody 1",
    "Birds",
    "Bells",
    "Piano",
    "Melody 2"
};

static const char *const SCENE_VALUES[] = {
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

/* ------------------------------------------------------------------------- */
/* Création des commandes                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Crée une commande dans le tableau appartenant à l'arbre.
 *
 * L'objet Command lui-même n'est pas alloué dynamiquement.
 * Seules ses ressources internes sont gérées par le moteur existant.
 */
static Command *creer_commande(
    AmlCommandTree *tree,
    const char *name,
    const char *description
)
{
    if (tree == NULL || tree->command_count >= AML_COMMAND_CAPACITY) {
        return NULL;
    }

    Command *command = &tree->commands[tree->command_count];

    if (command_init(command, name, description, NULL) != 0) {
        return NULL;
    }

    tree->command_count++;

    return command;
}

/*
 * Crée une commande enfant et l'ajoute à son parent.
 */
static Command *ajouter_commande(
    AmlCommandTree *tree,
    Command *parent,
    const char *name,
    const char *description
)
{
    if (parent == NULL) {
        return NULL;
    }

    Command *command = creer_commande(tree, name, description);

    if (command == NULL) {
        return NULL;
    }

    if (command_add_child(parent, command) != 0) {
        return NULL;
    }

    return command;
}

/*
 * Définit une valeur booléenne.
 * Les seules valeurs acceptées par le moteur sont "true" et "false".
 */
static int definir_booleen(Command *command)
{
    CommandValueSpec spec = {
        .type = COMMAND_VALUE_BOOL
    };

    return command_set_value_spec(command, &spec);
}

/*
 * Définit une valeur entière avec limites inclusives.
 */
static int definir_entier(
    Command *command,
    double minimum,
    double maximum
)
{
    CommandValueSpec spec = {
        .type = COMMAND_VALUE_INTEGER,
        .has_range = true,
        .minimum = minimum,
        .maximum = maximum
    };

    return command_set_value_spec(command, &spec);
}

/*
 * Définit une valeur numérique avec limites inclusives.
 */
static int definir_nombre(
    Command *command,
    double minimum,
    double maximum
)
{
    CommandValueSpec spec = {
        .type = COMMAND_VALUE_NUMBER,
        .has_range = true,
        .minimum = minimum,
        .maximum = maximum
    };

    return command_set_value_spec(command, &spec);
}

/*
 * Définit une chaîne choisie dans une liste autorisée.
 */
static int definir_choix(
    Command *command,
    const char *const *choices,
    size_t choice_count
)
{
    CommandValueSpec spec = {
        .type = COMMAND_VALUE_STRING,
        .choices = choices,
        .choice_count = choice_count
    };

    return command_set_value_spec(command, &spec);
}

/* ------------------------------------------------------------------------- */
/* Paramètres communs des alarmes                                            */
/* ------------------------------------------------------------------------- */

/* Ajoute sous « Alarm N » les réglages, identiques pour les deux alarmes. */
static int ajouter_parametres_alarme(
    AmlCommandTree *tree,
    Command *alarme
)
{
    Command *command;

    command = ajouter_commande(
        tree, alarme, "Enable", "Activer ou désactiver l'alarme"
    );
    if (command == NULL || definir_booleen(command) != 0) {
        return -1;
    }

    command = ajouter_commande(
        tree, alarme, "Time", "Heure de l'alarme au format HH:MM"
    );
    if (command == NULL) {
        return -1;
    }

    /*
     * La validation stricte du format HH:MM sera ajoutée au moteur
     * de validation dans une étape dédiée.
     */

    /* Un bit par jour : 7 bits, donc de 0 (aucun jour) à 127 (tous). */
    command = ajouter_commande(
        tree, alarme, "Days", "Jours actifs sous forme de masque binaire"
    );
    if (command == NULL || definir_entier(command, 0, 127) != 0) {
        return -1;
    }

    command = ajouter_commande(
        tree, alarme, "Snooze", "Durée de répétition en minutes"
    );
    if (
        command == NULL ||
        definir_choix(
            command,
            SNOOZE_VALUES,
            sizeof(SNOOZE_VALUES) / sizeof(SNOOZE_VALUES[0])
        ) != 0
    ) {
        return -1;
    }

    command = ajouter_commande(
        tree, alarme, "Tone", "Son de l'alarme"
    );
    if (
        command == NULL ||
        definir_choix(
            command,
            TONE_VALUES,
            sizeof(TONE_VALUES) / sizeof(TONE_VALUES[0])
        ) != 0
    ) {
        return -1;
    }

    command = ajouter_commande(
        tree, alarme, "Tone volume", "Volume du son de l'alarme"
    );
    if (command == NULL || definir_entier(command, 1, 10) != 0) {
        return -1;
    }

    command = ajouter_commande(
        tree, alarme, "Sunrise", "Activer ou désactiver le lever de soleil"
    );
    if (command == NULL || definir_booleen(command) != 0) {
        return -1;
    }

    command = ajouter_commande(
        tree, alarme, "Sunrise duration", "Durée du lever de soleil en minutes"
    );
    if (command == NULL || definir_entier(command, 2, 60) != 0) {
        return -1;
    }

    command = ajouter_commande(
        tree, alarme, "Sunrise brightness", "Luminosité maximale du lever de soleil"
    );
    if (command == NULL || definir_entier(command, 1, 100) != 0) {
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Construction de l'arbre                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Construit l'arbre complet. En cas d'échec (capacité dépassée,
 * collision de noms...), tout ce qui a été construit est libéré.
 */
int aml_commands_init(AmlCommandTree *tree)
{
    if (tree == NULL) {
        return -1;
    }

    memset(tree, 0, sizeof(*tree));

    if (registry_init(&tree->registry) != 0) {
        return -1;
    }

    Command *root = &tree->registry.root;
    Command *group;
    Command *command;

    /* ================================ Settings =========================== */

    group = ajouter_commande(
        tree, root, "Settings", "Consulter les paramètres de l'AML"
    );
    if (group == NULL) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Display brightness", "Luminosité de l'affichage"
    );
    if (
        command == NULL ||
        definir_choix(
            command,
            DISPLAY_BRIGHTNESS_VALUES,
            sizeof(DISPLAY_BRIGHTNESS_VALUES)
                / sizeof(DISPLAY_BRIGHTNESS_VALUES[0])
        ) != 0
    ) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Display time", "Format horaire de l'affichage"
    );
    if (
        command == NULL ||
        definir_choix(
            command,
            DISPLAY_TIME_VALUES,
            sizeof(DISPLAY_TIME_VALUES) / sizeof(DISPLAY_TIME_VALUES[0])
        ) != 0
    ) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Time", "Mode de gestion de l'heure"
    );
    if (
        command == NULL ||
        definir_choix(
            command,
            TIME_MODE_VALUES,
            sizeof(TIME_MODE_VALUES) / sizeof(TIME_MODE_VALUES[0])
        ) != 0
    ) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Feedback", "Activer ou désactiver le retour sonore"
    );
    if (command == NULL || definir_booleen(command) != 0) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Fade", "Activer ou désactiver le fondu lumineux"
    );
    if (command == NULL || definir_booleen(command) != 0) {
        goto erreur;
    }

    /* ================================= Alarm ============================= */

    group = ajouter_commande(
        tree, root, "Alarm", "Gérer les alarmes"
    );
    if (group == NULL) {
        goto erreur;
    }

    Command *alarme1 = ajouter_commande(
        tree, group, "Alarm 1", "Consulter ou modifier l'alarme 1"
    );
    if (alarme1 == NULL || ajouter_parametres_alarme(tree, alarme1) != 0) {
        goto erreur;
    }

    Command *alarme2 = ajouter_commande(
        tree, group, "Alarm 2", "Consulter ou modifier l'alarme 2"
    );
    if (alarme2 == NULL || ajouter_parametres_alarme(tree, alarme2) != 0) {
        goto erreur;
    }

    /* ================================= Light ============================= */

    group = ajouter_commande(
        tree, root, "Light", "Consulter ou modifier l'éclairage"
    );
    if (group == NULL) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Enable", "Activer ou désactiver l'éclairage"
    );
    if (command == NULL || definir_booleen(command) != 0) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Brightness", "Luminosité de l'éclairage"
    );
    if (command == NULL || definir_entier(command, 0, 100) != 0) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Sunset", "Activer ou désactiver le coucher de soleil"
    );
    if (command == NULL || definir_booleen(command) != 0) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Sunset duration", "Durée du coucher de soleil en minutes"
    );
    if (command == NULL || definir_entier(command, 1, 60) != 0) {
        goto erreur;
    }

    /* ================================= Mood ============================== */

    group = ajouter_commande(
        tree, root, "Mood", "Consulter ou modifier l'éclairage d'ambiance"
    );
    if (group == NULL) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Enable", "Activer ou désactiver le mode Mood"
    );
    if (command == NULL || definir_booleen(command) != 0) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Color", "Couleur RGB de l'éclairage d'ambiance"
    );
    if (command == NULL) {
        goto erreur;
    }

    /*
     * La validation des trois composantes RGB sera ajoutée dans une
     * étape dédiée. Elle doit accepter trois entiers compris entre 0 et 255.
     */

    command = ajouter_commande(
        tree, group, "Brightness", "Luminosité du mode Mood"
    );
    if (command == NULL || definir_entier(command, 0, 100) != 0) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Timer", "Activer ou désactiver le minuteur Mood"
    );
    if (command == NULL || definir_booleen(command) != 0) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Timer duration", "Durée du minuteur en minutes"
    );
    if (command == NULL || definir_entier(command, 1, 120) != 0) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Scene", "Scène lumineuse"
    );
    if (
        command == NULL ||
        definir_choix(
            command,
            SCENE_VALUES,
            sizeof(SCENE_VALUES) / sizeof(SCENE_VALUES[0])
        ) != 0
    ) {
        goto erreur;
    }

    /* ================================= Radio ============================= */

    group = ajouter_commande(
        tree, root, "Radio", "Consulter ou modifier la radio"
    );
    if (group == NULL) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Enable", "Activer ou désactiver la radio"
    );
    if (command == NULL || definir_booleen(command) != 0) {
        goto erreur;
    }

    if (
        ajouter_commande(
            tree, group, "<<", "Sélectionner la station détectée précédente"
        ) == NULL
    ) {
        goto erreur;
    }

    if (
        ajouter_commande(
            tree, group, "<", "Diminuer la fréquence de 0,1 MHz"
        ) == NULL
    ) {
        goto erreur;
    }

    if (
        ajouter_commande(
            tree, group, ">", "Augmenter la fréquence de 0,1 MHz"
        ) == NULL
    ) {
        goto erreur;
    }

    if (
        ajouter_commande(
            tree, group, ">>", "Sélectionner la station détectée suivante"
        ) == NULL
    ) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Set", "Régler la fréquence radio en MHz"
    );
    if (command == NULL || definir_nombre(command, 87.5, 108.0) != 0) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Channel", "Sélectionner un canal radio"
    );
    if (command == NULL || definir_entier(command, 1, 10) != 0) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Volume", "Volume de la radio"
    );
    if (command == NULL || definir_entier(command, 1, 10) != 0) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Sleep timer", "Activer ou désactiver l'arrêt programmé"
    );
    if (command == NULL || definir_booleen(command) != 0) {
        goto erreur;
    }

    command = ajouter_commande(
        tree, group, "Timer duration", "Durée avant l'arrêt en minutes"
    );
    if (command == NULL || definir_entier(command, 1, 60) != 0) {
        goto erreur;
    }

    /* ============================== Media Player ========================= */

    if (
        ajouter_commande(
            tree, root, "Media Player", "Connecter l'AML comme haut-parleur"
        ) == NULL
    ) {
        goto erreur;
    }

    return 0;

erreur:
    aml_commands_clear(tree);
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Nettoyage                                                                 */
/* ------------------------------------------------------------------------- */

void aml_commands_clear(AmlCommandTree *tree)
{
    if (tree == NULL) {
        return;
    }

    registry_clear(&tree->registry);

    /*
     * registry_clear libère les tableaux internes des commandes.
     * Les objets eux-mêmes appartiennent à tree et ne sont pas libérés.
     */
    memset(tree->commands, 0, sizeof(tree->commands));
    tree->command_count = 0;
}

CommandRegistry *aml_commands_registry(AmlCommandTree *tree)
{
    if (tree == NULL) {
        return NULL;
    }

    return &tree->registry;
}