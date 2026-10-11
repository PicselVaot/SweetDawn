/*
 * test_config.c — Tests du module de configuration SweetDawn
 *
 * Ces tests vérifient le chargement du JSON et la sélection
 * d'un appareil AML005.
 */

#include <stdio.h>
#include <string.h>

#include "config.h"

static int test_chargement(void)
{
    AppConfig config;
    char error[256];

    if (config_load("config/aml.json", &config,
                    error, sizeof(error)) < 0) {
        fprintf(stderr, "ÉCHEC : chargement : %s\n", error);
        return 1;
    }

    if (config.count == 0) {
        fprintf(stderr, "ÉCHEC : aucun appareil chargé.\n");
        return 1;
    }

    printf("OK : configuration chargée (%zu appareil(s)).\n",
           config.count);

    return 0;
}

static int test_valeurs(void)
{
    AppConfig config;
    char error[256];

    if (config_load("config/aml.json", &config,
                    error, sizeof(error)) < 0) {
        fprintf(stderr, "ÉCHEC : chargement : %s\n", error);
        return 1;
    }

    const AmlConfig *device = config_select(&config, NULL);

    if (device == NULL) {
        fprintf(stderr, "ÉCHEC : aucun appareil par défaut.\n");
        return 1;
    }

    if (strcmp(device->name, "Alias for device") != 0 ||
        strcmp(device->aml_mac, "AA:BB:CC:DD:EE:FF") != 0 ||
        device->aml_chn != 1 ||
        strcmp(device->blt_mac, "FF:EE:DD:CC:BB:AA") != 0) {
        fprintf(stderr, "ÉCHEC : valeurs de configuration incorrectes.\n");
        return 1;
    }

    printf("OK : les paramètres de l'appareil sont corrects.\n");
    return 0;
}

static int test_selection_par_nom(void)
{
    AppConfig config;
    char error[256];

    if (config_load("config/aml.json", &config,
                    error, sizeof(error)) < 0) {
        fprintf(stderr, "ÉCHEC : chargement : %s\n", error);
        return 1;
    }

    const AmlConfig *device = config_select(&config, "Alias for device");

    if (device == NULL || strcmp(device->name, "Alias for device") != 0) {
        fprintf(stderr, "ÉCHEC : sélection par nom.\n");
        return 1;
    }

    printf("OK : sélection par nom.\n");
    return 0;
}

static int test_nom_inconnu(void)
{
    AppConfig config;
    char error[256];

    if (config_load("config/aml.json", &config,
                    error, sizeof(error)) < 0) {
        fprintf(stderr, "ÉCHEC : chargement : %s\n", error);
        return 1;
    }

    if (config_select(&config, "appareil-inexistant") != NULL) {
        fprintf(stderr, "ÉCHEC : un nom inconnu a été accepté.\n");
        return 1;
    }

    printf("OK : un nom inconnu est rejeté.\n");
    return 0;
}

static int test_fichier_inexistant(void)
{
    AppConfig config;
    char error[256];

    if (config_load("config/fichier-inexistant.json",
                    &config, error, sizeof(error)) == 0) {
        fprintf(stderr, "ÉCHEC : un fichier absent a été accepté.\n");
        return 1;
    }

    if (error[0] == '\0') {
        fprintf(stderr, "ÉCHEC : aucun message d'erreur fourni.\n");
        return 1;
    }

    printf("OK : un fichier absent produit une erreur explicite.\n");
    return 0;
}

int main(void)
{
    int failures = 0;

    printf("=== Tests de configuration SweetDawn ===\n\n");

    failures += test_chargement();
    failures += test_valeurs();
    failures += test_selection_par_nom();
    failures += test_nom_inconnu();
    failures += test_fichier_inexistant();

    if (failures != 0) {
        fprintf(stderr, "\n%d test(s) en échec.\n", failures);
        return 1;
    }

    printf("\nTous les tests sont passés.\n");
    return 0;
}