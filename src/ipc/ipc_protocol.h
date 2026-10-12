#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

/*
 * ipc/ipc_protocol.h — Messages JSON échangés avec les clients IPC
 *
 * Deux fonctions sans état, utilisées par ipc_server.c :
 *   - lire un message reçu et vérifier que c'est un objet JSON ;
 *   - fabriquer la réponse correspondante.
 *
 * Exemple de réponse en succès :
 *   {"type":"response","id":1,"ok":true,"result":{...}}
 * et en erreur :
 *   {"type":"response","id":1,"ok":false,
 *    "error":{"code":"INVALID_REQUEST","message":"..."}}
 *
 * L'id de la requête est recopié dans la réponse : c'est ce qui permet
 * à un client d'associer chaque réponse à sa requête.
 */

#include <stddef.h>
#include <json-c/json.h>

/*
 * Analyse un message JSON.
 *
 * Le message doit être exactement un objet JSON : rien avant, rien
 * après, pas de tableau ni de valeur simple. L'analyse est stricte
 * (JSON_TOKENER_STRICT) : pas de virgule finale, pas de commentaire.
 *
 * Retour :
 *   0  : message JSON valide
 *  -1  : message invalide
 *
 * Le résultat appartient à l'appelant, qui doit le libérer
 * avec json_object_put().
 */
int ipc_protocol_parse(
    const char *message,
    size_t message_length,
    struct json_object **result
);

/*
 * Construit une réponse JSON à une requête.
 *
 * request       : requête d'origine, pour en recopier l'id (peut être
 *                 NULL si elle était illisible : l'id vaut alors null).
 * success       : différent de 0 pour une réponse en succès.
 * result        : contenu de "result" en succès (NULL : objet vide).
 *                 La fonction prend sa propre référence : l'appelant
 *                 reste responsable de libérer le sien.
 * error_code    : code d'erreur (NULL : "INTERNAL_ERROR").
 * error_message : explication (NULL : message générique).
 *
 * Retourne un objet à libérer avec json_object_put(), ou NULL.
 */
struct json_object *ipc_protocol_response(
    struct json_object *request,
    int success,
    struct json_object *result,
    const char *error_code,
    const char *error_message
);

#endif
