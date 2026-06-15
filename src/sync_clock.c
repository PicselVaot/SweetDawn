#define _POSIX_C_SOURCE 200809L
#include "aml005.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int verbose = 0;

static void trim(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t') {
        start++;
    }
    if (start) {
        memmove(s, s + start, strlen(s + start) + 1);
    }
}

static int parse_int(const char *s, int *out) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || *s == '\0' || !end || *end != '\0') return -1;
    *out = (int)v;
    return 0;
}

static int load_config(const char *path, aml005_config_t *cfg) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        trim(key);
        trim(value);

        if (strcmp(key, "AML005_MAC") == 0) {
            snprintf(cfg->mac, sizeof(cfg->mac), "%s", value);
        } else if (strcmp(key, "AML005_CHANNEL") == 0) {
            int v;
            if (parse_int(value, &v) == 0) cfg->channel = (uint8_t)v;
        } else if (strcmp(key, "AML005_TIMEOUT_MS") == 0) {
            parse_int(value, &cfg->timeout_ms);
        }
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    const char *config_path = "/etc/aml005.conf";

    static const struct option options[] = {
        {"config", required_argument, NULL, 'c'},
        {"mac", required_argument, NULL, 'm'},
        {"channel", required_argument, NULL, 'n'},
        {0, 0, 0, 0}
    };

    aml005_config_t cfg;
    aml005_config_init(&cfg);

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "--config") == 0 || strcmp(argv[i], "-c") == 0) && i + 1 < argc) {
            config_path = argv[i + 1];
            break;
        }
    }

    load_config(config_path, &cfg);

    int opt;
    while ((opt = getopt_long(argc, argv, "c:m:n:v", options, NULL)) != -1) {
        switch (opt) {
            case 'c': config_path = optarg; break;
            case 'm': snprintf(cfg.mac, sizeof(cfg.mac), "%s", optarg); break;
            case 'n': cfg.channel = (uint8_t)atoi(optarg); break;
            case 'v':
                verbose = 1;
                break;
            default: break;
        }
    }

    if (cfg.mac[0] == '\0') {
        fprintf(stderr, "AML005 MAC is required.\n");
        return 1;
    }

    aml005_client_t *client = aml005_create(&cfg);
    if (!client) {
        fprintf(stderr, "Failed to create AML005 client.\n");
        return 1;
    } else {
        if (verbose) printf("Client created\n");
    }

    aml005_status_t rc = aml005_connect(client);
    if (rc != AML005_OK) {
        fprintf(stderr, "connect failed: %s\n", aml005_status_str(rc));
        aml005_destroy(client);
        return 1;
    } else {
        if (verbose) printf("Connected to AML005\n");
    }

    rc = aml005_handshake(client, 0x16);
    if (rc != AML005_OK) {
        fprintf(stderr, "handshake failed: %s\n", aml005_status_str(rc));
        aml005_destroy(client);
        return 1;
    } else {
        if (verbose) printf("Handshake OK\n");
    }

    rc = aml005_set_time_now(client);
    if (rc != AML005_OK) {
        fprintf(stderr, "set_time failed: %s\n", aml005_status_str(rc));
        aml005_destroy(client);
        return 1;
    } else {
        if (verbose) printf("Time synced\n");
    }
    
    if (verbose) printf("Done\n");
    aml005_destroy(client);
    return 0;
}
