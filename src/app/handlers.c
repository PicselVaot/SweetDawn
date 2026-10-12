/*
 * app/handlers.c — Commandes IPC du service
 *
 * Organisation du fichier :
 *   1. Contexte et erreurs  : ce qu'un handler reçoit et renvoie.
 *   2. Conversions          : texte <-> valeurs, choix <-> index.
 *   3. Handlers             : un groupe de l'arbre après l'autre
 *                             (Settings, Alarm, Light/Mood, Radio,
 *                             Media Player).
 *   4. Table des handlers   : quel chemin de l'arbre appelle quoi.
 *   5. Requêtes IPC         : status, commands, puis les chemins.
 *
 * Convention des handlers (CommandHandler, command/command.h) :
 *   - argc == 1 : argv[0] est la valeur à appliquer, déjà validée ;
 *   - argc == 0 : action (ex. Radio/>>) ou lecture d'un groupe ;
 *   - context   : un HandlerContext ; en cas d'échec, le handler y
 *                 décrit l'erreur et retourne -1.
 */

#define _POSIX_C_SOURCE 200809L

#include "app/handlers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "command/registry.h"
#include "core/control.h"

/* ------------------------------------------------------------------------- */
/* Contexte et erreurs                                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    SweetDawn *app;
    struct json_object *result;   /* NULL : la réponse sera {} */
    const char *error_code;
    char error_message[256];
} HandlerContext;

static int echec(HandlerContext *ctx, const char *code, const char *message)
{
    ctx->error_code = code;
    snprintf(ctx->error_message, sizeof(ctx->error_message), "%s", message);
    return -1;
}

/* Traduit un code AML_ERR_* en erreur IPC. 0 est renvoyé tel quel. */
static int verifier(HandlerContext *ctx, int code)
{
    switch (code) {
    case 0:
        return 0;
    case AML_ERR_DISCONNECTED:
        return echec(ctx, "AML_DISCONNECTED",
                     "L'AML n'est pas connecte pour le moment");
    case AML_ERR_TIMEOUT:
        return echec(ctx, "AML_TIMEOUT", "L'AML n'a pas repondu");
    case AML_ERR_PROTOCOL:
        return echec(ctx, "AML_PROTOCOL_ERROR",
                     "Reponse de l'AML inattendue");
    default:
        return echec(ctx, "AML_ERROR", "Echec de la commande AML");
    }
}

/* ------------------------------------------------------------------------- */
/* Conversions                                                               */
/* ------------------------------------------------------------------------- */

/* Les valeurs ont déjà été validées par l'arbre : pas de cas d'erreur. */
static bool lire_booleen(const char *value)
{
    return strcmp(value, "true") == 0;
}

static uint8_t lire_entier(const char *value)
{
    return (uint8_t)strtol(value, NULL, 10);
}

/* Position de value dans les choix de command (= octet envoyé à l'AML). */
static int index_choix(const Command *command, const char *value)
{
    for (size_t i = 0; i < command->value_spec.choice_count; i++) {
        if (strcmp(command->value_spec.choices[i], value) == 0)
            return (int)i;
    }

    return -1;
}

/*
 * Nom du choix numéro index de la commande enfant child_name de group,
 * ou NULL si l'index ne correspond à aucun choix connu.
 */
static const char *nom_choix(
    const Command *group,
    const char *child_name,
    unsigned index
)
{
    const Command *child = command_find_child(group, child_name);

    if (child == NULL || index >= child->value_spec.choice_count)
        return NULL;

    return child->value_spec.choices[index];
}

/* Ajoute un champ texte, ou null si le texte est inconnu. */
static void ajouter_texte(struct json_object *object, const char *key,
                          const char *text)
{
    json_object_object_add(object, key,
                           text != NULL ? json_object_new_string(text)
                                        : json_object_new_null());
}

/* Lit "HH:MM". Retourne 0 si le format et les valeurs sont corrects. */
static int lire_heure(const char *value, uint8_t *hour, uint8_t *minute)
{
    unsigned h, m;
    char reste;

    if (sscanf(value, "%2u:%2u%c", &h, &m, &reste) != 2 ||
        h > 23 || m > 59)
        return -1;

    *hour = (uint8_t)h;
    *minute = (uint8_t)m;
    return 0;
}

