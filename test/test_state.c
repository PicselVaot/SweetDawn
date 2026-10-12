/*
 * test/test_state.c — Tests unitaires de l'état mémorisé (core/state)
 *
 * Ces tests vérifient l'initialisation et la mise à jour de l'état
 * mémorisé de l'AML005.
 *
 * Aucun appareil Bluetooth n'est nécessaire.
 */

#include <stdio.h>

#include "core/state.h"

/* Vérifie que l'état est inconnu après son initialisation. */
static int test_initialisation(void)
{
    AmlState state;

    state_init(&state);

    if (state.known) {
        fprintf(stderr, "ÉCHEC : l'état devrait être inconnu.\n");
        return 1;
    }

    printf("OK : l'état est inconnu après initialisation.\n");
    return 0;
}

/* Vérifie que les valeurs reçues sont correctement mémorisées. */
static int test_mise_a_jour(void)
{
    AmlState state;

    state_init(&state);

    state_update_settings(
        &state,
        0x02,
        0x00,
        0x01,
        0x00,
        0x00
    );

    if (!state.known) {
        fprintf(stderr, "ÉCHEC : l'état devrait être connu.\n");
        return 1;
    }

    if (state.display != 0x02 ||
        state.date != 0x00 ||
        state.time != 0x01 ||
        state.feedback != 0x00 ||
        state.fade != 0x00) {
        fprintf(stderr, "ÉCHEC : valeurs mémorisées incorrectes.\n");
        return 1;
    }

    printf("OK : les cinq paramètres sont correctement mémorisés.\n");
    return 0;
}

/* Vérifie qu'une mise à jour remplace les valeurs précédentes. */
static int test_remplacement(void)
{
    AmlState state;

    state_init(&state);

    state_update_settings(&state, 1, 2, 3, 4, 5);
    state_update_settings(&state, 6, 7, 8, 9, 10);

    if (state.display != 6 ||
        state.date != 7 ||
        state.time != 8 ||
        state.feedback != 9 ||
        state.fade != 10) {
        fprintf(stderr, "ÉCHEC : les nouvelles valeurs n'ont pas été appliquées.\n");
        return 1;
    }

    printf("OK : une mise à jour remplace les valeurs précédentes.\n");
    return 0;
}

int main(void)
{
    int failures = 0;

    printf("=== Tests du module d'état AML005 ===\n\n");

    failures += test_initialisation();
    failures += test_mise_a_jour();
    failures += test_remplacement();

    if (failures != 0) {
        fprintf(stderr, "\n%d test(s) en échec.\n", failures);
        return 1;
    }

    printf("\nTous les tests sont passés.\n");
    return 0;
}