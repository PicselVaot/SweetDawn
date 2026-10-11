#ifndef CONFIG_H
#define CONFIG_H

/*
 * config.h — Gestion de la configuration des appareils AML005
 *
 * Ce module charge les appareils depuis un fichier JSON.
 * Il ne gère ni les connexions Bluetooth ni le protocole AML005.
 */

#include <stddef.h>

#define CONFIG_MAX_AMLS 32
#define CONFIG_NAME_MAX 64
#define CONFIG_MAC_LENGTH 18

/*
 * Configuration d'un appareil AML005.
 *
 * name    : nom logique utilisé pour sélectionner l'appareil.
 * aml_mac : adresse Bluetooth de l'AML005 distant.
 * aml_chn : canal RFCOMM distant.
 * blt_mac : adresse de l'adaptateur Bluetooth local.
 */
typedef struct {
    char name[CONFIG_NAME_MAX];
    char aml_mac[CONFIG_MAC_LENGTH];
    int aml_chn;
    char blt_mac[CONFIG_MAC_LENGTH];
} AmlConfig;

/* Liste des appareils chargés depuis le fichier JSON. */
typedef struct {
    AmlConfig devices[CONFIG_MAX_AMLS];
    size_t count;
} AppConfig;

/*
 * Charge et valide la configuration JSON.
 *
 * Retour :
 *   0  : chargement réussi.
 *  -1  : erreur de lecture ou de validation.
 *
 * En cas d'erreur, error_message reçoit une explication
 * si son tampon est fourni.
 */
int config_load(
    const char *path,
    AppConfig *config,
    char *error_message,
    size_t error_capacity
);

/*
 * Sélectionne un appareil par son nom.
 *
 * Si name vaut NULL ou est vide, le premier appareil est renvoyé.
 *
 * Si un nom explicite est fourni mais introuvable, la fonction
 * renvoie NULL : elle ne sélectionne pas un autre appareil.
 *
 * Le pointeur renvoyé désigne un élément de config.
 * Il reste valide tant que config existe et n'est pas modifié.
 */
const AmlConfig *config_select(
    const AppConfig *config,
    const char *name
);

#endif /* CONFIG_H */