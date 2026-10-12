/*
 * test/test_sweetdawn.c — Tests de la supervision de liaison (core/sweetdawn)
 *
 * Ces tests vérifient, sans Bluetooth ni AML005 :
 *   1. la connexion initiale et la première lecture de l'état ;
 *   2. la reconnexion après une liaison perdue ;
 *   3. l'attente croissante entre deux connexions échouées ;
 *   4. le rejet des trames qui ne sont pas la réponse attendue ;
 *   5. l'abandon d'une liaison devenue silencieuse ;
 *   6. la conservation de l'état pendant une panne.
 *
 * Le transport (aml/transport.c) est remplacé par un faux scripté défini ici,
 * et l'horloge de SweetDawn est pilotée par le test : aucune attente réelle.
 */

#include "core/sweetdawn.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Mini cadre de test (jamais compilé hors de l'évaluation : pas d'assert)   */
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
/* Horloge pilotée par le test                                               */
/* ------------------------------------------------------------------------- */

static int64_t fake_now = 0;

static int64_t fake_clock(void)
{
    return fake_now;
}

/* ------------------------------------------------------------------------- */
/* Faux transport AML                                                        */
/* ------------------------------------------------------------------------- */

#define QUEUE_MAX 8

typedef struct {
    uint8_t data[AML_PROTOCOL_MAX_FRAME_SIZE];
    size_t length;
} FakeFrame;

static FakeFrame fake_queue[QUEUE_MAX];
static int fake_queue_count = 0;
static int fake_queue_head = 0;

static int fake_connect_error = 0;   /* code renvoyé par aml_connect */
static int fake_send_error = 0;      /* code renvoyé par aml_send */
static int fake_empty_code = AML_ERR_TIMEOUT;

static int fake_connect_calls = 0;
static int fake_disconnect_calls = 0;
static int fake_send_calls = 0;
static uint8_t fake_last_request_code = 0;

static void fake_reset(void)
{
    fake_queue_count = 0;
    fake_queue_head = 0;
    fake_connect_error = 0;
    fake_send_error = 0;
    fake_empty_code = AML_ERR_TIMEOUT;
    fake_connect_calls = 0;
    fake_disconnect_calls = 0;
    fake_send_calls = 0;
    fake_last_request_code = 0;
}

/* Construit une trame de réponse : FE EF 0C LL AB BB .. code args chk 55 0D 0A */
static void fake_push_response(
    uint8_t code,
    const uint8_t *args,
    size_t args_length)
{
    FakeFrame *frame = &fake_queue[fake_queue_count++];
    uint8_t *f = frame->data;
    uint8_t checksum = 0;
    size_t total = args_length + 12;

    f[0] = 0xFE;
    f[1] = 0xEF;
    f[2] = 0x0C;
    f[3] = (uint8_t)(total - 4);
    f[4] = 0xAB;
    f[5] = 0xBB;
    f[6] = (uint8_t)(args_length + 3);
    f[7] = code;

    if (args_length > 0)
        memcpy(&f[8], args, args_length);

    for (size_t i = 6; i < 8 + args_length; i++)
        checksum ^= f[i];

    f[8 + args_length] = checksum;
    f[9 + args_length] = 0x55;
    f[10 + args_length] = 0x0D;
    f[11 + args_length] = 0x0A;

    frame->length = total;
}

/* Réponse de statut (autorisation 22) puis de paramètres display..fade. */
static void fake_push_refresh(uint8_t display, uint8_t fade)
{
    const uint8_t status[] = {0x16};
    const uint8_t settings[] = {display, 0x00, 0x01, 0x00, fade};

    fake_push_response(0xF0, status, sizeof(status));
    fake_push_response(0xE2, settings, sizeof(settings));
}

int aml_connect(AmlConnection *connection, const AmlConfig *config)
{
    (void)config;
    fake_connect_calls++;

    if (fake_connect_error != 0)
        return fake_connect_error;

    connection->socket_fd = 42;
    return 0;
}

int aml_send(AmlConnection *connection, const uint8_t *data, size_t length)
{
    (void)connection;
    fake_send_calls++;

    if (length >= 8)
        fake_last_request_code = data[7];

    return fake_send_error;
}

int aml_receive(
    AmlConnection *connection,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length,
    int timeout_ms)
{
    (void)connection;
    (void)timeout_ms;

    if (fake_queue_head >= fake_queue_count)
        return fake_empty_code;

    FakeFrame *frame = &fake_queue[fake_queue_head++];

    if (frame->length > output_capacity)
        return AML_ERR_PROTOCOL;

    memcpy(output, frame->data, frame->length);
    *output_length = frame->length;

    return 0;
}

size_t aml_flush(AmlConnection *connection)
{
    (void)connection;
    return 0;
}

void aml_disconnect(AmlConnection *connection)
{
    if (connection->socket_fd >= 0)
        fake_disconnect_calls++;

    connection->socket_fd = -1;
}