/* Lit "R,G,B". Retourne 0 si les trois composantes vont de 0 à 255. */
static int lire_couleur(const char *value, uint8_t rgb[3])
{
    unsigned r, g, b;
    char reste;

    if (sscanf(value, "%u,%u,%u%c", &r, &g, &b, &reste) != 3 ||
        r > 255 || g > 255 || b > 255)
        return -1;

    rgb[0] = (uint8_t)r;
    rgb[1] = (uint8_t)g;
    rgb[2] = (uint8_t)b;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Handlers : Settings                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Dans les réglages, feedback et fade sont inversés : 0 = activé.
 * C'est ainsi que l'application d'origine les encode.
 */
static struct json_object *reglages_json(const Command *group,
                                         const AmlState *state)
{
    struct json_object *object = json_object_new_object();

    ajouter_texte(object, "display_brightness",
                  nom_choix(group, "Display brightness", state->display));
    ajouter_texte(object, "display_time",
                  nom_choix(group, "Display time", state->date));
    ajouter_texte(object, "time",
                  nom_choix(group, "Time", state->time));
    json_object_object_add(object, "feedback",
                           json_object_new_boolean(state->feedback == 0));
    json_object_object_add(object, "fade",
                           json_object_new_boolean(state->fade == 0));

    return object;
}

/* Settings : relit les réglages sur l'AML. */
static int lire_reglages(const Command *command, int argc,
                         const char *const argv[], void *context)
{
    HandlerContext *ctx = context;
    (void)argc;
    (void)argv;

    if (!sweetdawn_is_connected(ctx->app))
        return verifier(ctx, AML_ERR_DISCONNECTED);

    if (verifier(ctx, sweetdawn_refresh(ctx->app)) != 0)
        return -1;

    ctx->result = reglages_json(command, &ctx->app->state);
    return 0;
}

/*
 * Settings/<réglage> : l'AML n'accepte que les cinq réglages d'un coup,
 * on part donc de l'état connu et on n'en change qu'un.
 */
static int ecrire_reglage(const Command *command, int argc,
                          const char *const argv[], void *context)
{
    HandlerContext *ctx = context;
    const char *name = command->name;
    AmlState settings;
    (void)argc;

    if (!ctx->app->state.known)
        return echec(ctx, "STATE_UNKNOWN",
                     "Reglages pas encore lus : reessayer apres connexion");

    settings = ctx->app->state;

    if (strcmp(name, "Feedback") == 0) {
        settings.feedback = lire_booleen(argv[0]) ? 0 : 1;
    } else if (strcmp(name, "Fade") == 0) {
        settings.fade = lire_booleen(argv[0]) ? 0 : 1;
    } else {
        uint8_t index = (uint8_t)index_choix(command, argv[0]);

        if (strcmp(name, "Display brightness") == 0)
            settings.display = index;
        else if (strcmp(name, "Display time") == 0)
            settings.date = index;
        else
            settings.time = index;
    }

    if (verifier(ctx, control_settings_write(ctx->app, &settings)) != 0)
        return -1;

    /* Comme l'application d'origine : passer en « auto » remet à l'heure. */
    if (strcmp(name, "Time") == 0 && settings.time == 0) {
        time_t now = time(NULL);
        struct tm local;

        localtime_r(&now, &local);
        return verifier(ctx, control_sync_time(ctx->app, &local));
    }

    return 0;
}

/*
 * Envoie une heure à l'AML.
 *
 * En mode « manual » (heure réglée avec les boutons de l'AML), l'AML
 * ignore toute mise à l'heure venue de l'extérieur : on repasse donc
 * d'abord en mode « auto », celui où l'heure est fournie par
 * l'application (constaté sur le matériel).
 */
static int envoyer_heure(HandlerContext *ctx, const struct tm *tm)
{
    if (!ctx->app->state.known)
        return echec(ctx, "STATE_UNKNOWN",
                     "Reglages pas encore lus : reessayer apres connexion");

    if (ctx->app->state.time != 0) {
        AmlState settings = ctx->app->state;

        settings.time = 0;

        if (verifier(ctx, control_settings_write(ctx->app, &settings)) != 0)
            return -1;
    }

    return verifier(ctx, control_sync_time(ctx->app, tm));
}

/* Settings/Sync time : met l'AML à l'heure du Raspberry Pi. */
static int synchroniser_heure(const Command *command, int argc,
                              const char *const argv[], void *context)
{
    HandlerContext *ctx = context;
    time_t now = time(NULL);
    struct tm local;
    (void)command;
    (void)argc;
    (void)argv;

    localtime_r(&now, &local);
    return envoyer_heure(ctx, &local);
}

/*
 * Lit une date et une heure choisies par l'utilisateur :
 *   HH:MM, HH:MM:SS                  (date du jour)
 *   AAAA-MM-JJ HH:MM, AAAA-MM-JJ HH:MM:SS
 * Les champs absents sont pris dans now ; les secondes absentes valent 0.
 * Retourne 0 si la date existe (le 30 février est refusé).
 */
static int lire_date_heure(const char *value, time_t now, struct tm *tm)
{
    int y, mo, d, h, mi, s = 0;
    char reste;
    int n;

    localtime_r(&now, tm);
    y = tm->tm_year + 1900;
    mo = tm->tm_mon + 1;
    d = tm->tm_mday;

    n = sscanf(value, "%4d-%2d-%2d %2d:%2d:%2d%c",
               &y, &mo, &d, &h, &mi, &s, &reste);

    if (n != 6) {
        s = 0;
        n = sscanf(value, "%4d-%2d-%2d %2d:%2d%c", &y, &mo, &d, &h, &mi, &reste);

        if (n != 5) {
            /* Pas de date : reprendre celle du jour (sscanf a pu y écrire). */
            y = tm->tm_year + 1900;
            mo = tm->tm_mon + 1;
            d = tm->tm_mday;

            if (sscanf(value, "%2d:%2d:%2d%c", &h, &mi, &s, &reste) != 3) {
                s = 0;
                if (sscanf(value, "%2d:%2d%c", &h, &mi, &reste) != 2)
                    return -1;
            }
        }
    }

    /* L'AML stocke l'année sur deux chiffres (2000 à 2099). */
    if (y < 2000 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59)
        return -1;

    tm->tm_year = y - 1900;
    tm->tm_mon = mo - 1;
    tm->tm_mday = d;
    tm->tm_hour = h;
    tm->tm_min = mi;
    tm->tm_sec = s;
    tm->tm_isdst = -1;

    /* mktime calcule le jour de la semaine ; une date inexistante
     * (31 avril...) est décalée, ce qui la trahit. */
    if (mktime(tm) == (time_t)-1 || tm->tm_mday != d || tm->tm_mon != mo - 1)
        return -1;

    return 0;
}

/* Settings/Set time : met l'AML à l'heure choisie. */
static int regler_heure(const Command *command, int argc,
                        const char *const argv[], void *context)
{
    HandlerContext *ctx = context;
    struct tm chosen;
    (void)command;
    (void)argc;

    if (lire_date_heure(argv[0], time(NULL), &chosen) != 0)
        return echec(ctx, "INVALID_VALUE",
                     "Heure attendue : HH:MM, HH:MM:SS ou AAAA-MM-JJ HH:MM");

    return envoyer_heure(ctx, &chosen);
}

/* ------------------------------------------------------------------------- */
/* Handlers : Alarm                                                          */
/* ------------------------------------------------------------------------- */

/* Groupe "1" ou "2" (Alarm/1, Alarm/2) -> numéro de l'alarme. */
static int numero_alarme(const Command *alarm_group)
{
    return strcmp(alarm_group->name, "2") == 0 ? 2 : 1;
}

static struct json_object *alarme_json(const Command *group,
                                       const ControlAlarm *alarm)
{
    struct json_object *object = json_object_new_object();
    char heure[6];

    snprintf(heure, sizeof(heure), "%02u:%02u",
             (unsigned)(alarm->hour % 24), (unsigned)(alarm->minute % 60));

    json_object_object_add(object, "enable",
                           json_object_new_boolean(alarm->enable));
    json_object_object_add(object, "time", json_object_new_string(heure));
    json_object_object_add(object, "days", json_object_new_int(alarm->days));
    ajouter_texte(object, "snooze", nom_choix(group, "Snooze", alarm->snooze));
    ajouter_texte(object, "tone", nom_choix(group, "Tone", alarm->tone));
    json_object_object_add(object, "tone_volume",
                           json_object_new_int(alarm->volume));
    json_object_object_add(object, "sunrise",
                           json_object_new_boolean(alarm->sunrise_enable));
    json_object_object_add(object, "sunrise_duration",
                           json_object_new_int(alarm->sunrise_minutes));
    json_object_object_add(object, "sunrise_brightness",
                           json_object_new_int(alarm->sunrise_brightness));

    return object;
}

/* Alarm/Alarm N : lit une alarme. */
static int lire_alarme(const Command *command, int argc,
                       const char *const argv[], void *context)
{
    HandlerContext *ctx = context;
    ControlAlarm alarm;
    (void)argc;
    (void)argv;

    int code = control_alarm_query(ctx->app, numero_alarme(command), &alarm);

    if (verifier(ctx, code) != 0)
        return -1;

    ctx->result = alarme_json(command, &alarm);
    return 0;
}

/* Alarm : lit les deux alarmes. */
static int lire_alarmes(const Command *command, int argc,
                        const char *const argv[], void *context)
{
    HandlerContext *ctx = context;
    struct json_object *object = json_object_new_object();
    (void)argc;
    (void)argv;

    for (int number = 1; number <= 2; number++) {
        const char *name = number == 1 ? "1" : "2";
        const Command *group = command_find_child(command, name);
        ControlAlarm alarm;

        int code = control_alarm_query(ctx->app, number, &alarm);

        if (verifier(ctx, code) != 0) {
            json_object_put(object);
            return -1;
        }

        json_object_object_add(object, name, alarme_json(group, &alarm));
    }

    ctx->result = object;
    return 0;
}

/*
 * Alarm/Alarm N/<réglage> : lit l'alarme, change un réglage, la réécrit
 * (l'AML n'accepte qu'une alarme complète).
 */
static int ecrire_alarme(const Command *command, int argc,
                         const char *const argv[], void *context)
{
    HandlerContext *ctx = context;
    const char *name = command->name;
    const char *value = argv[0];
    int number = numero_alarme(command->parent);
    ControlAlarm alarm;
    uint8_t hour = 0, minute = 0;
    (void)argc;

    /* Vérifier le format avant de déranger l'AML. */
    if (strcmp(name, "Time") == 0 && lire_heure(value, &hour, &minute) != 0)
        return echec(ctx, "INVALID_VALUE", "Heure attendue au format HH:MM");

    if (verifier(ctx, control_alarm_query(ctx->app, number, &alarm)) != 0)
        return -1;

    if (strcmp(name, "Enable") == 0) {
        alarm.enable = lire_booleen(value);
    } else if (strcmp(name, "Time") == 0) {
        alarm.hour = hour;
        alarm.minute = minute;
    } else if (strcmp(name, "Days") == 0) {
        alarm.days = lire_entier(value);
    } else if (strcmp(name, "Snooze") == 0) {
        alarm.snooze = (uint8_t)index_choix(command, value);
    } else if (strcmp(name, "Tone") == 0) {
        alarm.tone = (uint8_t)index_choix(command, value);
    } else if (strcmp(name, "Tone volume") == 0) {
        alarm.volume = lire_entier(value);
    } else if (strcmp(name, "Sunrise") == 0) {
        alarm.sunrise_enable = lire_booleen(value);
    } else if (strcmp(name, "Sunrise duration") == 0) {
        alarm.sunrise_minutes = lire_entier(value);
    } else {
        alarm.sunrise_brightness = lire_entier(value);
    }

    return verifier(ctx, control_alarm_write(ctx->app, number, &alarm));
}

/* Alarm/Stop et Alarm/Snooze. */
static int agir_sur_alarme(const Command *command, int argc,
                           const char *const argv[], void *context)
{
    HandlerContext *ctx = context;
    (void)argc;
    (void)argv;

    if (strcmp(command->name, "Stop") == 0)
        return verifier(ctx, control_alarm_stop(ctx->app));

    return verifier(ctx, control_alarm_snooze(ctx->app));
}

/* ------------------------------------------------------------------------- */
/* Handlers : Light et Mood                                                  */
/* ------------------------------------------------------------------------- */

/* Le groupe (Light ou Mood) détermine la source lumineuse visée. */
static ControlLightTarget cible(const Command *group)
{
    return strcmp(group->name, "Mood") == 0 ? CONTROL_MOOD : CONTROL_LIGHT;
}

/* Light, Mood : lit l'état de la source lumineuse. */
static int lire_lumiere(const Command *command, int argc,
                        const char *const argv[], void *context)
{
    HandlerContext *ctx = context;
    ControlLightTarget target = cible(command);
    ControlLight light;
    (void)argc;
    (void)argv;

    if (verifier(ctx, control_light_query(ctx->app, target, &light)) != 0)
        return -1;

    struct json_object *object = json_object_new_object();

    json_object_object_add(object, "enable",
                           json_object_new_boolean(light.enable));
    json_object_object_add(object, "brightness",
                           json_object_new_int(light.brightness));

    if (target == CONTROL_LIGHT) {
        json_object_object_add(object, "sunset",
                               json_object_new_boolean(light.timer_enable));
        json_object_object_add(object, "sunset_duration",
                               json_object_new_int(light.timer_minutes));
    } else {
        char couleur[12];

        snprintf(couleur, sizeof(couleur), "%u,%u,%u",
                 light.red, light.green, light.blue);

        json_object_object_add(object, "timer",
                               json_object_new_boolean(light.timer_enable));
        json_object_object_add(object, "timer_duration",
                               json_object_new_int(light.timer_minutes));
        json_object_object_add(object, "color",
                               json_object_new_string(couleur));
        ajouter_texte(object, "scene",
                      nom_choix(command, "Scene", light.scene));
    }

    ctx->result = object;
    return 0;
}

/*
 * Light/<réglage>, Mood/<réglage>.
 * Sunset (Light) et Timer (Mood) sont la même commande AML ; de même
 * pour Sunset duration et Timer duration.
 */
static int ecrire_lumiere(const Command *command, int argc,
                          const char *const argv[], void *context)
{
    HandlerContext *ctx = context;
    ControlLightTarget target = cible(command->parent);
    const char *name = command->name;
    const char *value = argv[0];
    int code;
    (void)argc;

    if (strcmp(name, "Enable") == 0) {
        code = control_light_enable(ctx->app, target, lire_booleen(value));
    } else if (strcmp(name, "Brightness") == 0) {
        code = control_light_brightness(ctx->app, target, lire_entier(value));
    } else if (strcmp(name, "Sunset") == 0 || strcmp(name, "Timer") == 0) {
        code = control_light_timer_enable(ctx->app, target,
                                          lire_booleen(value));
    } else if (strcmp(name, "Sunset duration") == 0 ||
               strcmp(name, "Timer duration") == 0) {
        code = control_light_timer(ctx->app, target, lire_entier(value));
    } else if (strcmp(name, "Color") == 0) {
        uint8_t rgb[3];

        if (lire_couleur(value, rgb) != 0)
            return echec(ctx, "INVALID_VALUE",
                         "Couleur attendue au format R,G,B (0 a 255)");

        code = control_mood_color(ctx->app, rgb[0], rgb[1], rgb[2]);
    } else {
        code = control_mood_scene(ctx->app,
                                  (uint8_t)index_choix(command, value));
    }

    return verifier(ctx, code);
}

/* ------------------------------------------------------------------------- */
/* Handlers : Radio                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Fréquence en dixièmes de MHz -> nombre en MHz (1015 -> 101.5).
 * Le texte est fourni explicitement : sinon json-c écrirait
 * 89.200000000000003.
 */
static struct json_object *frequence_json(uint16_t frequency)
{
    char text[8];

    snprintf(text, sizeof(text), "%u.%u", frequency / 10u, frequency % 10u);
    return json_object_new_double_s(frequency / 10.0, text);
}

/* Radio : lit l'état de la radio. */
static int lire_radio(const Command *command, int argc,
                      const char *const argv[], void *context)
{
    HandlerContext *ctx = context;
    ControlRadio radio;
    (void)command;
    (void)argc;
    (void)argv;

    if (verifier(ctx, control_radio_query(ctx->app, &radio)) != 0)
        return -1;

    struct json_object *object = json_object_new_object();

    json_object_object_add(object, "enable",
                           json_object_new_boolean(radio.enable));
    json_object_object_add(object, "channel",
                           json_object_new_int(radio.channel));
    json_object_object_add(object, "frequency",
                           frequence_json(radio.frequency));
    json_object_object_add(object, "volume",
                           json_object_new_int(radio.volume));
    json_object_object_add(object, "sleep_timer",
                           json_object_new_boolean(radio.timer_enable));
    json_object_object_add(object, "timer_duration",
                           json_object_new_int(radio.timer_minutes));

    ctx->result = object;
    return 0;
}

/* Radio/<, >, <<, >> : renvoie la nouvelle fréquence. */
static int changer_frequence(const Command *command, int argc,
                             const char *const argv[], void *context)
{
    HandlerContext *ctx = context;
    const char *name = command->name;
    ControlTune direction;
    uint16_t frequency = 0;
    (void)argc;
    (void)argv;

    if (strcmp(name, ">") == 0)
        direction = CONTROL_TUNE_UP;
    else if (strcmp(name, "<") == 0)
        direction = CONTROL_TUNE_DOWN;
    else if (strcmp(name, ">>") == 0)
        direction = CONTROL_SEEK_UP;
    else
        direction = CONTROL_SEEK_DOWN;

    int code = control_radio_tune(ctx->app, direction, &frequency);

    if (verifier(ctx, code) != 0)
        return -1;

    ctx->result = json_object_new_object();
    json_object_object_add(ctx->result, "frequency",
                           frequence_json(frequency));
    return 0;
}

/* Radio/<réglage>. */
static int ecrire_radio(const Command *command, int argc,
                        const char *const argv[], void *context)
{
    HandlerContext *ctx = context;
    const char *name = command->name;
    const char *value = argv[0];
    int code;
    (void)argc;

    if (strcmp(name, "Enable") == 0)
        code = control_radio_enable(ctx->app, lire_booleen(value));
    else if (strcmp(name, "Channel") == 0)
        code = control_radio_preset(ctx->app, lire_entier(value));
    else if (strcmp(name, "Volume") == 0)
        code = control_radio_volume(ctx->app, lire_entier(value));
    else if (strcmp(name, "Sleep timer") == 0)
        code = control_radio_timer_enable(ctx->app, lire_booleen(value));
    else
        code = control_radio_timer(ctx->app, lire_entier(value));

    return verifier(ctx, code);
}

/* ------------------------------------------------------------------------- */
/* Handlers : Media Player                                                   */
/* ------------------------------------------------------------------------- */

static int ecrire_haut_parleur(const Command *command, int argc,
                               const char *const argv[], void *context)
{
    HandlerContext *ctx = context;
    (void)command;
    (void)argc;

    return verifier(ctx, control_speaker_enable(ctx->app,
                                                lire_booleen(argv[0])));
}

/* ------------------------------------------------------------------------- */
/* Table des handlers                                                        */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *path;
    CommandHandler handler;
} Liaison;

