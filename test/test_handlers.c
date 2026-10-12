/*
 * test/test_handlers.c — Tests des commandes IPC, de bout en bout
 * (app/handlers + core/control + core/sweetdawn)
 *
 * Chaque test appelle handlers_request() comme le ferait le serveur IPC,
 * puis vérifie :
 *   - les octets réellement envoyés au faux AML (fake_aml.c) ;
 *   - la réponse JSON ou le code d'erreur renvoyé au client.
 *
 * Les trames attendues sont construites à la main d'après
 * docs/PROTOCOL.md, pour ne pas tester le code avec lui-même.
 */

#include "app/handlers.h"
#include "fake_aml.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Mini cadre de test                                                        */
/* ------------------------------------------------------------------------- */

static int failures = 0;

#define CHECK(condition, message)                                      \
    do {                                                               \
        if (condition) {                                               \
            printf("  OK : %s\n", message);                            \
        } else {                                                       \
            printf("  ECHEC : %s (ligne %d)\n", message, __LINE__);    \
            failures++;                                                \
        }                                                              \
    } while (0)

/* ------------------------------------------------------------------------- */
/* Outils                                                                    */
/* ------------------------------------------------------------------------- */

static SweetDawn app;

/* Résultat du dernier appel à requete(). */
static int last_rc;
static char last_error[64];
static struct json_object *last_result;

/* Application connectée, réglages lus (display=1, feedback et fade actifs). */
static void connecter(void)
{
    const uint8_t status[] = { 0x16 };
    const uint8_t settings[] = { 0x01, 0x00, 0x00, 0x00, 0x00 };

    sweetdawn_cleanup(&app);
    fake_setup(&app);
    handlers_attach(&app);

    fake_push_response(0xF0, status, sizeof(status));
    fake_push_response(0xE2, settings, sizeof(settings));
    sweetdawn_tick(&app);

    /* On ne garde que ce que le test lui-même enverra. */
    fake_sent_count = 0;
}

/* Exécute une requête IPC ; args_json est un tableau JSON, ex. "[75]". */
static void requete(const char *command, const char *args_json)
{
    struct json_object *args = json_tokener_parse(args_json);
    char message[256] = "";

    if (last_result != NULL) {
        json_object_put(last_result);
        last_result = NULL;
    }

    last_error[0] = '\0';
    last_rc = handlers_request(command, args, &last_result,
                               last_error, sizeof(last_error),
                               message, sizeof(message), &app);
    json_object_put(args);
}

/* Le n-ième envoi est-il exactement cette trame ? */
static int trame_envoyee(int n, const uint8_t *expected, size_t length)
{
    return n < fake_sent_count &&
           fake_sent[n].length == length &&
           memcmp(fake_sent[n].data, expected, length) == 0;
}

/* Code de commande et données du n-ième envoi. */
static uint8_t code_envoye(int n)
{
    return fake_sent[n].data[7];
}

static const uint8_t *donnees_envoyees(int n)
{
    return &fake_sent[n].data[8];
}

/* Champ entier ou texte du dernier résultat. */
static int champ_entier(const char *key)
{
    struct json_object *value = NULL;

    json_object_object_get_ex(last_result, key, &value);
    return json_object_get_int(value);
}

