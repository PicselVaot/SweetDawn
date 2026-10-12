#ifndef HANDLERS_H
#define HANDLERS_H

/*
 * app/handlers.h — Commandes IPC du service
 *
 * Fait le lien entre ce qu'un client demande par IPC et ce que
 * core/control.c sait faire. Trois sortes de commandes :
 *
 *   "status"    état de la liaison et réglages mémorisés (immédiat) ;
 *   "commands"  liste de toutes les commandes disponibles ;
 *   un chemin de l'arbre (command/aml_commands.c), par exemple :
 *
 *     {"command": "Light/Brightness", "args": [75]}   régler une valeur
 *     {"command": "Radio/>>",         "args": []}     lancer une action
 *     {"command": "Light",            "args": []}     lire l'état d'un groupe
 *
 * Pour un chemin, la valeur est d'abord validée par l'arbre (type,
 * limites, choix possibles) : un handler ne reçoit que des valeurs
 * valides.
 */

#include <stddef.h>
#include <json-c/json.h>

#include "core/sweetdawn.h"

/*
 * Attache les handlers aux commandes de l'arbre de app.
 * À appeler une fois, après sweetdawn_init().
 */
int handlers_attach(SweetDawn *app);

/*
 * Tâche périodique : remet l'AML à l'heure si besoin (voir handlers.c).
 * À appeler à chaque tour de la boucle du service, après sweetdawn_tick().
 */
void handlers_tick(SweetDawn *app);

/*
 * Exécute une requête IPC. Signature d'un IpcRequestHandler
 * (ipc/ipc_server.h), avec context = le SweetDawn.
 */
int handlers_request(
    const char *command,
    struct json_object *args,
    struct json_object **result,
    char *error_code,
    size_t error_code_capacity,
    char *error_message,
    size_t error_message_capacity,
    void *context
);

#endif /* HANDLERS_H */
