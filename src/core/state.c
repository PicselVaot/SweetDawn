/*
 * core/state.c — Mémorisation de l'état de l'AML005
 */

#include "core/state.h"

#include <stdio.h>

/*
 * Initialise l'état.
 *
 * Les valeurs numériques sont mises à zéro pour éviter de conserver
 * des données indéterminées, mais elles ne doivent pas être considérées
 * comme l'état réel de l'AML005 tant que known vaut false.
 */
void state_init(AmlState *state)
{
    if (state == NULL)
        return;

    state->known = false;

    state->display = 0;
    state->date = 0;
    state->time = 0;
    state->feedback = 0;
    state->fade = 0;
}

/*
 * Met à jour l'état à partir des paramètres extraits d'une réponse.
 *
 * Cette fonction ne décode pas les significations des paramètres :
 * elle mémorise uniquement les valeurs brutes reçues.
 */
void state_update_settings(
    AmlState *state,
    uint8_t display,
    uint8_t date,
    uint8_t time,
    uint8_t feedback,
    uint8_t fade
)
{
    if (state == NULL)
        return;

    state->display = display;
    state->date = date;
    state->time = time;
    state->feedback = feedback;
    state->fade = fade;

    state->known = true;
}

/* Affiche les valeurs mémorisées sans leur attribuer de signification. */
void state_print(const AmlState *state)
{
    if (state == NULL) {
        printf("État AML005 indisponible : pointeur nul.\n");
        return;
    }

    if (!state->known) {
        printf("État AML005 inconnu : aucune réponse exploitable reçue.\n");
        return;
    }

    printf("État connu de l'AML005 :\n");
    printf("  display  = %u (0x%02X)\n",
           (unsigned int)state->display,
           (unsigned int)state->display);
    printf("  date     = %u (0x%02X)\n",
           (unsigned int)state->date,
           (unsigned int)state->date);
    printf("  time     = %u (0x%02X)\n",
           (unsigned int)state->time,
           (unsigned int)state->time);
    printf("  feedback = %u (0x%02X)\n",
           (unsigned int)state->feedback,
           (unsigned int)state->feedback);
    printf("  fade     = %u (0x%02X)\n",
           (unsigned int)state->fade,
           (unsigned int)state->fade);
}