static const char *champ_texte(const char *key)
{
    struct json_object *value = NULL;

    if (!json_object_object_get_ex(last_result, key, &value))
        return "";

    return json_object_get_string(value);
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                     */
/* ------------------------------------------------------------------------- */

static void test_trame_exacte(void)
{
    /* Light/Enable true : code 0x37, cible 0x01 ; checksum 04^37^01 = 32. */
    const uint8_t expected[] = {
        0xFE, 0xEF, 0x0A, 0x09, 0xAB, 0xAA,
        0x04, 0x37, 0x01, 0x32, 0x55, 0x0D, 0x0A
    };

    printf("\n[1] Trame exacte\n");
    connecter();

    requete("Light/Enable", "[true]");

    CHECK(last_rc == 0, "Light/Enable réussit");
    CHECK(trame_envoyee(0, expected, sizeof(expected)),
          "la trame envoyée est octet pour octet celle attendue");
}

static void test_validation(void)
{
    printf("\n[2] Validation avant envoi\n");
    connecter();

    requete("Light/Brightness", "[150]");
    CHECK(last_rc != 0 && strcmp(last_error, "INVALID_VALUE") == 0,
          "une luminosité de 150 est refusée");

    requete("Light/Enable", "[]");
    CHECK(strcmp(last_error, "INVALID_ARGUMENTS") == 0,
          "un réglage sans valeur est refusé");

    requete("Radio/>>", "[1]");
    CHECK(strcmp(last_error, "INVALID_ARGUMENTS") == 0,
          "une action avec une valeur est refusée");

    requete("Light/Teleport", "[]");
    CHECK(strcmp(last_error, "UNKNOWN_COMMAND") == 0,
          "une commande inconnue est refusée");

    requete("Mood/Color", "[\"300,0,0\"]");
    CHECK(strcmp(last_error, "INVALID_VALUE") == 0,
          "une composante de couleur > 255 est refusée");

    requete("Alarm/1/Time", "[\"25:00\"]");
    CHECK(strcmp(last_error, "INVALID_VALUE") == 0,
          "une heure 25:00 est refusée");

    CHECK(fake_sent_count == 0, "aucune valeur refusée n'est partie à l'AML");
}

static void test_eclairage(void)
{
    printf("\n[3] Éclairage\n");
    connecter();

    requete("Light/Brightness", "[75]");
    CHECK(last_rc == 0 && code_envoye(0) == 0x31 &&
          donnees_envoyees(0)[0] == 0x01 && donnees_envoyees(0)[1] == 75,
          "Light/Brightness 75 envoie 31 01 4B");

    requete("Mood/Brightness", "[\"20\"]");
    CHECK(code_envoye(1) == 0x31 && donnees_envoyees(1)[0] == 0x02,
          "Mood vise la cible 2 (valeur texte acceptée)");

    requete("Mood/Color", "[\"255,0,128\"]");
    CHECK(code_envoye(2) == 0x32 && donnees_envoyees(2)[0] == 255 &&
          donnees_envoyees(2)[1] == 0 && donnees_envoyees(2)[2] == 128,
          "Mood/Color envoie 32 FF 00 80");

    requete("Mood/Scene", "[\"Wave\"]");
    CHECK(code_envoye(3) == 0x34 && donnees_envoyees(3)[0] == 6,
          "Mood/Scene Wave envoie l'index 6");

    requete("Light/Sunset", "[false]");
    CHECK(code_envoye(4) == 0x36 && donnees_envoyees(4)[0] == 0x01,
          "Light/Sunset false envoie 36 01");

    /* Réponse réelle de Light : 5 octets seulement (pas de couleur). */
    const uint8_t light[] = { 1, 1, 40, 0, 10 };
    fake_push_response(0xD0, light, sizeof(light));

    requete("Light", "[]");
    CHECK(last_rc == 0 && code_envoye(5) == 0x30 &&
          donnees_envoyees(5)[0] == 0x01,
          "Light (lecture) envoie 30 01");
    CHECK(champ_entier("brightness") == 40, "la luminosité lue est 40");
}

static void test_reglages(void)
{
    printf("\n[4] Réglages (les cinq écrits d'un coup)\n");
    connecter();

    fake_push_response(0xF2, NULL, 0);
    requete("Settings/Feedback", "[false]");

    const uint8_t *d = donnees_envoyees(0);

    CHECK(last_rc == 0 && code_envoye(0) == 0x02, "Settings envoie 0x02");
    CHECK(d[0] == 1 && d[1] == 0 && d[2] == 0 && d[4] == 0,
          "les autres réglages sont repris de l'état connu");
    CHECK(d[3] == 1, "feedback false est encodé 1 (valeur inversée)");
    CHECK(app.state.feedback == 1, "l'état mémorisé est mis à jour");

    fake_push_response(0xF2, NULL, 0);
    requete("Settings/Display brightness", "[\"A\"]");
    CHECK(donnees_envoyees(1)[0] == 2, "Display brightness A = 2");

    requete("status", "[]");

    struct json_object *settings = NULL;
    struct json_object *display = NULL;

    json_object_object_get_ex(last_result, "settings", &settings);
    json_object_object_get_ex(settings, "display_brightness", &display);

    CHECK(display != NULL && strcmp(json_object_get_string(display), "A") == 0,
          "status décode les réglages (display_brightness = A)");
}

static void test_heure(void)
{
    printf("\n[9] Réglage manuel de l'heure\n");
    connecter();

    fake_push_response(0xF1, NULL, 0);
    requete("Settings/Set time", "[\"2026-03-01 07:45\"]");

    const uint8_t *d = donnees_envoyees(0);

    /* s, min, h, jour sem., jour, mois, année-2000, 02 ; 01/03/2026 = dimanche. */
    CHECK(last_rc == 0 && code_envoye(0) == 0x01, "Set time envoie 0x01");
    CHECK(d[0] == 0 && d[1] == 45 && d[2] == 7, "secondes 0, 45 min, 7 h");
    CHECK(d[3] == 0 && d[4] == 1 && d[5] == 3 && d[6] == 26 && d[7] == 2,
          "dimanche 1er mars 2026, puis la constante 02");

    fake_push_response(0xF1, NULL, 0);
    requete("Settings/Set time", "[\"23:59:30\"]");
    d = donnees_envoyees(1);
    CHECK(last_rc == 0 && d[0] == 30 && d[1] == 59 && d[2] == 23,
          "HH:MM:SS seul garde la date du jour");

    requete("Settings/Set time", "[\"2026-02-30 10:00\"]");
    CHECK(strcmp(last_error, "INVALID_VALUE") == 0, "le 30 février est refusé");

    requete("Settings/Set time", "[\"24:00\"]");
    CHECK(strcmp(last_error, "INVALID_VALUE") == 0, "24:00 est refusé");

    CHECK(fake_sent_count == 2, "rien n'est envoyé pour une heure refusée");

    /* En mode manual, l'AML ignore l'heure : repasser d'abord en auto. */
    app.state.time = 1;
    fake_sent_count = 0;
    fake_push_response(0xF2, NULL, 0);
    fake_push_response(0xF1, NULL, 0);
    requete("Settings/Set time", "[\"07:45\"]");

    CHECK(last_rc == 0 && fake_sent_count == 2 &&
          code_envoye(0) == 0x02 && donnees_envoyees(0)[2] == 0 &&
          code_envoye(1) == 0x01,
          "en mode manual : réglages en auto (02), puis l'heure (01)");
    CHECK(app.state.time == 0, "le mode mémorisé est maintenant auto");
}

static void test_alarme(void)
{
    printf("\n[5] Alarme (lecture puis réécriture)\n");
    connecter();

    /* Alarm/2 = index 7 : activée, 07:30, lun-ven, sonnerie 2, ... */
    const uint8_t alarm[] = { 0x87, 1, 30, 7, 0x3E, 2, 5, 1, 1, 20, 80 };

    fake_push_response(0xF3, alarm, sizeof(alarm));
    fake_push_response(0xF3, NULL, 0);

    requete("Alarm/2/Time", "[\"06:45\"]");

    CHECK(last_rc == 0, "Alarm/2/Time réussit");
    CHECK(code_envoye(0) == 0x03 && donnees_envoyees(0)[0] == 0x87 &&
          donnees_envoyees(0)[1] == 0x02,
          "lecture : 03 87 02");

    const uint8_t *d = donnees_envoyees(1);

    CHECK(code_envoye(1) == 0x03 && d[0] == 0x07,
          "écriture : index 7, sans bit de lecture");
    CHECK(d[2] == 45 && d[3] == 6, "minute puis heure : 45, 6");
    CHECK(d[1] == 1 && d[4] == 0x3E && d[5] == 2 && d[10] == 80,
          "les autres réglages sont conservés");

    fake_push_response(0xF3, alarm, sizeof(alarm));
    requete("Alarm/2", "[]");
    CHECK(strcmp(champ_texte("time"), "07:30") == 0 &&
          strcmp(champ_texte("snooze"), "2") == 0,
          "lecture décodée : 07:30, snooze 2 min");
}

static void test_radio(void)
{
    printf("\n[6] Radio\n");
    connecter();

    /* Réponse : rappel 1 0, puis 0x03F7 = 1015 = 101,5 MHz. */
    const uint8_t tune[] = { 1, 0, 0x03, 0xF7 };
    fake_push_response(0xFA, tune, sizeof(tune));

    requete("Radio/>>", "[]");

    struct json_object *frequency = NULL;
    json_object_object_get_ex(last_result, "frequency", &frequency);

    CHECK(last_rc == 0 && code_envoye(0) == 0x0A &&
          donnees_envoyees(0)[0] == 1 && donnees_envoyees(0)[1] == 0,
          "Radio/>> envoie 0A 01 00 (recherche vers le haut)");
    CHECK(frequency != NULL && json_object_get_double(frequency) > 101.49 &&
          json_object_get_double(frequency) < 101.51,
          "la nouvelle fréquence est 101.5");

    requete("Radio/Volume", "[4]");
    CHECK(code_envoye(1) == 0x0B && donnees_envoyees(1)[0] == 4,
          "Radio/Volume 4 envoie 0B 04");
}

static void test_erreurs_aml(void)
{
    printf("\n[7] Erreurs de l'AML\n");
    connecter();

    requete("Radio/Enable", "[true]");
    CHECK(strcmp(last_error, "AML_TIMEOUT") == 0,
          "pas de réponse : AML_TIMEOUT");

    sweetdawn_cleanup(&app);
    fake_setup(&app);
    handlers_attach(&app);

    requete("Light/Enable", "[true]");
    CHECK(strcmp(last_error, "AML_DISCONNECTED") == 0 &&
          fake_sent_count == 0,
          "sans liaison : AML_DISCONNECTED, rien n'est envoyé");
}

static void test_commands(void)
{
    printf("\n[8] Liste des commandes\n");
    connecter();

    requete("commands", "[]");
    CHECK(last_rc == 0 && strcmp(champ_texte("name"), "AML") == 0,
          "commands renvoie l'arbre depuis la racine AML");
}

int main(void)
{
    printf("=== Tests des commandes IPC ===\n");

    test_trame_exacte();
    test_validation();
    test_eclairage();
    test_reglages();
    test_alarme();
    test_radio();
    test_erreurs_aml();
    test_commands();
    test_heure();

    if (last_result != NULL)
        json_object_put(last_result);

    sweetdawn_cleanup(&app);

    if (failures != 0) {
        printf("\n%d test(s) en échec.\n", failures);
        return 1;
    }

    printf("\nTous les tests sont passés.\n");
    return 0;
}
