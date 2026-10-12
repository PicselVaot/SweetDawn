#ifndef STATE_H
#define STATE_H

/*
 * core/state.h — État connu de l'AML005
 *
 * Ce module conserve en mémoire les dernières valeurs connues
 * des paramètres de l'AML005.
 *
 * Une valeur inconnue est distinguée d'une valeur égale à zéro :
 * on ne suppose pas qu'un paramètre vaut zéro simplement parce
 * qu'aucune réponse ne nous a encore renseignés à son sujet.
 */

#include <stdbool.h>
#include <stdint.h>

/*
 * État des paramètres transmis par l'AML005.
 *
 * known indique si les valeurs ont été reçues et interprétées.
 * Les champs restent des octets tant que leur signification
 * complète n'a pas été établie.
 */
typedef struct {
    bool known;

    /*
     * Octets bruts, dans l'ordre de la réponse 0xE2 (aml/protocol.h).
     * Correspondance probable avec le groupe « Settings » de
     * command/aml_commands.c, à confirmer sur le matériel :
     */
    uint8_t display;   /* Display brightness (H / A / L) ?  */
    uint8_t date;      /* Display time (24 h / 12 h) ?      */
    uint8_t time;      /* Time (auto / manual) ?            */
    uint8_t feedback;  /* Feedback (retour sonore) ?        */
    uint8_t fade;      /* Fade (fondu lumineux) ?           */
} AmlState;

/* Initialise l'état : aucune valeur n'est encore connue. */
void state_init(AmlState *state);

/* Enregistre les paramètres extraits d'une réponse AML005. */
void state_update_settings(
    AmlState *state,
    uint8_t display,
    uint8_t date,
    uint8_t time,
    uint8_t feedback,
    uint8_t fade
);

/* Affiche l'état connu pour faciliter le diagnostic. */
void state_print(const AmlState *state);

#endif /* STATE_H */