/*
 * Chaque chemin de l'arbre et le handler qui l'exécute. Un chemin absent
 * d'ici répond COMMAND_NOT_IMPLEMENTED.
 *
 * Les réglages des deux alarmes sont ajoutés par handlers_attach() :
 * voir REGLAGES_ALARME.
 */
static const Liaison LIAISONS[] = {
    { "Settings",                    lire_reglages },
    { "Settings/Display brightness", ecrire_reglage },
    { "Settings/Display time",       ecrire_reglage },
    { "Settings/Time",               ecrire_reglage },
    { "Settings/Feedback",           ecrire_reglage },
    { "Settings/Fade",               ecrire_reglage },
    { "Settings/Sync time",          synchroniser_heure },
    { "Settings/Set time",           regler_heure },

    { "Alarm",                       lire_alarmes },
    { "Alarm/1",               lire_alarme },
    { "Alarm/2",               lire_alarme },
    { "Alarm/Stop",                  agir_sur_alarme },
    { "Alarm/Snooze",                agir_sur_alarme },

    { "Light",                       lire_lumiere },
    { "Light/Enable",                ecrire_lumiere },
    { "Light/Brightness",            ecrire_lumiere },
    { "Light/Sunset",                ecrire_lumiere },
    { "Light/Sunset duration",       ecrire_lumiere },

    { "Mood",                        lire_lumiere },
    { "Mood/Enable",                 ecrire_lumiere },
    { "Mood/Color",                  ecrire_lumiere },
    { "Mood/Brightness",             ecrire_lumiere },
    { "Mood/Timer",                  ecrire_lumiere },
    { "Mood/Timer duration",         ecrire_lumiere },
    { "Mood/Scene",                  ecrire_lumiere },

    { "Radio",                       lire_radio },
    { "Radio/Enable",                ecrire_radio },
    { "Radio/<<",                    changer_frequence },
    { "Radio/<",                     changer_frequence },
    { "Radio/>",                     changer_frequence },
    { "Radio/>>",                    changer_frequence },
    { "Radio/Channel",               ecrire_radio },
    { "Radio/Volume",                ecrire_radio },
    { "Radio/Sleep timer",           ecrire_radio },
    { "Radio/Timer duration",        ecrire_radio },

    { "Media Player/Enable",         ecrire_haut_parleur },
};

