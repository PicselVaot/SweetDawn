#define _POSIX_C_SOURCE 200809L
#include "aml005.h"

#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

struct aml005_client {
    aml005_config_t cfg;
    int sock;
    bool connected;
    bool handshaked;
    pthread_mutex_t io_lock;
};

static bool aml005_valid_mac(const char *mac) {
    if (!mac) {
        return false;
    }
    if (strlen(mac) != 17) {
        return false;
    }
    for (size_t i = 0; i < 17; ++i) {
        if ((i + 1) % 3 == 0) {
            if (mac[i] != ':') {
                return false;
            }
        } else {
            char c = mac[i];
            bool hex = (c >= '0' && c <= '9') ||
                       (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F');
            if (!hex) {
                return false;
            }
        }
    }
    return true;
}

const char *aml005_status_str(aml005_status_t status) {
    switch (status) {
        case AML005_OK: return "ok";
        case AML005_ERR_PARAM: return "invalid parameter";
        case AML005_ERR_SOCKET: return "socket error";
        case AML005_ERR_CONNECT: return "connect error";
        case AML005_ERR_TIMEOUT: return "timeout";
        case AML005_ERR_PROTOCOL: return "protocol error";
        case AML005_ERR_IO: return "I/O error";
        case AML005_ERR_STATE: return "invalid state";
        default: return "unknown error";
    }
}

void aml005_config_init(aml005_config_t *cfg) {
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->channel = 1;
    cfg->timeout_ms = 2000;
}

aml005_client_t *aml005_create(const aml005_config_t *cfg) {
    if (!cfg || !aml005_valid_mac(cfg->mac) || cfg->channel == 0 || cfg->timeout_ms <= 0) {
        return NULL;
    }

    aml005_client_t *client = calloc(1, sizeof(*client));
    if (!client) {
        return NULL;
    }

    client->cfg = *cfg;
    client->sock = -1;
    pthread_mutex_init(&client->io_lock, NULL);
    return client;
}

void aml005_destroy(aml005_client_t *client) {
    if (!client) {
        return;
    }
    aml005_disconnect(client);
    pthread_mutex_destroy(&client->io_lock);
    free(client);
}

bool aml005_is_connected(const aml005_client_t *client) {
    return client && client->connected;
}

bool aml005_is_handshaked(const aml005_client_t *client) {
    return client && client->handshaked;
}

static aml005_status_t aml005_wait_readable(int fd, int timeout_ms) {
    struct pollfd pfd = {
        .fd = fd,
        .events = POLLIN,
        .revents = 0,
    };

    int rc;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);

    if (rc == 0) {
        return AML005_ERR_TIMEOUT;
    }
    if (rc < 0) {
        return AML005_ERR_IO;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        return AML005_ERR_IO;
    }
    return AML005_OK;
}

uint8_t aml005_checksum(const uint8_t *data, size_t len) {
    uint8_t x = 0;
    for (size_t i = 0; i < len; ++i) {
        x ^= data[i];
    }
    return x;
}

uint8_t aml005_expected_response(uint8_t command) {
    uint8_t hi = (command >> 4) & 0x0F;
    uint8_t lo = command & 0x0F;
    return (uint8_t)(((0x0F - hi) << 4) | lo);
}

size_t aml005_wrap_params(const uint8_t *params, size_t params_len, uint8_t *out, size_t out_cap) {
    if (!params || !out) {
        return 0;
    }

    size_t total = params_len + 11;
    if (out_cap < total || total > 0xFF) {
        return 0;
    }

    memset(out, 0, total);
    out[0] = 0xFE;
    out[1] = 0xEF;
    out[2] = 0x0A;
    out[3] = (uint8_t)(total - 4);
    out[4] = 0xAB;
    out[5] = 0xAA;
    out[6] = (uint8_t)(params_len + 2);
    memcpy(&out[7], params, params_len);
    out[total - 4] = aml005_checksum(&out[6], params_len + 1);
    out[total - 3] = 0x55;
    out[total - 2] = 0x0D;
    out[total - 1] = 0x0A;
    return total;
}

static aml005_status_t aml005_read_frame_locked(aml005_client_t *client, aml005_frame_t *frame, int timeout_ms) {
    if (!client || !frame || client->sock < 0) {
        return AML005_ERR_PARAM;
    }

    memset(frame, 0, sizeof(*frame));
    uint8_t temp[AML005_MAX_FRAME_SIZE];
    size_t used = 0;

    while (used < sizeof(temp)) {
        aml005_status_t wait_rc = aml005_wait_readable(client->sock, timeout_ms);
        if (wait_rc != AML005_OK) {
            return wait_rc;
        }

        ssize_t got = recv(client->sock, temp + used, sizeof(temp) - used, 0);
        if (got == 0) {
            client->connected = false;
            client->handshaked = false;
            return AML005_ERR_IO;
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return AML005_ERR_IO;
        }
        used += (size_t)got;

        for (size_t i = 0; i + 12 <= used; ++i) {
            if (temp[i] != 0xFE || temp[i + 1] != 0xEF) {
                continue;
            }
            if (i + 4 > used) {
                break;
            }
            size_t total = (size_t)temp[i + 3] + 4;
            if (total < 13 || i + total > used) {
                continue;
            }
            if (temp[i + total - 3] != 0x55 || temp[i + total - 2] != 0x0D || temp[i + total - 1] != 0x0A) {
                continue;
            }

            uint8_t computed = aml005_checksum(&temp[i + 6], total - 10);
            uint8_t received = temp[i + total - 4];
            if (computed != received) {
                return AML005_ERR_PROTOCOL;
            }

            memcpy(frame->raw, &temp[i], total);
            frame->len = total;
            return AML005_OK;
        }
    }

    return AML005_ERR_PROTOCOL;
}

