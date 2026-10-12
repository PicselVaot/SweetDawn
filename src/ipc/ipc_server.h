#ifndef IPC_SERVER_H
#define IPC_SERVER_H

/*
 * ipc/ipc_server.h — Serveur local sur socket Unix
 *
 * C'est par ce serveur que les clients (CLI, GUI, MQTT...) parlent au
 * service, sans jamais toucher au Bluetooth eux-mêmes.
 *
 * Fonctionnement :
 *   - un seul fil d'exécution, une boucle autour de poll() ;
 *   - jusqu'à IPC_SERVER_MAX_CLIENTS clients en même temps ;
 *   - un message = un objet JSON sur une ligne, terminé par '\n' ;
 *   - chaque requête reçoit exactement une réponse (format détaillé
 *     dans le README, section « IPC protocol »).
 *
 * Le serveur ne connaît aucune commande : il valide l'enveloppe
 * (type, id, command, args) puis confie la commande au gestionnaire
 * enregistré avec ipc_server_set_handler(). Dans le service, c'est
 * handle_request() dans app/service.c.
 *
 * Utilisation type :
 *
 *   IpcServer server;
 *   ipc_server_init(&server, "/tmp/sweetdawn.sock", err, sizeof(err));
 *   ipc_server_set_handler(&server, ma_fonction, mon_contexte);
 *   ipc_server_set_tick(&server, ma_tache_periodique, mon_contexte);
 *   ipc_server_run(&server);       // bloque jusqu'à SIGINT / SIGTERM
 *   ipc_server_cleanup(&server);
 */

#include <stddef.h>
#include <json-c/json.h>

/*
 * Socket utilisé par défaut, par le service comme par ses clients.
 *
 * /run/sweetdawn/ est créé par systemd au démarrage du service
 * (RuntimeDirectory= dans systemd/sweetdawn.service). Pour un lancement
 * manuel, choisir un autre chemin avec la variable d'environnement
 * IPC_SOCKET_ENV (ex. SWEETDAWN_SOCKET=/tmp/sweetdawn.sock).
 */
#define IPC_DEFAULT_SOCKET "/run/sweetdawn/sweetdawn.sock"
#define IPC_SOCKET_ENV     "SWEETDAWN_SOCKET"

/* Nombre maximal de clients connectés simultanément. */
#define IPC_SERVER_MAX_CLIENTS 16

/* Taille maximale d'un message reçu, '\n' non compris. */
#define IPC_SERVER_MAX_MESSAGE 4096

/* Tailles des tampons de message et de code d'erreur. */
#define IPC_SERVER_ERROR_SIZE 256
#define IPC_SERVER_ERROR_CODE_SIZE 64

/*
 * Traite une commande validée par le transport IPC.
 *
 * Retour :
 *   0  : succès, result contient le résultat JSON
 *  -1  : erreur, error_code et error_message décrivent l'erreur
 *
 * Le serveur prend une référence sur result pour construire
 * la réponse. Le gestionnaire doit conserver sa propre gestion
 * de la durée de vie de l'objet.
 */
typedef int (*IpcRequestHandler)(
    const char *command,
    struct json_object *args,
    struct json_object **result,
    char *error_code,
    size_t error_code_capacity,
    char *error_message,
    size_t error_message_capacity,
    void *context
);

/*
 * Appelée à chaque tour de la boucle, au moins une fois par seconde,
 * qu'il y ait ou non des clients. Elle sert aux tâches périodiques du
 * service (entretien de la liaison Bluetooth, par exemple).
 *
 * Elle s'exécute dans le même fil que les requêtes : si elle dure,
 * les clients attendent. Elle peut appeler ipc_server_stop().
 */
typedef void (*IpcTickHandler)(void *context);

typedef struct {
    int listen_fd;     /* socket d'écoute, -1 si fermé                   */
    int running;       /* 0 dès qu'un arrêt est demandé                   */
    int owns_socket;   /* 1 si c'est nous qui avons créé le fichier socket */

    /* 108 : taille de sun_path dans struct sockaddr_un sous Linux. */
    char socket_path[108];

    IpcRequestHandler handler;
    void *handler_context;

    IpcTickHandler tick;
    void *tick_context;
} IpcServer;

/*
 * Initialise le serveur et crée le socket Unix.
 *
 * Si le chemin est déjà occupé par un socket que plus aucun processus
 * n'écoute (reste d'un arrêt brutal), il est remplacé. Un socket encore
 * actif, ou un fichier qui n'est pas un socket, n'est jamais touché :
 * l'initialisation échoue alors.
 */
int ipc_server_init(
    IpcServer *server,
    const char *socket_path,
    char *error_message,
    size_t error_capacity
);

/* Configure le gestionnaire des commandes applicatives. */
void ipc_server_set_handler(
    IpcServer *server,
    IpcRequestHandler handler,
    void *context
);

/* Configure la tâche périodique (peut être NULL pour la retirer). */
void ipc_server_set_tick(
    IpcServer *server,
    IpcTickHandler tick,
    void *context
);

/* Lance la boucle de traitement des clients. */
int ipc_server_run(IpcServer *server);

/* Demande l'arrêt de la boucle du serveur. */
void ipc_server_stop(IpcServer *server);

/* Ferme le serveur et supprime son socket s'il lui appartient. */
void ipc_server_cleanup(IpcServer *server);

#endif