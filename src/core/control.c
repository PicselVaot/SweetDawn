/*
 * core/control.c — Piloter l'AML005 : une fonction par opération
 *
 * Chaque fonction se contente de ranger les données dans l'ordre attendu
 * par l'AML : l'enveloppe de la trame (en-tête, longueur, checksum) est
 * ajoutée par aml/protocol.c, l'envoi et l'attente de la réponse sont
 * faits par sweetdawn_request().
 *
 * L'ordre des octets de chaque commande vient de l'application Android
 * d'origine ; il est résumé dans docs/PROTOCOL.md.
 */

#include "core/control.h"

#include <stddef.h>

#include "aml/protocol.h"

/*
 * Taille de l'enveloppe autour des données d'une réponse :
 * 8 octets avant (FE EF 0C LL AB BB IL CC), 4 après (XX 55 0D 0A).
 */
#define FRAME_OVERHEAD 12

/* Une recherche de station peut parcourir toute la bande FM. */
#define RADIO_SEEK_TIMEOUT_MS 15000

/* Index des alarmes dans le protocole ; bit 7 = lecture. */
#define ALARM_INDEX_1    0x01
#define ALARM_INDEX_2    0x07
#define ALARM_READ_FLAG  0x80

/* Une réponse reçue, et un accès direct à ses données. */
typedef struct {
    uint8_t frame[AML_PROTOCOL_MAX_FRAME_SIZE];
    size_t length;
    const uint8_t *data;
    size_t data_length;
} Reponse;

/* ------------------------------------------------------------------------- */
/* Outils                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Envoie une commande et attend sa réponse, qui doit contenir au moins
 * min_data octets de données.
 */
static int demander(
    SweetDawn *app,
    uint8_t request_code,
    const uint8_t *arguments,
    size_t arguments_length,
    uint8_t response_code,
    int timeout_ms,
    size_t min_data,
    Reponse *reponse
)
{
    int code = sweetdawn_request(
        app,
        request_code,
        arguments,
        arguments_length,
        response_code,
        timeout_ms,
        reponse->frame,
        sizeof(reponse->frame),
        &reponse->length
    );

    if (code != 0)
        return code;

    if (reponse->length < FRAME_OVERHEAD + min_data)
        return AML_ERR_PROTOCOL;

    reponse->data = &reponse->frame[AML_FRAME_DATA_OFFSET];
    reponse->data_length = reponse->length - FRAME_OVERHEAD;

    return 0;
}

/* Envoie une commande à laquelle l'AML ne répond pas. */
static int envoyer(
    SweetDawn *app,
    uint8_t request_code,
    const uint8_t *arguments,
    size_t arguments_length
)
{
    return sweetdawn_request(
        app,
        request_code,
        arguments,
        arguments_length,
        SWEETDAWN_NO_RESPONSE,
        0,
        NULL,
        0,
        NULL
    );
}

static int cible_valide(ControlLightTarget target)
{
    return target == CONTROL_LIGHT || target == CONTROL_MOOD;
}

/* ------------------------------------------------------------------------- */
/* Réglages généraux                                                         */
/* ------------------------------------------------------------------------- */

int control_settings_write(SweetDawn *app, const AmlState *settings)
{
    Reponse reponse;

    if (app == NULL || settings == NULL)
        return AML_ERR_GENERIC;

    /* Même ordre que la réponse de lecture des réglages (0xE2). */
    const uint8_t args[] = {
        settings->display,
        settings->date,
        settings->time,
        settings->feedback,
        settings->fade
    };

    int code = demander(app, AML_REQ_WRITE_SETTINGS, args, sizeof(args),
                        AML_RSP_WRITE_SETTINGS, 0, 0, &reponse);

    if (code != 0)
        return code;

    state_update_settings(
        &app->state,
        settings->display,
        settings->date,
        settings->time,
        settings->feedback,
        settings->fade
    );

    return 0;
}