aml005_status_t aml005_connect(aml005_client_t *client) {
    if (!client) {
        return AML005_ERR_PARAM;
    }
    if (client->connected) {
        return AML005_OK;
    }

    int sock = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (sock < 0) {
        return AML005_ERR_SOCKET;
    }

    struct sockaddr_rc addr = {0};
    addr.rc_family = AF_BLUETOOTH;
    addr.rc_channel = client->cfg.channel;
    if (str2ba(client->cfg.mac, &addr.rc_bdaddr) != 0) {
        close(sock);
        return AML005_ERR_PARAM;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return AML005_ERR_CONNECT;
    }

    client->sock = sock;
    client->connected = true;
    client->handshaked = false;
    return AML005_OK;
}

void aml005_disconnect(aml005_client_t *client) {
    if (!client) {
        return;
    }
    if (client->sock >= 0) {
        close(client->sock);
        client->sock = -1;
    }
    client->connected = false;
    client->handshaked = false;
}

aml005_status_t aml005_send(aml005_client_t *client, const uint8_t *params, size_t params_len) {
    if (!client || !params || params_len == 0) {
        return AML005_ERR_PARAM;
    }
    if (!client->connected) {
        aml005_status_t rc = aml005_connect(client);
        if (rc != AML005_OK) {
            return rc;
        }
    }

    uint8_t frame[AML005_MAX_FRAME_SIZE];
    size_t frame_len = aml005_wrap_params(params, params_len, frame, sizeof(frame));
    if (frame_len == 0) {
        return AML005_ERR_PARAM;
    }

    size_t sent = 0;
    while (sent < frame_len) {
        ssize_t rc = send(client->sock, frame + sent, frame_len - sent, 0);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            client->connected = false;
            client->handshaked = false;
            return AML005_ERR_IO;
        }
        sent += (size_t)rc;
    }

    return AML005_OK;
}

aml005_status_t aml005_request(
    aml005_client_t *client,
    const uint8_t *params,
    size_t params_len,
    uint8_t expected_command,
    aml005_frame_t *response,
    int timeout_ms
) {
    if (!client || !params || params_len == 0 || !response) {
        return AML005_ERR_PARAM;
    }
    if (timeout_ms <= 0) {
        timeout_ms = client->cfg.timeout_ms;
    }

    pthread_mutex_lock(&client->io_lock);

    aml005_status_t rc = aml005_send(client, params, params_len);
    if (rc != AML005_OK) {
        pthread_mutex_unlock(&client->io_lock);
        return rc;
    }

    for (;;) {
        rc = aml005_read_frame_locked(client, response, timeout_ms);
        if (rc != AML005_OK) {
            pthread_mutex_unlock(&client->io_lock);
            return rc;
        }
        if (response->len > 8 && response->raw[7] == expected_command) {
            pthread_mutex_unlock(&client->io_lock);
            return AML005_OK;
        }
        /* Ignore unrelated async frames and continue waiting. */
    }
}

aml005_status_t aml005_handshake(aml005_client_t *client, uint8_t handshake_param) {
    if (!client) {
        return AML005_ERR_PARAM;
    }
    const uint8_t params[] = { AML005_HANDSHAKE_CMD, handshake_param };
    aml005_frame_t rsp;
    aml005_status_t rc = aml005_request(
        client,
        params,
        sizeof(params),
        AML005_HANDSHAKE_RSP,
        &rsp,
        client->cfg.timeout_ms
    );
    if (rc == AML005_OK) {
        client->handshaked = true;
    }
    return rc;
}

aml005_status_t aml005_enable_light(aml005_client_t *client, uint8_t target) {
    const uint8_t params[] = { AML005_ENABLE_LIGHT_CMD, target };
    return aml005_send(client, params, sizeof(params));
}

aml005_status_t aml005_disable_light(aml005_client_t *client, uint8_t target) {
    const uint8_t params[] = { AML005_DISABLE_LIGHT_CMD, target };
    return aml005_send(client, params, sizeof(params));
}

aml005_status_t aml005_set_mood_color(aml005_client_t *client, uint8_t r, uint8_t g, uint8_t b) {
    const uint8_t params[] = { AML005_SET_COLOR_CMD, r, g, b };
    return aml005_send(client, params, sizeof(params));
}

aml005_status_t aml005_set_time_tm(aml005_client_t *client, const struct tm *tm_local) {
    if (!client || !tm_local) {
        return AML005_ERR_PARAM;
    }

    int year = tm_local->tm_year + 1900;
    if (year >= 2000) {
        year -= 2000;
    }

    int weekday = (tm_local->tm_wday + 1) % 7;
    const uint8_t params[] = {
        AML005_SET_TIME_CMD,
        (uint8_t)(tm_local->tm_sec & 0xFF),
        (uint8_t)(tm_local->tm_min & 0xFF),
        (uint8_t)(tm_local->tm_hour & 0xFF),
        (uint8_t)(weekday & 0xFF),
        (uint8_t)(tm_local->tm_mday & 0xFF),
        (uint8_t)((tm_local->tm_mon + 1) & 0xFF),
        (uint8_t)(year & 0xFF),
        0x02,
        0x00,
    };

    return aml005_send(client, params, sizeof(params));
}

aml005_status_t aml005_set_time_now(aml005_client_t *client) {
    time_t now = time(NULL);
    if (now == (time_t)-1) {
        return AML005_ERR_IO;
    }

    struct tm tm_local;
    if (!localtime_r(&now, &tm_local)) {
        return AML005_ERR_IO;
    }

    return aml005_set_time_tm(client, &tm_local);
}
