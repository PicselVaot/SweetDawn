/*
 * app/handlers.c — Commandes IPC du service
 *
 * Organisation du fichier :
 *   1. Contexte et erreurs  : ce qu'un handler reçoit et renvoie.
 *   2. Conversions          : texte <-> valeurs, choix <-> index.
 *   3. Handlers             : un groupe de l'arbre après l'autre ;
 *                             la section Settings gère aussi l'horloge
 *                             de l'AML (heure connue, remise à l'heure)
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

/* Définie plus bas, avec la gestion de l'horloge de l'AML. */
static int envoyer_heure(HandlerContext *ctx, const struct tm *tm);

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
        return envoyer_heure(ctx, &local);
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Horloge de l'AML                                                          */
/* ------------------------------------------------------------------------- */
/*
 * L'AML ne permet pas de relire son heure. On la connaît quand même :
 * c'est SweetDawn qui la règle. On retient l'écart avec l'horloge du
 * système (champs clock_* de SweetDawn) ; l'heure de l'AML est alors
 * l'heure du système plus cet écart.
 */

/* Remise à l'heure automatique : une fois par jour, en mode auto. */
#define CLOCK_RESYNC_MS (24LL * 3600 * 1000)

/* Après un échec, nouvel essai dans 10 minutes. */
#define CLOCK_RETRY_MS (10LL * 60 * 1000)

/* Retient que l'AML vient d'être réglé sur tm (heure locale). */
static void noter_heure(SweetDawn *app, const struct tm *tm)
{
    struct tm copie = *tm;
    int64_t offset;

    copie.tm_isdst = -1;
    offset = (int64_t)(mktime(&copie) - time(NULL));

    /* Mise à l'heure du système : l'écart d'une seconde vient de l'arrondi. */
    if (offset >= -1 && offset <= 1)
        offset = 0;

    app->clock_set = true;
    app->clock_offset_s = offset;
    app->clock_synced_ms = sweetdawn_now_ms(app);
}

/*
 * Heure actuelle de l'AML, si elle est connue : réglée par SweetDawn
 * depuis son démarrage, et mode auto (en manual, elle a pu être changée
 * aux boutons de l'AML).
 */
static bool horloge_aml(const SweetDawn *app, time_t *aml_now)
{
    if (!app->clock_set || !app->state.known || app->state.time != 0)
        return false;

    *aml_now = time(NULL) + (time_t)app->clock_offset_s;
    return true;
}

