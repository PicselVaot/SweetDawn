/*
 * test/fake_aml.c — Faux AML005 pour tester sans Bluetooth
 *
 * Voir fake_aml.h.
 */

#include "fake_aml.h"

#include <string.h>

int fake_connect_error = 0;
int fake_send_error = 0;
int fake_empty_code = AML_ERR_TIMEOUT;

int fake_connect_calls = 0;
int fake_disconnect_calls = 0;
int fake_send_calls = 0;
uint8_t fake_last_request_code = 0;
FakeFrame fake_sent[FAKE_QUEUE_MAX];
int fake_sent_count = 0;

int64_t fake_now = 0;

/* Réponses en attente de lecture par aml_receive. */
static FakeFrame fake_queue[FAKE_QUEUE_MAX];
static int fake_queue_count = 0;
static int fake_queue_head = 0;

void fake_reset(void)
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
    fake_sent_count = 0;
}

/* Construit une trame de réponse : FE EF 0C LL AB BB IL code args chk 55 0D 0A */
void fake_push_response(uint8_t code, const uint8_t *args, size_t args_length)
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
    frame->spontaneous = false;
}

void fake_push_event(uint8_t code, const uint8_t *args, size_t args_length)
{
    fake_push_response(code, args, args_length);
    fake_queue[fake_queue_count - 1].spontaneous = true;
}

static int64_t fake_clock(void)
{
    return fake_now;
}

static AmlConfig fake_device = {
    .name = "test",
    .aml_mac = "00:00:00:00:00:01",
    .aml_chn = 1,
    .blt_mac = "00:00:00:00:00:02"
};

void fake_setup(SweetDawn *app)
{
    memset(app, 0, sizeof(*app));
    app->connection.socket_fd = -1;
    app->last_refresh_ms = -1;
    app->device = &fake_device;
    app->initialized = true;
    app->clock_ms = fake_clock;

    state_init(&app->state);
    aml_commands_init(&app->commands);

    fake_reset();
    fake_now = 0;
}

/* ------------------------------------------------------------------------- */
/* Remplaçants de aml/transport.c                                            */
/* ------------------------------------------------------------------------- */

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

    if (fake_sent_count < FAKE_QUEUE_MAX &&
        length <= sizeof(fake_sent[0].data)) {
        memcpy(fake_sent[fake_sent_count].data, data, length);
        fake_sent[fake_sent_count].length = length;
        fake_sent_count++;
    }

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

    if (fake_queue_head >= fake_queue_count)
        return fake_empty_code;

    /*
     * Lecture sans attente : seul un message spontané est déjà là ; une
     * réponse n'arrive qu'en réaction à une requête.
     */
    if (timeout_ms == 0 && !fake_queue[fake_queue_head].spontaneous)
        return AML_ERR_TIMEOUT;

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

/* ------------------------------------------------------------------------- */
/* Remplaçants de core/config.c (sweetdawn.c y fait référence)               */
/* ------------------------------------------------------------------------- */

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