int control_sync_time(SweetDawn *app, const struct tm *now)
{
    Reponse reponse;

    if (app == NULL || now == NULL || now->tm_year < 100)
        return AML_ERR_GENERIC;

    /*
     * Du plus petit au plus grand ; jour de la semaine 0 = dimanche,
     * année sur deux chiffres. Le dernier octet (2) est une constante
     * de l'application d'origine, de sens inconnu.
     */
    const uint8_t args[] = {
        (uint8_t)now->tm_sec,
        (uint8_t)now->tm_min,
        (uint8_t)now->tm_hour,
        (uint8_t)now->tm_wday,
        (uint8_t)now->tm_mday,
        (uint8_t)(now->tm_mon + 1),
        (uint8_t)(now->tm_year - 100),
        0x02
    };

    return demander(app, AML_REQ_SYNC_TIME, args, sizeof(args),
                    AML_RSP_SYNC_TIME, 0, 0, &reponse);
}

/* ------------------------------------------------------------------------- */
/* Éclairage                                                                 */
/* ------------------------------------------------------------------------- */

int control_light_query(SweetDawn *app, ControlLightTarget target,
                        ControlLight *light)
{
    Reponse reponse;

    if (app == NULL || light == NULL || !cible_valide(target))
        return AML_ERR_GENERIC;

    const uint8_t args[] = { (uint8_t)target };

    int code = demander(app, AML_REQ_LIGHT_QUERY, args, sizeof(args),
                        AML_RSP_LIGHT_QUERY, 0, 5, &reponse);

    if (code != 0)
        return code;

    const uint8_t *d = reponse.data;

    /* d[0] rappelle la cible interrogée. */
    if (d[0] != (uint8_t)target)
        return AML_ERR_PROTOCOL;

    light->enable = d[1] == 1;
    light->brightness = d[2];
    light->timer_enable = d[3] == 1;
    light->timer_minutes = d[4];

    /*
     * Couleur et scène : seulement dans la réponse de Mood (9 octets).
     * Celle de Light s'arrête après la durée (5 octets), constaté sur
     * le matériel.
     */
    light->red = light->green = light->blue = light->scene = 0;

    if (reponse.data_length >= 9) {
        light->red = d[5];
        light->green = d[6];
        light->blue = d[7];
        light->scene = d[8];
    }

    return 0;
}

int control_light_enable(SweetDawn *app, ControlLightTarget target,
                         bool enable)
{
    if (!cible_valide(target))
        return AML_ERR_GENERIC;

    const uint8_t args[] = { (uint8_t)target };

    return envoyer(app, enable ? AML_REQ_LIGHT_ON : AML_REQ_LIGHT_OFF,
                   args, sizeof(args));
}

int control_light_brightness(SweetDawn *app, ControlLightTarget target,
                             uint8_t brightness)
{
    if (!cible_valide(target) || brightness > 100)
        return AML_ERR_GENERIC;

    const uint8_t args[] = { (uint8_t)target, brightness };

    return envoyer(app, AML_REQ_LIGHT_BRIGHTNESS, args, sizeof(args));
}

int control_light_timer_enable(SweetDawn *app, ControlLightTarget target,
                               bool enable)
{
    if (!cible_valide(target))
        return AML_ERR_GENERIC;

    const uint8_t args[] = { (uint8_t)target };

    return envoyer(app,
                   enable ? AML_REQ_LIGHT_TIMER_ON : AML_REQ_LIGHT_TIMER_OFF,
                   args, sizeof(args));
}

int control_light_timer(SweetDawn *app, ControlLightTarget target,
                        uint8_t minutes)
{
    if (!cible_valide(target))
        return AML_ERR_GENERIC;

    const uint8_t args[] = { (uint8_t)target, minutes };

    return envoyer(app, AML_REQ_LIGHT_TIMER, args, sizeof(args));
}

int control_mood_color(SweetDawn *app,
                       uint8_t red, uint8_t green, uint8_t blue)
{
    const uint8_t args[] = { red, green, blue };

    return envoyer(app, AML_REQ_MOOD_COLOR, args, sizeof(args));
}