/* Écrit t (heure locale) au format ISO 8601 avec fuseau : 2026-10-12T07:00:00+02:00 */
static void format_iso(time_t t, char *buffer, size_t capacity)
{
    struct tm local;
    char zone[8];

    localtime_r(&t, &local);
    strftime(buffer, capacity, "%Y-%m-%dT%H:%M:%S", &local);
    strftime(zone, sizeof(zone), "%z", &local);   /* +0200 */

    size_t used = strlen(buffer);
    snprintf(buffer + used, capacity - used, "%.3s:%.2s", zone, zone + 3);
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

    if (verifier(ctx, control_sync_time(ctx->app, tm)) != 0)
        return -1;

    noter_heure(ctx->app, tm);
    return 0;
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

/* Définies plus bas, avec le suivi du réveil (section Événements). */
static void surveiller_alarme_bientot(void);
static void noter_arret_demande(void);

/* Groupe "1" ou "2" (Alarm/1, Alarm/2) -> numéro de l'alarme. */
static int numero_alarme(const Command *alarm_group)
{
    return strcmp(alarm_group->name, "2") == 0 ? 2 : 1;
}

/*
 * Délai avant le prochain déclenchement d'une alarme, selon l'horloge
 * de l'AML. Faux si l'alarme est désactivée ou l'horloge inconnue.
 *
 * Jours : bit 0 = dimanche ... bit 6 = samedi, la même convention que
 * tm_wday. Aucun jour coché : prochaine occurrence de l'heure (à
 * confirmer sur le matériel).
 */
static bool prochaine_alarme(const SweetDawn *app, const ControlAlarm *alarm,
                             int64_t *delay_s)
{
    time_t aml_now;
    struct tm today;

    if (!alarm->enable || !horloge_aml(app, &aml_now))
        return false;

    localtime_r(&aml_now, &today);

    /* Aujourd'hui (si l'heure n'est pas passée), puis les 7 jours suivants. */
    for (int k = 0; k <= 7; k++) {
        struct tm candidate = today;

        candidate.tm_mday += k;
        candidate.tm_hour = alarm->hour;
        candidate.tm_min = alarm->minute;
        candidate.tm_sec = 0;
        candidate.tm_isdst = -1;

        time_t t = mktime(&candidate);   /* normalise et calcule tm_wday */

        if (t == (time_t)-1 || t <= aml_now)
            continue;

        if ((alarm->days & 0x7F) == 0 ||
            (alarm->days & (1u << candidate.tm_wday)) != 0) {
            *delay_s = (int64_t)(t - aml_now);
            return true;
        }
    }

    return false;
}

/* Délai lisible : "11 h 05 min", "2 j 3 h 00 min". */
static void format_delai(int64_t seconds, char *buffer, size_t capacity)
{
    long long minutes = (seconds + 59) / 60;
    long long days = minutes / (24 * 60);
    long long hours = minutes / 60 % 24;

    minutes %= 60;

    if (days > 0)
        snprintf(buffer, capacity, "%lld j %lld h %02lld min", days, hours, minutes);
    else
        snprintf(buffer, capacity, "%lld h %02lld min", hours, minutes);
}

static struct json_object *alarme_json(const SweetDawn *app,
                                       const Command *group,
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

    /*
     * Prochain déclenchement, en heure réelle (celle du système) : c'est
     * le moment où l'horloge de l'AML atteindra l'heure de l'alarme.
     */
    int64_t delay_s;

    if (prochaine_alarme(app, alarm, &delay_s)) {
        char next[40];
        char in[32];

        format_iso(time(NULL) + (time_t)delay_s, next, sizeof(next));
        format_delai(delay_s, in, sizeof(in));

        json_object_object_add(object, "next", json_object_new_string(next));
        json_object_object_add(object, "in_s", json_object_new_int64(delay_s));
        json_object_object_add(object, "in", json_object_new_string(in));
    } else {
        json_object_object_add(object, "next", json_object_new_null());
    }

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

    ctx->result = alarme_json(ctx->app, command, &alarm);
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

        json_object_object_add(object, name,
                               alarme_json(ctx->app, group, &alarm));
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

    bool stop = strcmp(command->name, "Stop") == 0;
    int code = stop ? control_alarm_stop(ctx->app)
                    : control_alarm_snooze(ctx->app);

    /* L'état du réveil vient de changer : le relire au prochain tour. */
    surveiller_alarme_bientot();

    if (stop && code == 0)
        noter_arret_demande();

    return verifier(ctx, code);
}

/* ------------------------------------------------------------------------- */
/* Handlers : Light et Mood                                                  */
/* ------------------------------------------------------------------------- */

/* Le groupe (Light ou Mood) détermine la source lumineuse visée. */
static ControlLightTarget cible(const Command *group)
{
    return strcmp(group->name, "Mood") == 0 ? CONTROL_MOOD : CONTROL_LIGHT;
}

/* État d'une source lumineuse en JSON ; group : Light ou Mood. */
static struct json_object *lumiere_json(const Command *group,
                                        const ControlLight *light)
{
    ControlLightTarget target = cible(group);
    struct json_object *object = json_object_new_object();

    json_object_object_add(object, "enable",
                           json_object_new_boolean(light->enable));
    json_object_object_add(object, "brightness",
                           json_object_new_int(light->brightness));

    if (target == CONTROL_LIGHT) {
        json_object_object_add(object, "sunset",
                               json_object_new_boolean(light->timer_enable));
        json_object_object_add(object, "sunset_duration",
                               json_object_new_int(light->timer_minutes));
    } else {
        char couleur[12];

        snprintf(couleur, sizeof(couleur), "%u,%u,%u",
                 light->red, light->green, light->blue);

        json_object_object_add(object, "timer",
                               json_object_new_boolean(light->timer_enable));
        json_object_object_add(object, "timer_duration",
                               json_object_new_int(light->timer_minutes));
        json_object_object_add(object, "color",
                               json_object_new_string(couleur));
        ajouter_texte(object, "scene",
                      nom_choix(group, "Scene", light->scene));
    }

    return object;
}

/* Light, Mood : lit l'état de la source lumineuse. */
static int lire_lumiere(const Command *command, int argc,
                        const char *const argv[], void *context)
{
    HandlerContext *ctx = context;
    ControlLight light;
    (void)argc;
    (void)argv;

    if (verifier(ctx, control_light_query(ctx->app, cible(command), &light)) != 0)
        return -1;

    ctx->result = lumiere_json(command, &light);
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
/* Événements                                                                */
/* ------------------------------------------------------------------------- */
/*
 * Messages spontanés de l'AML -> événements pour les clients abonnés.
 *
 * Le cœur appelle handlers_on_event() dès qu'un message arrive, parfois
 * au milieu d'une requête : on se contente de le noter. handlers_tick()
 * le traite ensuite (avec relectures si besoin), puis le diffuse :
 *
 *   {"type":"event","event":"alarm_state",
 *    "data":{"state":"ringing","alarm":2,"alarms":{"1":"idle","2":"ringing"}}}
 *   {"type":"event","event":"light_changed","data":{"light":{...},"mood":{...}}}
 *   {"type":"event","event":"timer_ended","data":{"timer":"radio"}}
 *
 * État du réveil : l'AML signale le début du lever de soleil (C6) et la
 * sonnerie (C7), mais rien quand on le tape (snooze) ou qu'on l'arrête
 * aux boutons. Tant qu'une alarme est en cours, on relit donc son état
 * (code 25) toutes les ALARM_WATCH_MS pour suivre :
 * idle -> sunrise -> ringing -> snoozed -> ringing -> ... -> idle.
 * Les deux alarmes sont suivies séparément : elles peuvent se chevaucher
 * (l'une sonne pendant le lever de soleil de l'autre).
 *
 * Snooze ou arrêt ? Quand la sonnerie cesse, l'état relu ne permet pas
 * toujours de les distinguer (constaté sur le matériel : « rien en
 * cours » dans les deux cas). On passe alors en « snoozed » pour la
 * durée de snooze de l'alarme, plus une marge : si elle resonne, on le
 * verra ; sinon c'était un arrêt, et on repasse en « idle ». Un arrêt
 * demandé par Alarm/Stop, lui, est connu : « idle » tout de suite.
 */

#define ALARM_WATCH_MS 1000
#define EVENT_QUEUE_SIZE 16

/* Marge après la fin du snooze avant de conclure à un arrêt. */
#define SNOOZE_MARGIN_MS 60000

/* Durées de snooze, dans l'ordre des choix Alarm/<n>/Snooze. */
static const int SNOOZE_MINUTES[] = { 1, 2, 5, 10, 20, 30 };

typedef enum {
    REVEIL_REPOS,      /* rien en cours            */
    REVEIL_LEVER,      /* lever de soleil en cours */
    REVEIL_SONNE,      /* l'alarme sonne           */
    REVEIL_SNOOZE      /* sonnerie reportée        */
} EtatReveil;

/* Noms diffusés aux clients, dans l'ordre de EtatReveil. */
static const char *const NOMS_REVEIL[] = { "idle", "sunrise", "ringing", "snoozed" };

/* Suivi d'une alarme. */
typedef struct {
    EtatReveil etat;
    bool a_sonne;                /* a sonné depuis le dernier repos :  */
                                 /* distingue un snooze d'un lever     */
    int64_t fin_snooze_ms;       /* au-delà, un snooze sans resonner   */
                                 /* était en fait un arrêt             */
} SuiviAlarme;

static struct {
    /* Messages notés par handlers_on_event, pas encore traités. */
    struct { uint8_t code; uint8_t value; } file[EVENT_QUEUE_SIZE];
    size_t taille;
    bool eclairage_change;       /* au moins un CA reçu */

    /* Réveil : un suivi par alarme (les deux peuvent se chevaucher). */
    SuiviAlarme alarmes[2];      /* [0] : alarme 1, [1] : alarme 2     */
    bool reveil_change;          /* à diffuser en fin de traitement    */
    bool surveiller;             /* relire l'état régulièrement        */
    int64_t prochaine_lecture_ms;
    bool arret_demande;          /* Alarm/Stop envoyé par SweetDawn    */
    unsigned connexions;         /* pour relire l'état après reconnexion */

    /* Diffusion aux clients. */
    HandlersBroadcast diffuser;
    void *contexte;
} evenements;

void handlers_on_event(void *context, uint8_t code,
                       const uint8_t *data, size_t length)
{
    (void)context;

    /* Plusieurs CA de suite (gestes) : une seule relecture suffit. */
    if (code == AML_EVT_LIGHT_REFRESH) {
        evenements.eclairage_change = true;
        return;
    }

    if (evenements.taille < EVENT_QUEUE_SIZE) {
        evenements.file[evenements.taille].code = code;
        evenements.file[evenements.taille].value = length > 0 ? data[0] : 0;
        evenements.taille++;
    }
}

void handlers_set_broadcast(HandlersBroadcast broadcast, void *context)
{
    evenements.diffuser = broadcast;
    evenements.contexte = context;
}

/* Diffuse un événement ; data est pris en charge (libéré ici). */
static void diffuser(const char *name, struct json_object *data)
{
    struct json_object *event = json_object_new_object();

    json_object_object_add(event, "type", json_object_new_string("event"));
    json_object_object_add(event, "event", json_object_new_string(name));
    json_object_object_add(event, "data", data);

    printf("SweetDawn : événement %s %s\n", name,
           json_object_to_json_string_ext(data, JSON_C_TO_STRING_PLAIN));

    if (evenements.diffuser != NULL)
        evenements.diffuser(evenements.contexte, event);

    json_object_put(event);
}

/* Priorité pour le résumé : sonne > snooze > lever de soleil > repos. */
static int priorite(EtatReveil etat)
{
    static const int PRIORITE[] = { 0, 1, 3, 2 };   /* ordre de EtatReveil */
    return PRIORITE[etat];
}

/*
 * {"state": "ringing", "alarm": 1, "alarms": {"1": "ringing", "2": "sunrise"}}
 *
 * state / alarm : l'alarme la plus avancée (alarm vaut null au repos) ;
 * alarms : l'état de chacune.
 */
static struct json_object *reveil_json(void)
{
    struct json_object *object = json_object_new_object();
    struct json_object *alarms = json_object_new_object();
    int principale = 0;

    for (int n = 1; n <= 2; n++) {
        EtatReveil etat = evenements.alarmes[n - 1].etat;
        char key[2] = { (char)('0' + n), '\0' };

        json_object_object_add(alarms, key,
                               json_object_new_string(NOMS_REVEIL[etat]));

        if (etat != REVEIL_REPOS &&
            (principale == 0 ||
             priorite(etat) > priorite(evenements.alarmes[principale - 1].etat)))
            principale = n;
    }

    json_object_object_add(object, "state", json_object_new_string(
        NOMS_REVEIL[principale ? evenements.alarmes[principale - 1].etat
                               : REVEIL_REPOS]));
    json_object_object_add(object, "alarm", principale
                           ? json_object_new_int(principale)
                           : json_object_new_null());
    json_object_object_add(object, "alarms", alarms);
    return object;
}

/* Change l'état d'une alarme (1 ou 2) ; la diffusion est groupée. */
static void changer_alarme(int numero, EtatReveil etat)
{
    if (numero < 1 || numero > 2 || evenements.alarmes[numero - 1].etat == etat)
        return;

    evenements.alarmes[numero - 1].etat = etat;
    evenements.reveil_change = true;
}

/* Diffuse alarm_state si au moins une alarme a changé. */
static void diffuser_reveil(void)
{
    if (evenements.reveil_change) {
        evenements.reveil_change = false;
        diffuser("alarm_state", reveil_json());
    }
}

/* Relire l'état du réveil au prochain tour (après un arrêt, une reconnexion...). */
static void surveiller_alarme_bientot(void)
{
    evenements.surveiller = true;
    evenements.prochaine_lecture_ms = 0;
}

/* Alarm/Stop a réussi : c'est un arrêt, pas un snooze. */
static void noter_arret_demande(void)
{
    evenements.arret_demande = true;
}

/*
 * La sonnerie de l'alarme numero vient de cesser sans arrêt demandé :
 * snooze présumé, pour sa durée de snooze (relue) plus une marge.
 */
static void presumer_snooze(SweetDawn *app, int numero, int64_t now)
{
    ControlAlarm alarm;
    int minutes = 30;   /* le plus long, si la durée n'est pas lisible */

    if (control_alarm_query(app, numero, &alarm) == 0 &&
        alarm.snooze < sizeof(SNOOZE_MINUTES) / sizeof(SNOOZE_MINUTES[0]))
        minutes = SNOOZE_MINUTES[alarm.snooze];

    evenements.alarmes[numero - 1].fin_snooze_ms =
        now + minutes * 60000LL + SNOOZE_MARGIN_MS;
    changer_alarme(numero, REVEIL_SNOOZE);
}

/*
 * Tant qu'une alarme est en cours, relit l'état (code 25 : numéro de
 * l'alarme en lever de soleil, numéro de celle qui sonne) et en déduit
 * celui de chaque alarme.
 */
static void suivre_reveil(SweetDawn *app, int64_t now)
{
    uint8_t lever, sonne;
    bool en_cours = false;

    if (!evenements.surveiller || now < evenements.prochaine_lecture_ms)
        return;

    evenements.prochaine_lecture_ms = now + ALARM_WATCH_MS;

    if (control_alarm_status(app, &lever, &sonne) != 0)
        return;

    for (int n = 1; n <= 2; n++) {
        SuiviAlarme *a = &evenements.alarmes[n - 1];

        if (sonne == n) {
            a->a_sonne = true;
            a->fin_snooze_ms = 0;
            changer_alarme(n, REVEIL_SONNE);
        } else if (lever == n && !evenements.arret_demande) {
            changer_alarme(n, a->a_sonne ? REVEIL_SNOOZE : REVEIL_LEVER);
        } else if (a->etat == REVEIL_SONNE && !evenements.arret_demande) {
            presumer_snooze(app, n, now);
        } else if (a->etat == REVEIL_SNOOZE && !evenements.arret_demande &&
                   now < a->fin_snooze_ms) {
            /* Snooze présumé en cours : attendre qu'elle resonne ou non. */
        } else {
            a->a_sonne = false;
            a->fin_snooze_ms = 0;
            changer_alarme(n, REVEIL_REPOS);
        }

        if (a->etat != REVEIL_REPOS)
            en_cours = true;
    }

    /* Plus rien en cours : arrêter de relire. */
    if (!en_cours) {
        evenements.surveiller = false;
        evenements.arret_demande = false;
    }

    diffuser_reveil();
}

/* Relit Light et Mood, puis diffuse light_changed. */
static void relire_eclairage(SweetDawn *app)
{
    const CommandRegistry *registry = &app->commands.registry;
    ControlLight light, mood;

    if (control_light_query(app, CONTROL_LIGHT, &light) != 0 ||
        control_light_query(app, CONTROL_MOOD, &mood) != 0)
        return;

    struct json_object *data = json_object_new_object();

    json_object_object_add(data, "light",
                           lumiere_json(registry_find(registry, "Light"), &light));
    json_object_object_add(data, "mood",
                           lumiere_json(registry_find(registry, "Mood"), &mood));
    diffuser("light_changed", data);
}

/* Traite les messages notés depuis le tour précédent. */
static void traiter_evenements(SweetDawn *app, int64_t now)
{
    for (size_t i = 0; i < evenements.taille; i++) {
        uint8_t value = evenements.file[i].value;
        const char *timer = NULL;

        switch (evenements.file[i].code) {
        case AML_EVT_SUNRISE_START:
            if (value >= 1 && value <= 2) {
                evenements.alarmes[value - 1].a_sonne = false;
                changer_alarme(value, REVEIL_LEVER);
            }
            evenements.arret_demande = false;
            evenements.surveiller = true;
            evenements.prochaine_lecture_ms = now + ALARM_WATCH_MS;
            break;
        case AML_EVT_ALARM_RING:
            if (value >= 1 && value <= 2) {
                evenements.alarmes[value - 1].a_sonne = true;
                evenements.alarmes[value - 1].fin_snooze_ms = 0;
                changer_alarme(value, REVEIL_SONNE);
            }
            evenements.arret_demande = false;
            evenements.surveiller = true;
            evenements.prochaine_lecture_ms = now + ALARM_WATCH_MS;
            break;
        case AML_EVT_LIGHT_TIMER_END:
            timer = "light";
            evenements.eclairage_change = true;
            break;
        case AML_EVT_MOOD_TIMER_END:
            timer = "mood";
            evenements.eclairage_change = true;
            break;
        case AML_EVT_RADIO_TIMER_END:
            timer = "radio";
            break;
        case AML_EVT_MUSIC_TIMER_END:
            timer = "music";
            break;
        default:
            break;
        }

        if (timer != NULL) {
            struct json_object *data = json_object_new_object();

            json_object_object_add(data, "timer", json_object_new_string(timer));
            diffuser("timer_ended", data);
        }
    }

    /* Vider la file avant les relectures : elles peuvent en ajouter. */
    evenements.taille = 0;
    diffuser_reveil();

    if (evenements.eclairage_change) {
        evenements.eclairage_change = false;
        relire_eclairage(app);
    }
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

/*
 * Remise à l'heure automatique de l'AML, en mode auto uniquement :
 * à la première occasion après le démarrage, puis une fois par jour.
 * Garde l'écart choisi avec Settings/Set time (0 par défaut) et
 * rattrape la dérive de l'horloge de l'AML.
 */
static void entretenir_horloge(SweetDawn *app, int64_t now_ms)
{
    static int64_t next_attempt_ms = 0;

    if (!app->state.known || app->state.time != 0)
        return;

    if (now_ms < next_attempt_ms ||
        (app->clock_set && now_ms - app->clock_synced_ms < CLOCK_RESYNC_MS))
        return;

    time_t target = time(NULL) + (time_t)(app->clock_set ? app->clock_offset_s : 0);
    struct tm tm;

    localtime_r(&target, &tm);

    if (control_sync_time(app, &tm) == 0) {
        noter_heure(app, &tm);
        printf("SweetDawn : horloge de l'AML remise à l'heure.\n");
    } else {
        next_attempt_ms = now_ms + CLOCK_RETRY_MS;
    }
}

/*
 * Tâche périodique du service, après sweetdawn_tick() : événements de
 * l'AML, suivi du réveil, puis horloge.
 */
void handlers_tick(SweetDawn *app)
{
    if (app == NULL || !sweetdawn_is_connected(app))
        return;

    int64_t now = sweetdawn_now_ms(app);

    /* Nouvelle connexion : l'état du réveil a pu changer entre-temps. */
    if (app->connect_count != evenements.connexions) {
        evenements.connexions = app->connect_count;
        surveiller_alarme_bientot();
    }

    traiter_evenements(app, now);
    suivre_reveil(app, now);
    entretenir_horloge(app, now);
}

int handlers_attach(SweetDawn *app)
{
    CommandRegistry *registry;
    char path[64];

    if (app == NULL || !app->initialized)
        return -1;

    /* Repartir d'un état d'événements vierge (diffusion comprise). */
    memset(&evenements, 0, sizeof(evenements));

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

    /* Horloge de l'AML : connue seulement si SweetDawn l'a réglée. */
    struct json_object *clock = json_object_new_object();
    time_t aml_now;
    bool known = horloge_aml(app, &aml_now);

    json_object_object_add(clock, "known", json_object_new_boolean(known));

    if (known) {
        char text[40];

        format_iso(aml_now, text, sizeof(text));
        json_object_object_add(clock, "time", json_object_new_string(text));
        json_object_object_add(clock, "offset_s",
                               json_object_new_int64(app->clock_offset_s));
    }

    json_object_object_add(object, "clock", clock);

    /* État du réveil, mis à jour par les événements. */
    json_object_object_add(object, "alarm_state", reveil_json());

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
