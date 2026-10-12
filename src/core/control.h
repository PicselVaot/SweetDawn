#ifndef CONTROL_H
#define CONTROL_H

/*
 * core/control.h — Piloter l'AML005 : une fonction par opération
 *
 * Chaque fonction construit les données de la commande, l'envoie avec
 * sweetdawn_request(), puis décode la réponse s'il y en a une. Le format
 * exact de chaque commande est décrit dans docs/PROTOCOL.md.
 *
 * Toutes les fonctions retournent 0 en cas de succès, ou un code
 * AML_ERR_* négatif (voir aml/transport.h) :
 *   AML_ERR_DISCONNECTED : pas de liaison avec l'AML en ce moment ;
 *   AML_ERR_TIMEOUT      : l'AML n'a pas répondu ;
 *   AML_ERR_PROTOCOL     : réponse inattendue ou trop courte ;
 *   AML_ERR_GENERIC      : argument invalide (pointeur nul, valeur
 *                          hors limites...).
 *
 * Les valeurs ne sont vérifiées ici que pour ne pas envoyer d'octet
 * absurde : la validation fine (plages, choix possibles) est faite en
 * amont par l'arbre des commandes (command/aml_commands.c).
 *
 * Commandes « sans réponse » : l'AML n'accuse pas réception. Un succès
 * signifie seulement que la trame est partie ; pour vérifier l'effet,
 * relire l'état avec la fonction *_query correspondante.
 */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "core/sweetdawn.h"

/* ------------------------------------------------------------------------- */
/* Réglages généraux                                                         */
/* ------------------------------------------------------------------------- */

/*
 * Écrit les cinq réglages d'un coup (l'AML n'accepte pas de les modifier
 * un par un) puis met à jour l'état mémorisé de app.
 *
 * settings contient les octets bruts, tels que lus par sweetdawn_refresh :
 * pour modifier un seul réglage, copier app->state, changer le champ
 * voulu, puis appeler cette fonction.
 */
int control_settings_write(SweetDawn *app, const AmlState *settings);

/* Met l'horloge de l'AML à l'heure donnée (heure locale). */
int control_sync_time(SweetDawn *app, const struct tm *now);

/* ------------------------------------------------------------------------- */
/* Éclairage                                                                 */
/* ------------------------------------------------------------------------- */

/* Les deux sources lumineuses : la lampe principale et l'ambiance. */
typedef enum {
    CONTROL_LIGHT = 1,   /* AML_LIGHT_TARGET_LIGHT */
    CONTROL_MOOD  = 2    /* AML_LIGHT_TARGET_MOOD  */
} ControlLightTarget;

/*
 * État d'une source lumineuse.
 *
 * timer : pour Light, c'est le « coucher de soleil » (extinction
 *         progressive) ; pour Mood, un simple minuteur d'extinction.
 * red, green, blue, scene : utiles pour Mood uniquement.
 */
typedef struct {
    bool enable;
    uint8_t brightness;      /* 0 à 100 */
    bool timer_enable;
    uint8_t timer_minutes;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t scene;           /* index dans la liste des scènes, 0 = Off */
} ControlLight;

int control_light_query(SweetDawn *app, ControlLightTarget target,
                        ControlLight *light);

/* Commandes sans réponse. */
int control_light_enable(SweetDawn *app, ControlLightTarget target,
                         bool enable);
int control_light_brightness(SweetDawn *app, ControlLightTarget target,
                             uint8_t brightness);
int control_light_timer_enable(SweetDawn *app, ControlLightTarget target,
                               bool enable);
int control_light_timer(SweetDawn *app, ControlLightTarget target,
                        uint8_t minutes);
int control_mood_color(SweetDawn *app,
                       uint8_t red, uint8_t green, uint8_t blue);
int control_mood_scene(SweetDawn *app, uint8_t scene);

/* ------------------------------------------------------------------------- */
/* Alarmes                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Réglages d'une alarme.
 *
 * days   : un bit par jour, bit 0 = dimanche, bit 1 = lundi ...
 *          bit 6 = samedi (0x3E = du lundi au vendredi).
 * snooze : index dans { 1, 2, 5, 10, 20, 30 } minutes.
 * tone   : index de la sonnerie (voir TONE_VALUES dans aml_commands.c).
 */
typedef struct {
    bool enable;
    uint8_t hour;
    uint8_t minute;
    uint8_t days;
    uint8_t tone;
    uint8_t volume;
    uint8_t snooze;
    bool sunrise_enable;
    uint8_t sunrise_minutes;
    uint8_t sunrise_brightness;
} ControlAlarm;

/* number : 1 ou 2. */
int control_alarm_query(SweetDawn *app, int number, ControlAlarm *alarm);

/*
 * Écrit une alarme complète. Pour ne changer qu'un réglage : lire
 * l'alarme avec control_alarm_query, modifier le champ, puis écrire.
 */
int control_alarm_write(SweetDawn *app, int number,
                        const ControlAlarm *alarm);

/*
 * Ce qui se passe en ce moment : numéro de l'alarme dont le lever de
 * soleil est en cours, et de celle qui sonne (0 = aucune).
 * Pendant un snooze : lever de soleil en cours, rien ne sonne.
 */
int control_alarm_status(SweetDawn *app, uint8_t *sunrise, uint8_t *ringing);

/* Agissent sur l'alarme qui sonne en ce moment. */
int control_alarm_stop(SweetDawn *app);
int control_alarm_snooze(SweetDawn *app);

/* ------------------------------------------------------------------------- */
/* Radio                                                                     */
/* ------------------------------------------------------------------------- */

/* frequency : en dixièmes de MHz (1015 = 101,5 MHz). */
typedef struct {
    bool enable;
    uint8_t channel;         /* présélection courante, 1 à 10 */
    uint16_t frequency;
    uint8_t volume;          /* 1 à 10 */
    bool timer_enable;
    uint8_t timer_minutes;
} ControlRadio;

/*
 * TUNE : pas de 0,1 MHz.
 * SEEK : recherche de la station suivante ou précédente.
 */
typedef enum {
    CONTROL_TUNE_UP,
    CONTROL_TUNE_DOWN,
    CONTROL_SEEK_UP,
    CONTROL_SEEK_DOWN
} ControlTune;

int control_radio_query(SweetDawn *app, ControlRadio *radio);
int control_radio_enable(SweetDawn *app, bool enable);

/* frequency (peut être NULL) reçoit la nouvelle fréquence. */
int control_radio_tune(SweetDawn *app, ControlTune direction,
                       uint16_t *frequency);

/* channel : 1 à 10. */
int control_radio_preset(SweetDawn *app, uint8_t channel);

/* Commandes sans réponse. */
int control_radio_volume(SweetDawn *app, uint8_t volume);
int control_radio_timer_enable(SweetDawn *app, bool enable);
int control_radio_timer(SweetDawn *app, uint8_t minutes);

/* ------------------------------------------------------------------------- */
/* Haut-parleur Bluetooth                                                    */
/* ------------------------------------------------------------------------- */

/* Active ou désactive le mode haut-parleur Bluetooth (Media Player). */
int control_speaker_enable(SweetDawn *app, bool enable);

#endif /* CONTROL_H */