/* sweetdawn.c référence le module de configuration : inutile ici. */
int config_load(
    const char *path,
    AppConfig *config,
    char *error_message,
    size_t error_capacity)
{
    (void)path;
    (void)config;
    (void)error_message;
    (void)error_capacity;
    return -1;
}

const AmlConfig *config_select(const AppConfig *config, const char *name)
{
    (void)config;
    (void)name;
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Outils                                                                    */
/* ------------------------------------------------------------------------- */

static AmlConfig fake_device = {
    .name = "test",
    .aml_mac = "00:00:00:00:00:01",
    .aml_chn = 1,
    .blt_mac = "00:00:00:00:00:02"
};

/* Prépare une application prête à l'emploi (équivalent de sweetdawn_init). */
static void setup(SweetDawn *app)
{
    memset(app, 0, sizeof(*app));
    app->connection.socket_fd = -1;
    app->last_refresh_ms = -1;
    app->device = &fake_device;
    app->initialized = true;
    app->clock_ms = fake_clock;

    state_init(&app->state);

    fake_reset();
    fake_now = 0;
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                     */
/* ------------------------------------------------------------------------- */

static void test_connexion_initiale(void)
{
    SweetDawn app;

    printf("\n[1] Connexion initiale\n");
    setup(&app);

    CHECK(sweetdawn_state_age_ms(&app) == -1,
          "l'âge de l'état est inconnu avant toute lecture");

    fake_push_refresh(0x02, 0x01);
    sweetdawn_tick(&app);

    CHECK(sweetdawn_is_connected(&app), "la liaison est ouverte");
    CHECK(app.state.known, "l'état est connu");
    CHECK(app.state.display == 0x02 && app.state.fade == 0x01,
          "les paramètres lus sont mémorisés");
    CHECK(app.authorization == 0x16, "l'autorisation est mémorisée");
    CHECK(sweetdawn_state_age_ms(&app) == 0, "l'état est tout frais");

    /* Rien n'est dû avant l'intervalle de relecture. */
    fake_now = SWEETDAWN_REFRESH_INTERVAL_MS - 1;
    int sent_before = fake_send_calls;
    sweetdawn_tick(&app);
    CHECK(fake_send_calls == sent_before,
          "aucune requête avant l'intervalle de relecture");

    /* Une relecture est lancée à l'échéance. */
    fake_now = SWEETDAWN_REFRESH_INTERVAL_MS;
    fake_push_refresh(0x01, 0x00);
    sweetdawn_tick(&app);
    CHECK(app.state.display == 0x01, "relecture périodique effectuée");
    CHECK(sweetdawn_state_age_ms(&app) == 0, "l'âge repart à zéro");
}

static void test_reconnexion_apres_perte(void)
{
    SweetDawn app;

    printf("\n[2] Reconnexion après une liaison perdue\n");
    setup(&app);

    fake_push_refresh(0x02, 0x00);
    sweetdawn_tick(&app);
    CHECK(sweetdawn_is_connected(&app), "liaison établie au départ");
    CHECK(fake_connect_calls == 1, "une seule connexion jusqu'ici");

    /* À la relecture, l'AML a disparu : l'envoi signale la perte. */
    fake_now = SWEETDAWN_REFRESH_INTERVAL_MS;
    fake_send_error = AML_ERR_DISCONNECTED;
    sweetdawn_tick(&app);

    CHECK(!sweetdawn_is_connected(&app), "la perte est détectée");
    CHECK(fake_disconnect_calls == 1, "la socket morte est fermée");
    CHECK(app.state.known && app.state.display == 0x02,
          "l'état est conservé pendant la panne");
    CHECK(sweetdawn_state_age_ms(&app) == SWEETDAWN_REFRESH_INTERVAL_MS,
          "son âge reflète qu'il n'est plus rafraîchi");

    /* Pas de nouvelle tentative avant le délai d'attente. */
    fake_send_error = 0;
    fake_now += SWEETDAWN_BACKOFF_MIN_MS - 1;
    sweetdawn_tick(&app);
    CHECK(fake_connect_calls == 1, "pas de reconnexion trop précoce");

    /* À l'échéance, la reconnexion réussit et l'état est relu. */
    fake_now += 1;
    fake_push_refresh(0x01, 0x01);
    sweetdawn_tick(&app);

    CHECK(sweetdawn_is_connected(&app), "la liaison est rétablie");
    CHECK(fake_connect_calls == 2, "une seconde connexion a eu lieu");
    CHECK(app.connect_count == 2, "le compteur de connexions est à jour");
    CHECK(app.state.display == 0x01 && app.state.fade == 0x01,
          "l'état est relu après la reconnexion");
}

static void test_attente_croissante(void)
{
    SweetDawn app;
    const int64_t expected[] = {
        1000, 2000, 4000, 8000, 16000, 30000, 30000
    };

    printf("\n[3] Attente croissante entre deux connexions échouées\n");
    setup(&app);

    fake_connect_error = AML_ERR_IO;

    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        int64_t before = fake_now;

        sweetdawn_tick(&app);

        char message[80];
        snprintf(message, sizeof(message),
                 "échec %zu : prochaine tentative dans %lld ms",
                 i + 1, (long long)expected[i]);

        CHECK(app.next_connect_ms - before == expected[i], message);

        /* Avancer exactement jusqu'à la prochaine tentative. */
        fake_now = app.next_connect_ms;
    }

    CHECK(!sweetdawn_is_connected(&app), "toujours déconnecté");

    /* L'AML revient : la connexion aboutit et le délai est remis à zéro. */
    fake_connect_error = 0;
    fake_push_refresh(0x02, 0x00);
    sweetdawn_tick(&app);

    CHECK(sweetdawn_is_connected(&app), "connecté dès que l'AML revient");
    CHECK(app.connect_backoff_ms == 0,
          "l'attente est remise à zéro après une lecture réussie");
}

static void test_trames_inattendues(void)
{
    SweetDawn app;

    printf("\n[4] Réponses qui ne sont pas celles attendues\n");
    setup(&app);

    /*
     * Une réponse de paramètres tardive (0xE2) arrive avant la vraie
     * réponse de statut (0xF0). Elle ne doit pas être prise pour elle.
     */
    const uint8_t stray[] = {0x07, 0x07, 0x07, 0x07, 0x07};
    fake_push_response(0xE2, stray, sizeof(stray));
    fake_push_refresh(0x03, 0x00);

    sweetdawn_tick(&app);

    CHECK(sweetdawn_is_connected(&app), "la liaison reste ouverte");
    CHECK(app.authorization == 0x16, "le statut est bien lu");
    CHECK(app.state.known && app.state.display == 0x03,
          "les paramètres sont ceux de la vraie réponse");
    CHECK(app.state.display != 0x07, "la trame parasite est ignorée");
}

static void test_liaison_silencieuse(void)
{
    SweetDawn app;

    printf("\n[5] Liaison devenue silencieuse\n");
    setup(&app);

    fake_push_refresh(0x02, 0x00);
    sweetdawn_tick(&app);
    CHECK(sweetdawn_is_connected(&app), "liaison établie au départ");

    /* L'AML ne répond plus : chaque relecture échoue par délai dépassé. */
    fake_now = SWEETDAWN_REFRESH_INTERVAL_MS;
    sweetdawn_tick(&app);
    CHECK(sweetdawn_is_connected(&app),
          "un premier délai dépassé ne coupe pas la liaison");

    fake_now += SWEETDAWN_REFRESH_RETRY_MS;
    sweetdawn_tick(&app);
    CHECK(sweetdawn_is_connected(&app),
          "un deuxième non plus");

    fake_now += SWEETDAWN_REFRESH_RETRY_MS;
    sweetdawn_tick(&app);
    CHECK(!sweetdawn_is_connected(&app),
          "le troisième échec consécutif abandonne la liaison");
    CHECK(fake_disconnect_calls == 1, "la socket est fermée");
    CHECK(app.state.known, "l'état précédent reste consultable");

    /* Elle sera rouverte après le délai d'attente. */
    fake_now += SWEETDAWN_BACKOFF_MIN_MS;
    fake_empty_code = AML_ERR_TIMEOUT;
    fake_push_refresh(0x01, 0x00);
    sweetdawn_tick(&app);
    CHECK(sweetdawn_is_connected(&app), "la liaison est rouverte ensuite");
    CHECK(app.state.display == 0x01, "et l'état est relu");
}

static void test_reponse_courte(void)
{
    SweetDawn app;

    printf("\n[6] Réponse de paramètres tronquée\n");
    setup(&app);

    const uint8_t status[] = {0x16};
    const uint8_t too_short[] = {0x01};   /* 1 octet au lieu de 5 */

    fake_push_response(0xF0, status, sizeof(status));
    fake_push_response(0xE2, too_short, sizeof(too_short));

    sweetdawn_tick(&app);

    CHECK(!app.state.known, "l'état n'est pas modifié par une trame trop courte");
    CHECK(sweetdawn_is_connected(&app),
          "une trame mal formée ne coupe pas la liaison");
}

int main(void)
{
    printf("=== Tests de la supervision de liaison SweetDawn ===\n");

    test_connexion_initiale();
    test_reconnexion_apres_perte();
    test_attente_croissante();
    test_trames_inattendues();
    test_liaison_silencieuse();
    test_reponse_courte();

    if (failures != 0) {
        printf("\n%d vérification(s) en échec.\n", failures);
        return 1;
    }

    printf("\nTous les tests sont passés.\n");
    return 0;
}