int control_mood_scene(SweetDawn *app, uint8_t scene)
{
    const uint8_t args[] = { scene };

    return envoyer(app, AML_REQ_MOOD_SCENE, args, sizeof(args));
}

/* ------------------------------------------------------------------------- */
/* Alarmes                                                                   */
/* ------------------------------------------------------------------------- */

/* Numéro d'alarme (1 ou 2) vers son index dans le protocole, 0 si invalide. */
static uint8_t index_alarme(int number)
{
    switch (number) {
    case 1:  return ALARM_INDEX_1;
    case 2:  return ALARM_INDEX_2;
    default: return 0;
    }
}

int control_alarm_query(SweetDawn *app, int number, ControlAlarm *alarm)
{
    Reponse reponse;
    uint8_t index = index_alarme(number);

    if (app == NULL || alarm == NULL || index == 0)
        return AML_ERR_GENERIC;

    /* Le second octet (2) est une constante de l'application d'origine. */
    const uint8_t args[] = { (uint8_t)(index | ALARM_READ_FLAG), 0x02 };

    int code = demander(app, AML_REQ_ALARM, args, sizeof(args),
                        AML_RSP_ALARM, 0, 11, &reponse);

    if (code != 0)
        return code;

    const uint8_t *d = reponse.data;

    if ((d[0] & 0x7F) != index)
        return AML_ERR_PROTOCOL;

    /* Attention : la minute vient avant l'heure. */
    alarm->enable = d[1] == 1;
    alarm->minute = d[2];
    alarm->hour = d[3];
    alarm->days = d[4] & 0x7F;
    alarm->tone = d[5];
    alarm->volume = d[6];
    alarm->snooze = d[7];
    alarm->sunrise_enable = d[8] == 1;
    alarm->sunrise_minutes = d[9];
    alarm->sunrise_brightness = d[10];

    return 0;
}

int control_alarm_write(SweetDawn *app, int number,
                        const ControlAlarm *alarm)
{
    Reponse reponse;
    uint8_t index = index_alarme(number);

    if (app == NULL || alarm == NULL || index == 0 ||
        alarm->hour > 23 || alarm->minute > 59)
        return AML_ERR_GENERIC;

    /* Même ordre que la réponse de lecture, sans le bit de lecture. */
    const uint8_t args[] = {
        index,
        alarm->enable ? 1 : 0,
        alarm->minute,
        alarm->hour,
        (uint8_t)(alarm->days & 0x7F),
        alarm->tone,
        alarm->volume,
        alarm->snooze,
        alarm->sunrise_enable ? 1 : 0,
        alarm->sunrise_minutes,
        alarm->sunrise_brightness
    };

    return demander(app, AML_REQ_ALARM, args, sizeof(args),
                    AML_RSP_ALARM, 0, 0, &reponse);
}

int control_alarm_status(SweetDawn *app, uint8_t *sunrise, uint8_t *ringing)
{
    Reponse reponse;

    if (app == NULL || sunrise == NULL || ringing == NULL)
        return AML_ERR_GENERIC;

    int code = demander(app, AML_REQ_ALARM_STATUS, NULL, 0,
                        AML_RSP_ALARM_STATUS, 0, 2, &reponse);

    if (code != 0)
        return code;

    *sunrise = reponse.data[0];
    *ringing = reponse.data[1];

    return 0;
}

int control_alarm_stop(SweetDawn *app)
{
    Reponse reponse;

    return demander(app, AML_REQ_ALARM_STOP, NULL, 0,
                    AML_RSP_ALARM_STOP, 0, 0, &reponse);
}

int control_alarm_snooze(SweetDawn *app)
{
    Reponse reponse;

    return demander(app, AML_REQ_ALARM_SNOOZE, NULL, 0,
                    AML_RSP_ALARM_SNOOZE, 0, 0, &reponse);
}