/* Réglages présents sous Alarm/1 et Alarm/2. */
static const char *const REGLAGES_ALARME[] = {
    "Enable", "Time", "Days", "Snooze", "Tone", "Tone volume",
    "Sunrise", "Sunrise duration", "Sunrise brightness"
};

static int lier(CommandRegistry *registry, const char *path,
                CommandHandler handler)
{
    Command *command = registry_find_path(registry, path);

    if (command == NULL) {
        fprintf(stderr, "Handlers : commande introuvable : %s\n", path);
        return -1;
    }

    command->handler = handler;
    return 0;
}

int handlers_attach(SweetDawn *app)
{
    CommandRegistry *registry;
    char path[64];

    if (app == NULL || !app->initialized)
        return -1;

    registry = &app->commands.registry;

    for (size_t i = 0; i < sizeof(LIAISONS) / sizeof(LIAISONS[0]); i++) {
        if (lier(registry, LIAISONS[i].path, LIAISONS[i].handler) != 0)
            return -1;
    }

    for (int number = 1; number <= 2; number++) {
        for (size_t i = 0;
             i < sizeof(REGLAGES_ALARME) / sizeof(REGLAGES_ALARME[0]);
             i++) {
            snprintf(path, sizeof(path), "Alarm/%d/%s",
                     number, REGLAGES_ALARME[i]);

            if (lier(registry, path, ecrire_alarme) != 0)
                return -1;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Requêtes IPC                                                              */
/* ------------------------------------------------------------------------- */

/* Copie un texte dans un tampon d'erreur fourni par le serveur IPC. */
static void set_text(char *buffer, size_t capacity, const char *text)
{
    if (buffer != NULL && capacity > 0)
        snprintf(buffer, capacity, "%s", text);
}

/*
 * "status" : état mémorisé, sans interroger l'AML (la lecture est faite
 * en tâche de fond par sweetdawn_tick). La réponse est donc immédiate,
 * même si l'AML est injoignable.
 *
 *   connected : la liaison Bluetooth est-elle ouverte en ce moment ?
 *   known     : a-t-on déjà lu l'état au moins une fois ?
 *   age_ms    : ancienneté de la dernière lecture. Pendant une panne,
 *               les valeurs restent celles de la dernière lecture :
 *               c'est age_ms qui permet de savoir si elles sont fraîches.
 */
static struct json_object *status_json(SweetDawn *app)
{
    struct json_object *object = json_object_new_object();
    const AmlState *state = &app->state;

    json_object_object_add(object, "connected",
                           json_object_new_boolean(sweetdawn_is_connected(app)));
    json_object_object_add(object, "known",
                           json_object_new_boolean(state->known));

    if (state->known) {
        const Command *settings =
            registry_find(&app->commands.registry, "Settings");

        json_object_object_add(object, "age_ms",
                               json_object_new_int64(sweetdawn_state_age_ms(app)));
        json_object_object_add(object, "authorization",
                               json_object_new_int(app->authorization));
        json_object_object_add(object, "settings",
                               reglages_json(settings, state));
    }

    return object;
}

/*
 * Décrit une commande et ses enfants, pour "commands" : de quoi
 * construire une interface sans connaître l'arbre à l'avance.
 */
static struct json_object *commande_json(const Command *command)
{
    static const char *const TYPES[] = {
        "none", "bool", "integer", "number", "string"
    };
    const CommandValueSpec *spec = &command->value_spec;
    struct json_object *object = json_object_new_object();

    json_object_object_add(object, "name",
                           json_object_new_string(command->name));

    if (command->description != NULL)
        json_object_object_add(object, "description",
                               json_object_new_string(command->description));

    json_object_object_add(object, "type",
                           json_object_new_string(TYPES[spec->type]));

    if (spec->has_range) {
        json_object_object_add(object, "min",
                               json_object_new_int((int)spec->minimum));
        json_object_object_add(object, "max",
                               json_object_new_int((int)spec->maximum));
    }

    if (spec->choice_count > 0) {
        struct json_object *choices = json_object_new_array();

        for (size_t i = 0; i < spec->choice_count; i++)
            json_object_array_add(choices,
                                  json_object_new_string(spec->choices[i]));

        json_object_object_add(object, "choices", choices);
    }

    json_object_object_add(object, "implemented",
                           json_object_new_boolean(command->handler != NULL));

    if (command->child_count > 0) {
        struct json_object *children = json_object_new_array();

        for (size_t i = 0; i < command->child_count; i++)
            json_object_array_add(children,
                                  commande_json(command->children[i]));

        json_object_object_add(object, "children", children);
    }

    return object;
}

int handlers_request(
    const char *name,
    struct json_object *args,
    struct json_object **result,
    char *error_code,
    size_t error_code_capacity,
    char *error_message,
    size_t error_message_capacity,
    void *context)
{
    SweetDawn *app = context;
    size_t argc = json_object_array_length(args);

    if (strcmp(name, "status") == 0 || strcmp(name, "commands") == 0) {
        if (argc != 0) {
            set_text(error_code, error_code_capacity, "INVALID_ARGUMENTS");
            set_text(error_message, error_message_capacity,
                     "Cette commande n'accepte aucun argument");
            return -1;
        }

        *result = strcmp(name, "status") == 0
                  ? status_json(app)
                  : commande_json(&app->commands.registry.root);
        return 0;
    }

    Command *command = registry_find_path(&app->commands.registry, name);

    if (command == NULL) {
        set_text(error_code, error_code_capacity, "UNKNOWN_COMMAND");
        set_text(error_message, error_message_capacity,
                 "Commande inconnue (voir la commande \"commands\")");
        return -1;
    }

    /* Une valeur pour les réglages, aucune pour les actions et groupes. */
    size_t expected = command->value_spec.type == COMMAND_VALUE_NONE ? 0 : 1;

    if (argc != expected) {
        set_text(error_code, error_code_capacity, "INVALID_ARGUMENTS");
        set_text(error_message, error_message_capacity,
                 expected == 0 ? "Cette commande n'accepte aucun argument"
                               : "Cette commande attend une valeur");
        return -1;
    }

    /*
     * La valeur peut arriver en texte, nombre ou booléen JSON :
     * json-c la convertit en texte ("75", "true"...), puis l'arbre la
     * valide comme si elle avait été tapée en ligne de commande.
     */
    const char *value = NULL;

    if (expected == 1) {
        value = json_object_get_string(json_object_array_get_idx(args, 0));

        if (value == NULL ||
            command_validate_value(command, value, error_message,
                                   error_message_capacity) != 0) {
            set_text(error_code, error_code_capacity, "INVALID_VALUE");
            return -1;
        }
    }

    if (command->handler == NULL) {
        set_text(error_code, error_code_capacity, "COMMAND_NOT_IMPLEMENTED");
        set_text(error_message, error_message_capacity,
                 "Cette commande n'est pas encore implementee");
        return -1;
    }

    HandlerContext ctx = { .app = app };
    const char *argv[1] = { value };

    if (command_execute(command, (int)expected, argv, &ctx) != 0) {
        set_text(error_code, error_code_capacity,
                 ctx.error_code != NULL ? ctx.error_code : "COMMAND_FAILED");
        set_text(error_message, error_message_capacity,
                 ctx.error_message[0] != '\0' ? ctx.error_message
                                              : "La commande a echoue");

        if (ctx.result != NULL)
            json_object_put(ctx.result);

        return -1;
    }

    *result = ctx.result != NULL ? ctx.result : json_object_new_object();
    return 0;
}
