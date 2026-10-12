/*
 * ipc/ipc_protocol.c — Lecture et construction des messages JSON IPC
 */

#include "ipc/ipc_protocol.h"

#include <string.h>

int ipc_protocol_parse(
    const char *message,
    size_t message_length,
    struct json_object **result
)
{
    struct json_tokener *tokener;
    struct json_object *object;
    enum json_tokener_error error;
    size_t parsed_length;

    if (message == NULL || result == NULL || message_length == 0) {
        return -1;
    }

    *result = NULL;

    tokener = json_tokener_new();
    if (tokener == NULL) {
        return -1;
    }

    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);

    object = json_tokener_parse_ex(
        tokener,
        message,
        (int)message_length
    );

    error = json_tokener_get_error(tokener);
    parsed_length = json_tokener_get_parse_end(tokener);

    /*
     * Rejeter : JSON invalide ou incomplet, texte en trop après l'objet
     * ({"a":1}xyz), ou racine qui n'est pas un objet ([1,2], 42...).
     */
    if (error != json_tokener_success ||
        object == NULL ||
        parsed_length != message_length ||
        !json_object_is_type(object, json_type_object)) {
        if (object != NULL) {
            json_object_put(object);
        }

        json_tokener_free(tokener);
        return -1;
    }

    json_tokener_free(tokener);
    *result = object;

    return 0;
}

struct json_object *ipc_protocol_response(
    struct json_object *request,
    int success,
    struct json_object *result,
    const char *error_code,
    const char *error_message
)
{
    struct json_object *response;
    struct json_object *id = NULL;

    response = json_object_new_object();
    if (response == NULL) {
        return NULL;
    }

    json_object_object_add(
        response,
        "type",
        json_object_new_string("response")
    );

    if (request != NULL) {
        json_object_object_get_ex(request, "id", &id);
    }

    if (id != NULL) {
        json_object_get(id);
        json_object_object_add(response, "id", id);
    } else {
        json_object_object_add(response, "id", json_object_new_null());
    }

    json_object_object_add(
        response,
        "ok",
        json_object_new_boolean(success != 0)
    );

    if (success) {
        if (result != NULL) {
            json_object_get(result);
            json_object_object_add(response, "result", result);
        } else {
            json_object_object_add(
                response,
                "result",
                json_object_new_object()
            );
        }
    } else {
        struct json_object *error = json_object_new_object();

        if (error == NULL) {
            json_object_put(response);
            return NULL;
        }

        json_object_object_add(
            error,
            "code",
            json_object_new_string(
                error_code != NULL ? error_code : "INTERNAL_ERROR"
            )
        );

        json_object_object_add(
            error,
            "message",
            json_object_new_string(
                error_message != NULL ? error_message : "Erreur interne"
            )
        );

        json_object_object_add(response, "error", error);
    }

    return response;
}
