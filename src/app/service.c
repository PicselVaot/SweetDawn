/*
 * app/service.c — Programme bin/sweetdawn-service : le service persistant
 *
 * Assemble les deux moitiés de SweetDawn :
 *
 *     AML005  <--Bluetooth-->  core/sweetdawn  <--IPC-->  clients
 *
 *   - core/sweetdawn possède l'unique liaison avec l'AML : il se
 *     connecte, se reconnecte et relit l'état périodiquement ;
 *   - ipc/ipc_server accepte les clients sur un socket Unix et transmet
 *     leurs commandes à app/handlers.c, qui les exécute avec
 *     core/control.c ;
 *   - les messages spontanés de l'AML (alarme, éclairage...) passent par
 *     app/handlers.c, qui les diffuse aux clients abonnés.
 *
 * Tout tourne dans un seul fil : la boucle du serveur IPC appelle
 * sweetdawn_tick() au moins une fois par seconde (voir service_tick).
 *
 * Usage : ./bin/sweetdawn-service [nom_appareil] [chemin_socket]
 *   nom_appareil  : entrée de config/aml.json (défaut : la première).
 *                   Passer "" pour garder le défaut et préciser le socket.
 *   chemin_socket : défaut $SWEETDAWN_SOCKET, sinon
 *                   /run/sweetdawn/sweetdawn.sock (voir ipc/ipc_server.h).
 *
 * En temps normal, il est lancé par systemd (systemd/sweetdawn.service).
 *
 * Arrêt propre : Ctrl+C ou SIGTERM.
 */

#define _POSIX_C_SOURCE 200809L

#include "app/handlers.h"
#include "core/sweetdawn.h"
#include "ipc/ipc_server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Chemin relatif : le service doit être lancé depuis la racine du projet. */
#define SERVICE_CONFIG_PATH "config/aml.json"

/*
 * Tâche périodique du service, appelée par la boucle IPC : entretien de
 * la liaison, puis de l'horloge de l'AML.
 */
static void service_tick(void *context)
{
    sweetdawn_tick(context);
    handlers_tick(context);
}

/* Envoie un événement de l'AML à tous les clients abonnés. */
static void service_broadcast(void *context, struct json_object *event)
{
    ipc_server_broadcast(context, event);
}

int main(int argc, char **argv)
{
    if (argc > 3) {
        fprintf(
            stderr,
            "Usage: %s [nom_appareil] [chemin_socket]\n",
            argv[0]
        );
        return 2;
    }

    const char *device_name = argc >= 2 ? argv[1] : NULL;
    const char *socket_path = argc >= 3 ? argv[2] : getenv(IPC_SOCKET_ENV);

    if (socket_path == NULL || socket_path[0] == '\0')
        socket_path = IPC_DEFAULT_SOCKET;

    SweetDawn app;
    IpcServer server;

    /*
     * Une écriture sur un client IPC disparu ne doit jamais tuer
     * le service : l'erreur est gérée par le code appelant.
     */
    signal(SIGPIPE, SIG_IGN);

    /*
     * Sous systemd, stdout est un tube : sans cela, les messages
     * n'apparaîtraient dans le journal que par blocs, bien après coup.
     */
    setvbuf(stdout, NULL, _IOLBF, 0);

    char error[256] = {0};

    int app_initialized = 0;
    int server_initialized = 0;
    int exit_code = 1;

    if (sweetdawn_init(
            &app,
            SERVICE_CONFIG_PATH,
            device_name,
            error,
            sizeof(error)) != 0) {
        fprintf(
            stderr,
            "SweetDawn: initialisation impossible: %s\n",
            error
        );
        return 1;
    }

    app_initialized = 1;

    if (handlers_attach(&app) != 0) {
        fprintf(stderr, "SweetDawn: commandes IPC impossibles a preparer.\n");
        goto cleanup;
    }

    /*
     * Aucune connexion n'est tentée ici : sweetdawn_tick(), appelée par
     * la boucle IPC, ouvre la liaison puis la maintient. Le service
     * démarre donc même si l'AML est éteint ou hors de portée, et se
     * connecte dès qu'il redevient joignable ; status indique
     * connected=false en attendant.
     */

    if (ipc_server_init(
            &server,
            socket_path,
            error,
            sizeof(error)) != 0) {
        fprintf(
            stderr,
            "SweetDawn: creation du socket IPC impossible: %s\n",
            error
        );
        goto cleanup;
    }

    server_initialized = 1;

    ipc_server_set_handler(
        &server,
        handlers_request,
        &app
    );

    ipc_server_set_tick(
        &server,
        service_tick,
        &app
    );

    /*
     * Événements : les messages spontanés de l'AML vont aux handlers,
     * qui les traitent et les diffusent aux clients abonnés.
     */
    sweetdawn_set_event_handler(&app, handlers_on_event, &app);
    handlers_set_broadcast(service_broadcast, &server);

    printf("SweetDawn service demarre.\n");
    printf("Appareil: %s\n", app.device->name);
    printf("Socket IPC: %s\n", socket_path);
    printf("Commandes: status, commands, ou un chemin (ex. Light/Enable)\n");
    fflush(stdout);

    if (ipc_server_run(&server) != 0) {
        fprintf(
            stderr,
            "SweetDawn: erreur dans la boucle IPC.\n"
        );
        goto cleanup;
    }

    exit_code = 0;

cleanup:
    if (server_initialized) {
        ipc_server_cleanup(&server);
    }

    if (app_initialized) {
        sweetdawn_cleanup(&app);
    }

    return exit_code;
}