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
 *     leurs commandes à handle_request(), ci-dessous.
 *
 * Tout tourne dans un seul fil : la boucle du serveur IPC appelle
 * sweetdawn_tick() au moins une fois par seconde (voir service_tick).
 *
 * Usage : ./bin/sweetdawn-service [nom_appareil] [chemin_socket]
 *   nom_appareil  : entrée de config/aml.json (défaut : la première).
 *                   Passer "" pour garder le défaut et préciser le socket.
 *   chemin_socket : défaut /tmp/sweetdawn.sock.
 *
 * Arrêt propre : Ctrl+C ou SIGTERM.
 */

#define _POSIX_C_SOURCE 200809L

#include "core/sweetdawn.h"
#include "ipc/ipc_server.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

/* Chemin relatif : le service doit être lancé depuis la racine du projet. */
#define SERVICE_CONFIG_PATH "config/aml.json"
#define SERVICE_DEFAULT_SOCKET "/tmp/sweetdawn.sock"

/* Adapte sweetdawn_tick() à la signature attendue par le serveur IPC. */
static void service_tick(void *context)
{
    sweetdawn_tick(context);
}

/* Copie un texte dans un tampon d'erreur fourni par le serveur IPC. */
static void set_text(
    char *buffer,
    size_t capacity,
    const char *text)
{
    if (buffer != NULL && capacity > 0) {
        snprintf(buffer, capacity, "%s", text);
    }
}

/*
 * Exécute une commande reçue d'un client IPC.
 *
 * Seule « status » existe pour l'instant : elle renvoie l'état mémorisé,
 * sans interroger l'AML (la lecture est faite en tâche de fond par
 * sweetdawn_tick). La réponse est donc immédiate, même si l'AML est
 * injoignable.
 *
 * Pour ajouter une commande : tester son nom ici, remplir *result en
 * cas de succès, ou error_code / error_message en cas d'échec.
 */
static int handle_request(
    const char *command,
    struct json_object *args,
    struct json_object **result,
    char *error_code,
    size_t error_code_capacity,
    char *error_message,
    size_t error_message_capacity,
    void *context)
{
    SweetDawn *app = context;

    if (command == NULL ||
        args == NULL ||
        result == NULL ||
        app == NULL) {
        set_text(
            error_code,
            error_code_capacity,
            "INVALID_REQUEST"
        );
        set_text(
            error_message,
            error_message_capacity,
            "Arguments invalides pour la commande"
        );
        return -1;
    }

    if (json_object_array_length(args) != 0) {
        set_text(
            error_code,
            error_code_capacity,
            "INVALID_ARGUMENTS"
        );
        set_text(
            error_message,
            error_message_capacity,
            "Cette commande n'accepte aucun argument"
        );
        return -1;
    }

    if (strcmp(command, "status") != 0) {
        set_text(
            error_code,
            error_code_capacity,
            "COMMAND_NOT_IMPLEMENTED"
        );
        set_text(
            error_message,
            error_message_capacity,
            "Cette commande n'est pas encore implementee"
        );
        return -1;
    }

    const AmlState *state = sweetdawn_get_state(app);

    if (state == NULL) {
        set_text(
            error_code,
            error_code_capacity,
            "STATE_UNAVAILABLE"
        );
        set_text(
            error_message,
            error_message_capacity,
            "L'etat SweetDawn n'est pas disponible"
        );
        return -1;
    }

    struct json_object *object = json_object_new_object();

    if (object == NULL) {
        set_text(
            error_code,
            error_code_capacity,
            "INTERNAL_ERROR"
        );
        set_text(
            error_message,
            error_message_capacity,
            "Impossible de creer la reponse JSON"
        );
        return -1;
    }

    /*
     * connected : la liaison Bluetooth est-elle ouverte en ce moment ?
     * known     : a-t-on déjà lu l'état au moins une fois ?
     * age_ms    : ancienneté de la dernière lecture. Pendant une panne,
     *             les valeurs restent celles de la dernière lecture :
     *             c'est age_ms qui permet de savoir si elles sont fraîches.
     */
    json_object_object_add(
        object,
        "connected",
        json_object_new_boolean(sweetdawn_is_connected(app))
    );

    json_object_object_add(
        object,
        "known",
        json_object_new_boolean(state->known)
    );

    if (state->known) {
        json_object_object_add(
            object,
            "age_ms",
            json_object_new_int64(sweetdawn_state_age_ms(app))
        );
        json_object_object_add(
            object,
            "authorization",
            json_object_new_int(app->authorization)
        );
        json_object_object_add(
            object, "display", json_object_new_int(state->display)
        );
        json_object_object_add(
            object, "date", json_object_new_int(state->date)
        );
        json_object_object_add(
            object, "time", json_object_new_int(state->time)
        );
        json_object_object_add(
            object, "feedback", json_object_new_int(state->feedback)
        );
        json_object_object_add(
            object, "fade", json_object_new_int(state->fade)
        );
    }

    *result = object;

    return 0;
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
    const char *socket_path =
        argc >= 3 ? argv[2] : SERVICE_DEFAULT_SOCKET;

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
        handle_request,
        &app
    );

    ipc_server_set_tick(
        &server,
        service_tick,
        &app
    );

    printf("SweetDawn service demarre.\n");
    printf("Appareil: %s\n", app.device->name);
    printf("Socket IPC: %s\n", socket_path);
    printf("Commande disponible: status\n");
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