#ifndef AML005_H
#define AML005_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AML005_MAX_FRAME_SIZE 512
#define AML005_HANDSHAKE_CMD 0x00
#define AML005_HANDSHAKE_RSP 0xF0
#define AML005_SET_TIME_CMD  0x01
#define AML005_SET_COLOR_CMD 0x32
#define AML005_DISABLE_LIGHT_CMD 0x35
#define AML005_ENABLE_LIGHT_CMD  0x37

#define AML005_LIGHT_TARGET_MAIN 1
#define AML005_LIGHT_TARGET_MOOD 2

typedef enum {
    AML005_OK = 0,
    AML005_ERR_PARAM = -1,
    AML005_ERR_SOCKET = -2,
    AML005_ERR_CONNECT = -3,
    AML005_ERR_TIMEOUT = -4,
    AML005_ERR_PROTOCOL = -5,
    AML005_ERR_IO = -6,
    AML005_ERR_STATE = -7,
} aml005_status_t;

typedef struct {
    uint8_t raw[AML005_MAX_FRAME_SIZE];
    size_t len;
} aml005_frame_t;

typedef struct {
    char mac[18];
    uint8_t channel;
    int timeout_ms;
} aml005_config_t;

typedef struct aml005_client aml005_client_t;

const char *aml005_status_str(aml005_status_t status);

void aml005_config_init(aml005_config_t *cfg);

aml005_client_t *aml005_create(const aml005_config_t *cfg);
void aml005_destroy(aml005_client_t *client);

aml005_status_t aml005_connect(aml005_client_t *client);
void aml005_disconnect(aml005_client_t *client);
bool aml005_is_connected(const aml005_client_t *client);
bool aml005_is_handshaked(const aml005_client_t *client);

aml005_status_t aml005_handshake(aml005_client_t *client, uint8_t handshake_param);

aml005_status_t aml005_send(aml005_client_t *client, const uint8_t *params, size_t params_len);
aml005_status_t aml005_request(
    aml005_client_t *client,
    const uint8_t *params,
    size_t params_len,
    uint8_t expected_command,
    aml005_frame_t *response,
    int timeout_ms
);

aml005_status_t aml005_enable_light(aml005_client_t *client, uint8_t target);
aml005_status_t aml005_disable_light(aml005_client_t *client, uint8_t target);
aml005_status_t aml005_set_mood_color(aml005_client_t *client, uint8_t r, uint8_t g, uint8_t b);
aml005_status_t aml005_set_time_now(aml005_client_t *client);
aml005_status_t aml005_set_time_tm(aml005_client_t *client, const struct tm *tm_local);

uint8_t aml005_expected_response(uint8_t command);
uint8_t aml005_checksum(const uint8_t *data, size_t len);
size_t aml005_wrap_params(const uint8_t *params, size_t params_len, uint8_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif
