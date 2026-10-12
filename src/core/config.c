/*
 * core/config.c — Chargement et validation de la configuration AML005
 *
 * La bibliothèque json-c est utilisée pour analyser le fichier JSON.
 * Les données sont ensuite copiées dans nos propres structures C.
 */

#include "core/config.h"

#include <json-c/json.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Prépare un message d'erreur si un tampon est disponible. */
static void set_error(
    char *buffer,
    size_t capacity,
    const char *format,
    ...
)
{
    if (buffer == NULL || capacity == 0)
        return;

    va_list args;
    va_start(args, format);
    vsnprintf(buffer, capacity, format, args);
    va_end(args);
}

/*
 * Vérifie le format XX:XX:XX:XX:XX:XX d'une adresse MAC.
 *
 * Chaque paire doit contenir deux caractères hexadécimaux.
 */
static int valid_mac(const char *mac)
{
    if (mac == NULL || strlen(mac) != 17)
        return 0;

    for (size_t i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (mac[i] != ':')
                return 0;
        } else {
            char c = mac[i];

            if (!((c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) {
                return 0;
            }
        }
    }

    return 1;
}

/* Lit un champ JSON obligatoire de type chaîne. */
static int read_string(
    struct json_object *object,
    const char *key,
    char *destination,
    size_t capacity,
    char *error_message,
    size_t error_capacity
)
{
    struct json_object *value = NULL;

    if (!json_object_object_get_ex(object, key, &value) ||
        value == NULL ||
        !json_object_is_type(value, json_type_string)) {
        set_error(
            error_message,
            error_capacity,
            "Champ '%s' absent ou non textuel.",
            key
        );
        return -1;
    }

    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);

    if (text == NULL || length == 0 || length >= capacity) {
        set_error(
            error_message,
            error_capacity,
            "Champ '%s' vide ou trop long.",
            key
        );
        return -1;
    }

    /*
     * Refuser les caractères NUL intégrés à la chaîne JSON :
     * ils rendraient les validations C ambiguës.
     */
    if (strlen(text) != length) {
        set_error(
            error_message,
            error_capacity,
            "Champ '%s' contient un caractère invalide.",
            key
        );
        return -1;
    }

    memcpy(destination, text, length + 1);
    return 0;
}

/* Lit le canal RFCOMM, qui doit être un entier compris entre 1 et 30. */
static int read_channel(
    struct json_object *object,
    int *channel,
    char *error_message,
    size_t error_capacity
)
{
    struct json_object *value = NULL;

    if (!json_object_object_get_ex(object, "AML_CHN", &value) ||
        value == NULL ||
        !json_object_is_type(value, json_type_int)) {
        set_error(
            error_message,
            error_capacity,
            "Champ 'AML_CHN' absent ou non entier."
        );
        return -1;
    }

    int number = json_object_get_int(value);

    if (number < 1 || number > 30) {
        set_error(
            error_message,
            error_capacity,
            "Canal RFCOMM invalide : %d (valeurs admises : 1 à 30).",
            number
        );
        return -1;
    }

    *channel = number;
    return 0;
}

/* Valide et copie la configuration d'un appareil. */
static int parse_device(
    struct json_object *object,
    AmlConfig *device,
    char *error_message,
    size_t error_capacity
)
{
    if (object == NULL ||
        !json_object_is_type(object, json_type_object)) {
        set_error(
            error_message,
            error_capacity,
            "Chaque entrée de la configuration doit être un objet JSON."
        );
        return -1;
    }

    if (read_string(
            object, "name",
            device->name, sizeof(device->name),
            error_message, error_capacity) < 0) {
        return -1;
    }

    if (read_string(
            object, "AML_MAC",
            device->aml_mac, sizeof(device->aml_mac),
            error_message, error_capacity) < 0) {
        return -1;
    }

    if (read_channel(
            object,
            &device->aml_chn,
            error_message,
            error_capacity) < 0) {
        return -1;
    }

    if (read_string(
            object, "BLT_MAC",
            device->blt_mac, sizeof(device->blt_mac),
            error_message, error_capacity) < 0) {
        return -1;
    }

    if (!valid_mac(device->aml_mac)) {
        set_error(
            error_message,
            error_capacity,
            "Adresse AML_MAC invalide pour '%s'.",
            device->name
        );
        return -1;
    }

    if (!valid_mac(device->blt_mac)) {
        set_error(
            error_message,
            error_capacity,
            "Adresse BLT_MAC invalide pour '%s'.",
            device->name
        );
        return -1;
    }

    return 0;
}

/* Charge le fichier JSON et valide l'ensemble des appareils. */
int config_load(
    const char *path,
    AppConfig *config,
    char *error_message,
    size_t error_capacity
)
{
    if (error_message != NULL && error_capacity > 0)
        error_message[0] = '\0';

    if (path == NULL || config == NULL) {
        set_error(
            error_message,
            error_capacity,
            "Chemin de fichier ou configuration invalide."
        );
        return -1;
    }

    config->count = 0;

    struct json_object *root = json_object_from_file(path);

    if (root == NULL) {
        set_error(
            error_message,
            error_capacity,
            "Impossible de lire ou d'analyser le JSON '%s'.",
            path
        );
        return -1;
    }

    if (!json_object_is_type(root, json_type_array)) {
        set_error(
            error_message,
            error_capacity,
            "La racine de '%s' doit être un tableau JSON.",
            path
        );
        json_object_put(root);
        return -1;
    }

    size_t count = json_object_array_length(root);

    if (count == 0) {
        set_error(
            error_message,
            error_capacity,
            "La configuration ne contient aucun appareil."
        );
        json_object_put(root);
        return -1;
    }

    if (count > CONFIG_MAX_AMLS) {
        set_error(
            error_message,
            error_capacity,
            "Trop d'appareils : %zu (maximum : %d).",
            count,
            CONFIG_MAX_AMLS
        );
        json_object_put(root);
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        struct json_object *entry = json_object_array_get_idx(root, i);

        if (parse_device(
                entry,
                &config->devices[i],
                error_message,
                error_capacity) < 0) {
            config->count = 0;
            json_object_put(root);
            return -1;
        }

        /* Les noms logiques doivent être uniques. */
        for (size_t j = 0; j < i; j++) {
            if (strcmp(
                    config->devices[j].name,
                    config->devices[i].name) == 0) {
                set_error(
                    error_message,
                    error_capacity,
                    "Nom d'appareil dupliqué : '%s'.",
                    config->devices[i].name
                );
                config->count = 0;
                json_object_put(root);
                return -1;
            }
        }
    }

    config->count = count;
    json_object_put(root);

    return 0;
}

/*
 * Renvoie le premier appareil si aucun nom n'est précisé.
 * Sinon, recherche une correspondance exacte.
 */
const AmlConfig *config_select(
    const AppConfig *config,
    const char *name
)
{
    if (config == NULL || config->count == 0)
        return NULL;

    if (name == NULL || name[0] == '\0')
        return &config->devices[0];

    for (size_t i = 0; i < config->count; i++) {
        if (strcmp(config->devices[i].name, name) == 0)
            return &config->devices[i];
    }

    return NULL;
}