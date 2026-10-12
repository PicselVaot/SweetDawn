#ifndef HANDLERS_H
#define HANDLERS_H

/*
 * app/handlers.h — Commandes IPC du service
 *
 * Fait le lien entre ce qu'un client demande par IPC et ce que
 * core/control.c sait faire. Trois sortes de commandes :
 *
 *   "status"    état de la liaison, réglages mémorisés, heure de l'AML
 *               et état du réveil (immédiat) ;
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
#include <stdint.h>
#include <json-c/json.h>

#include "core/sweetdawn.h"

/*
 * Attache les handlers aux commandes de l'arbre de app.
 * À appeler une fois, après sweetdawn_init().
 */
int handlers_attach(SweetDawn *app);

/*
 * Tâche périodique : traite les événements de l'AML, suit l'état du
 * réveil, remet l'AML à l'heure si besoin (voir handlers.c).
 * À appeler à chaque tour de la boucle du service, après sweetdawn_tick().
 */
void handlers_tick(SweetDawn *app);

/*
 * Gestionnaire des messages spontanés de l'AML, à enregistrer avec
 * sweetdawn_set_event_handler() (après handlers_attach()). Il ne fait
 * que noter : le traitement a lieu dans handlers_tick().
 */
void handlers_on_event(void *context, uint8_t code,
                       const uint8_t *data, size_t length);

/*
 * Diffusion des événements aux clients : le service y branche
 * ipc_server_broadcast(). event est l'objet complet
 * {"type":"event","event":...,"data":...} ; il est libéré après l'appel.
 */
typedef void (*HandlersBroadcast)(void *context, struct json_object *event);

void handlers_set_broadcast(HandlersBroadcast broadcast, void *context);

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
