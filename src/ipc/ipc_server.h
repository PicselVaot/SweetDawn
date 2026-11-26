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
 * handlers_request() dans app/handlers.c.
 *
 * Seule exception, "subscribe" : le client est abonné aux événements,
 * que le service diffuse avec ipc_server_broadcast(). Ils arrivent sur
 * la même connexion, sous la forme {"type":"event", ...}.
 *
 * Accès web (facultatif, ipc_server_listen_http) : la même boucle peut
 * aussi écouter en TCP et parler un HTTP minimal, pour qu'un navigateur
 * pilote l'AML sans autre programme sur le Raspberry Pi :
 *   GET  /        page de contrôle (gzip embarqué, voir web/index.html) ;
 *   POST /api     corps = une requête IPC, réponse = la réponse IPC ;
 *   GET  /events  événements en direct (Server-Sent Events).
 * Aucun fichier n'est lu, aucun processus n'est lancé : une requête web
 * coûte autant qu'une requête IPC.
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

/*
 * Nombre maximal de clients connectés simultanément, IPC et web
 * confondus (un navigateur ouvert en occupe deux ou trois).
 */
#define IPC_SERVER_MAX_CLIENTS 32

/*
 * Accès web : variable d'environnement donnant l'adresse d'écoute,
 * "port" ou "adresse:port" ("80", "0.0.0.0:80", "127.0.0.1:8080").
 * "off" désactive le web. Défaut : IPC_HTTP_DEFAULT.
 */
#define IPC_HTTP_ENV     "SWEETDAWN_HTTP"
#define IPC_HTTP_DEFAULT "80"

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

struct IpcClient;      /* état d'un client, privé à ipc_server.c */

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

    /* Clients connectés, valable pendant ipc_server_run() seulement. */
    struct IpcClient *clients;

    /* Accès web : socket TCP d'écoute, -1 si désactivé. */
    int http_fd;

    /* Page servie sur GET /, déjà compressée en gzip (peut être NULL). */
    const unsigned char *page;
    size_t page_length;
    char page_etag[24];
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

/*
 * Ouvre l'accès web sur address ("80", "0.0.0.0:80", "[::1]:8080"...).
 * À appeler après ipc_server_init(). En cas d'échec (port déjà pris,
 * droits insuffisants pour un port < 1024), le serveur IPC reste
 * utilisable : seul le web est indisponible.
 */
int ipc_server_listen_http(
    IpcServer *server,
    const char *address,
    char *error_message,
    size_t error_capacity
);

/*
 * Page servie sur GET /, compressée en gzip. Le tableau n'est pas
 * copié : il doit rester valide (en pratique, une constante embarquée).
 */
void ipc_server_set_page(
    IpcServer *server,
    const unsigned char *gzip_page,
    size_t length
);

/* Lance la boucle de traitement des clients. */
int ipc_server_run(IpcServer *server);

/*
 * Envoie un événement à tous les clients abonnés ("subscribe").
 *
 * À appeler pendant ipc_server_run() (depuis la tâche périodique, par
 * exemple). event est sérialisé tel quel ; l'appelant le libère.
 * Un client trop en retard pour suivre est déconnecté.
 *
 * Retour : 0, ou -1 hors de la boucle ou en cas d'erreur.
 */
int ipc_server_broadcast(IpcServer *server, struct json_object *event);

/* Demande l'arrêt de la boucle du serveur. */
void ipc_server_stop(IpcServer *server);

/* Ferme le serveur et supprime son socket s'il lui appartient. */
void ipc_server_cleanup(IpcServer *server);

#endif