/* ------------------------------------------------------------------------- */
/* Radio                                                                     */
/* ------------------------------------------------------------------------- */

int control_radio_query(SweetDawn *app, ControlRadio *radio)
{
    Reponse reponse;

    if (app == NULL || radio == NULL)
        return AML_ERR_GENERIC;

    /* Argument constant (2) repris de l'application d'origine. */
    const uint8_t args[] = { 0x02 };

    int code = demander(app, AML_REQ_RADIO_QUERY, args, sizeof(args),
                        AML_RSP_RADIO_QUERY, 0, 7, &reponse);

    if (code != 0)
        return code;

    const uint8_t *d = reponse.data;

    radio->enable = d[0] == 1;
    radio->channel = d[1];
    radio->frequency = (uint16_t)((d[2] << 8) | d[3]);
    radio->volume = d[4];
    radio->timer_enable = d[5] == 1;
    radio->timer_minutes = d[6];

    return 0;
}

int control_radio_enable(SweetDawn *app, bool enable)
{
    Reponse reponse;
    const uint8_t args[] = { enable ? 1 : 0 };

    return demander(app, AML_REQ_RADIO_ENABLE, args, sizeof(args),
                    AML_RSP_RADIO_ENABLE, 0, 0, &reponse);
}

int control_radio_tune(SweetDawn *app, ControlTune direction,
                       uint16_t *frequency)
{
    Reponse reponse;
    bool seek = direction == CONTROL_SEEK_UP || direction == CONTROL_SEEK_DOWN;
    bool down = direction == CONTROL_TUNE_DOWN || direction == CONTROL_SEEK_DOWN;

    /* Premier octet : 0 = pas fin, 1 = recherche ; second : 0 = monter. */
    const uint8_t args[] = { seek ? 1 : 0, down ? 1 : 0 };

    int code = demander(app, AML_REQ_RADIO_TUNE, args, sizeof(args),
                        AML_RSP_RADIO_TUNE,
                        seek ? RADIO_SEEK_TIMEOUT_MS : 0,
                        4, &reponse);

    if (code != 0)
        return code;

    /* Réponse : rappel des deux octets, puis la nouvelle fréquence. */
    if (frequency != NULL)
        *frequency = (uint16_t)((reponse.data[2] << 8) | reponse.data[3]);

    return 0;
}

int control_radio_preset(SweetDawn *app, uint8_t channel)
{
    Reponse reponse;

    if (channel < 1 || channel > 10)
        return AML_ERR_GENERIC;

    const uint8_t args[] = { channel };

    return demander(app, AML_REQ_RADIO_PRESET, args, sizeof(args),
                    AML_RSP_RADIO_PRESET, 0, 0, &reponse);
}

int control_radio_volume(SweetDawn *app, uint8_t volume)
{
    const uint8_t args[] = { volume };

    return envoyer(app, AML_REQ_RADIO_VOLUME, args, sizeof(args));
}

int control_radio_timer_enable(SweetDawn *app, bool enable)
{
    const uint8_t args[] = { enable ? 1 : 0 };

    return envoyer(app, AML_REQ_RADIO_TIMER_ENABLE, args, sizeof(args));
}

int control_radio_timer(SweetDawn *app, uint8_t minutes)
{
    const uint8_t args[] = { minutes };

    return envoyer(app, AML_REQ_RADIO_TIMER, args, sizeof(args));
}

/* ------------------------------------------------------------------------- */
/* Haut-parleur Bluetooth                                                    */
/* ------------------------------------------------------------------------- */

int control_speaker_enable(SweetDawn *app, bool enable)
{
    Reponse reponse;

    /* Activer reçoit une réponse, désactiver non. */
    if (!enable)
        return envoyer(app, AML_REQ_SPEAKER_OFF, NULL, 0);

    return demander(app, AML_REQ_SPEAKER_ON, NULL, 0,
                    AML_RSP_SPEAKER_ON, 0, 0, &reponse);
}
