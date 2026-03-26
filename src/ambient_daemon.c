#define _POSIX_C_SOURCE 200809L
#include "aml005.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CAMERA "/dev/video0"
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480
#define DEFAULT_FPS 15
#define DEFAULT_STRIDE 4
#define DEFAULT_BORDER_RATIO 0.15
#define DEFAULT_COLOR_SMOOTH 0.45
#define DEFAULT_ATTEMPTS 0
#define DEFAULT_LOG_EVERY 120

typedef struct {
    void *start;
    size_t length;
} buffer_t;

typedef struct {
    char camera_path[256];
    int width;
    int height;
    int fps;
    int stride;
    double border_ratio;
    double color_smooth;
    int reconnect_attempts;
    int log_every;
    aml005_config_t aml;
} config_t;

typedef struct {
    int fd;
    buffer_t *buffers;
    uint32_t n_buffers;
    int width;
    int height;
    uint32_t pixfmt;
} v4l2_ctx_t;

typedef struct {
    double r;
    double g;
    double b;
} colorf_t;

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static int xioctl(int fd, unsigned long request, void *arg) {
    int rc;
    do {
        rc = ioctl(fd, request, arg);
    } while (rc == -1 && errno == EINTR);
    return rc;
}

static void config_init(config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->camera_path, sizeof(cfg->camera_path), "%s", DEFAULT_CAMERA);
    cfg->width = DEFAULT_WIDTH;
    cfg->height = DEFAULT_HEIGHT;
    cfg->fps = DEFAULT_FPS;
    cfg->stride = DEFAULT_STRIDE;
    cfg->border_ratio = DEFAULT_BORDER_RATIO;
    cfg->color_smooth = DEFAULT_COLOR_SMOOTH;
    cfg->reconnect_attempts = DEFAULT_ATTEMPTS;
    cfg->log_every = DEFAULT_LOG_EVERY;
    aml005_config_init(&cfg->aml);
}

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

static bool parse_int(const char *s, int *out) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || *s == '\0' || !end || *end != '\0') return false;
    *out = (int)v;
    return true;
}

static bool parse_double(const char *s, double *out) {
    char *end = NULL;
    double v = strtod(s, &end);
    if (!s || *s == '\0' || !end || *end != '\0') return false;
    *out = v;
    return true;
}

static int load_config_file(const char *path, config_t *cfg) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }

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
            snprintf(cfg->aml.mac, sizeof(cfg->aml.mac), "%s", value);
        } else if (strcmp(key, "AML005_CHANNEL") == 0) {
            parse_int(value, (int *)&cfg->aml.channel);
        } else if (strcmp(key, "AML005_TIMEOUT_MS") == 0) {
            parse_int(value, &cfg->aml.timeout_ms);
        } else if (strcmp(key, "CAMERA") == 0) {
            snprintf(cfg->camera_path, sizeof(cfg->camera_path), "%s", value);
        } else if (strcmp(key, "WIDTH") == 0) {
            parse_int(value, &cfg->width);
        } else if (strcmp(key, "HEIGHT") == 0) {
            parse_int(value, &cfg->height);
        } else if (strcmp(key, "FPS") == 0) {
            parse_int(value, &cfg->fps);
        } else if (strcmp(key, "STRIDE") == 0) {
            parse_int(value, &cfg->stride);
        } else if (strcmp(key, "BORDER_RATIO") == 0) {
            parse_double(value, &cfg->border_ratio);
        } else if (strcmp(key, "COLOR_SMOOTH") == 0) {
            parse_double(value, &cfg->color_smooth);
        } else if (strcmp(key, "RECONNECT_ATTEMPTS") == 0) {
            parse_int(value, &cfg->reconnect_attempts);
        } else if (strcmp(key, "LOG_EVERY") == 0) {
            parse_int(value, &cfg->log_every);
        }
    }

    fclose(f);
    return 0;
}

static uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b) {
    int c = (int)y - 16;
    int d = (int)u - 128;
    int e = (int)v - 128;
    if (c < 0) c = 0;

    int rr = (298 * c + 409 * e + 128) >> 8;
    int gg = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int bb = (298 * c + 516 * d + 128) >> 8;

    *r = clamp_u8(rr);
    *g = clamp_u8(gg);
    *b = clamp_u8(bb);
}

static colorf_t boost_color(colorf_t c) {
    double maxv = fmax(c.r, fmax(c.g, c.b));
    double minv = fmin(c.r, fmin(c.g, c.b));
    double v = maxv / 255.0;
    if (v < 0.12) {
        colorf_t off = {0, 0, 0};
        return off;
    }
    double s = (maxv <= 0.0) ? 0.0 : (maxv - minv) / maxv;
    if (s < 0.15) {
        colorf_t off = {0, 0, 0};
        return off;
    }

    s *= 2.2;
    if (s > 1.0) s = 1.0;

    v = (v - 0.12) / (1.0 - 0.12);
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    v = pow(v, 1.6);

    double scale = (maxv <= 0.0) ? 0.0 : (v * 255.0 / maxv);
    colorf_t out = {
        .r = c.r * scale,
        .g = c.g * scale,
        .b = c.b * scale,
    };
    return out;
}

static colorf_t smooth_color(colorf_t current, colorf_t target, double alpha) {
    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;
    colorf_t out = {
        .r = current.r + (target.r - current.r) * alpha,
        .g = current.g + (target.g - current.g) * alpha,
        .b = current.b + (target.b - current.b) * alpha,
    };
    return out;
}

static colorf_t compute_ambient_yuyv(const uint8_t *data, int width, int height, int stride, double border_ratio) {
    int border_y = (int)(height * border_ratio);
    int border_x = (int)(width * border_ratio);
    if (border_y < 1) border_y = 1;
    if (border_x < 1) border_x = 1;

    double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0, sum_w = 0.0;

    for (int y = 0; y < height; y += stride) {
        bool on_border_y = (y < border_y) || (y >= height - border_y);
        for (int x = 0; x < width; x += 2 * stride) {
            bool on_border = on_border_y || (x < border_x) || (x >= width - border_x);
            if (!on_border) continue;

            int idx = (y * width + x) * 2;
            uint8_t y0 = data[idx + 0];
            uint8_t u  = data[idx + 1];
            uint8_t y1 = data[idx + 2];
            uint8_t v  = data[idx + 3];

            uint8_t r, g, b;
            yuv_to_rgb(y0, u, v, &r, &g, &b);
            double maxv = fmax(r, fmax(g, b)) / 255.0;
            double minv = fmin(r, fmin(g, b)) / 255.0;
            double sat = (maxv <= 0.0) ? 0.0 : (maxv - minv) / maxv;
            if (sat > 0.20 && maxv > 0.08) {
                double w = pow(sat, 2.2) * pow(maxv, 1.2);
                sum_r += r * w;
                sum_g += g * w;
                sum_b += b * w;
                sum_w += w;
            }

            yuv_to_rgb(y1, u, v, &r, &g, &b);
            maxv = fmax(r, fmax(g, b)) / 255.0;
            minv = fmin(r, fmin(g, b)) / 255.0;
            sat = (maxv <= 0.0) ? 0.0 : (maxv - minv) / maxv;
            if (sat > 0.20 && maxv > 0.08) {
                double w = pow(sat, 2.2) * pow(maxv, 1.2);
                sum_r += r * w;
                sum_g += g * w;
                sum_b += b * w;
                sum_w += w;
            }
        }
    }

    colorf_t out;
    if (sum_w > 0.0) {
        out.r = sum_r / sum_w;
        out.g = sum_g / sum_w;
        out.b = sum_b / sum_w;
    } else {
        out.r = out.g = out.b = 0.0;
    }
    return boost_color(out);
}

static int v4l2_open_camera(v4l2_ctx_t *ctx, const config_t *cfg) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;

    ctx->fd = open(cfg->camera_path, O_RDWR | O_NONBLOCK, 0);
    if (ctx->fd < 0) {
        perror("open camera");
        return -1;
    }

    struct v4l2_capability cap;
    if (xioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) == -1) {
        perror("VIDIOC_QUERYCAP");
        close(ctx->fd);
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Camera does not support V4L2 capture + streaming.\n");
        close(ctx->fd);
        return -1;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = cfg->width;
    fmt.fmt.pix.height = cfg->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (xioctl(ctx->fd, VIDIOC_S_FMT, &fmt) == -1) {
        perror("VIDIOC_S_FMT");
        close(ctx->fd);
        return -1;
    }

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        fprintf(stderr, "Camera refused YUYV. Current format is %.4s. This implementation expects YUYV.\n",
                (char *)&fmt.fmt.pix.pixelformat);
        close(ctx->fd);
        return -1;
    }

    ctx->width = fmt.fmt.pix.width;
    ctx->height = fmt.fmt.pix.height;
    ctx->pixfmt = fmt.fmt.pix.pixelformat;

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = (cfg->fps > 0) ? cfg->fps : DEFAULT_FPS;
    xioctl(ctx->fd, VIDIOC_S_PARM, &parm);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) == -1 || req.count < 2) {
        perror("VIDIOC_REQBUFS");
        close(ctx->fd);
        return -1;
    }

    ctx->buffers = calloc(req.count, sizeof(buffer_t));
    ctx->n_buffers = req.count;
    if (!ctx->buffers) {
        close(ctx->fd);
        return -1;
    }

    for (uint32_t i = 0; i < ctx->n_buffers; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) == -1) {
            perror("VIDIOC_QUERYBUF");
            return -1;
        }

        ctx->buffers[i].length = buf.length;
        ctx->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, buf.m.offset);
        if (ctx->buffers[i].start == MAP_FAILED) {
            perror("mmap");
            return -1;
        }
    }

    for (uint32_t i = 0; i < ctx->n_buffers; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) == -1) {
            perror("VIDIOC_QBUF");
            return -1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) == -1) {
        perror("VIDIOC_STREAMON");
        return -1;
    }

    fprintf(stderr, "Camera opened: %s (%dx%d YUYV)\n", cfg->camera_path, ctx->width, ctx->height);
    return 0;
}

static void v4l2_close_camera(v4l2_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    }
    if (ctx->buffers) {
        for (uint32_t i = 0; i < ctx->n_buffers; ++i) {
            if (ctx->buffers[i].start && ctx->buffers[i].start != MAP_FAILED) {
                munmap(ctx->buffers[i].start, ctx->buffers[i].length);
            }
        }
        free(ctx->buffers);
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
}

static int v4l2_read_color(v4l2_ctx_t *ctx, const config_t *cfg, colorf_t *out) {
    struct pollfd pfd = {.fd = ctx->fd, .events = POLLIN, .revents = 0};
    int rc;
    do {
        rc = poll(&pfd, 1, 2000);
    } while (rc == -1 && errno == EINTR);
    if (rc <= 0) {
        return -1;
    }

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (xioctl(ctx->fd, VIDIOC_DQBUF, &buf) == -1) {
        if (errno == EAGAIN) return 1;
        perror("VIDIOC_DQBUF");
        return -1;
    }

    *out = compute_ambient_yuyv((const uint8_t *)ctx->buffers[buf.index].start, ctx->width, ctx->height, cfg->stride, cfg->border_ratio);

    if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) == -1) {
        perror("VIDIOC_QBUF");
        return -1;
    }
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s --config /etc/aml005.conf [options]\n"
        "  --config PATH           Config file\n"
        "  --camera /dev/videoX    Camera path\n"
        "  --width N               Capture width\n"
        "  --height N              Capture height\n"
        "  --fps N                 Target FPS\n"
        "  --mac AA:BB:CC:DD:EE:FF AML005 MAC\n"
        "  --channel N             RFCOMM channel\n",
        argv0
    );
}

int main(int argc, char **argv) {
    config_t cfg;
    config_init(&cfg);

    const char *config_path = "/etc/aml005.conf";

    static const struct option options[] = {
        {"config", required_argument, NULL, 'c'},
        {"camera", required_argument, NULL, 'd'},
        {"width", required_argument, NULL, 'w'},
        {"height", required_argument, NULL, 'h'},
        {"fps", required_argument, NULL, 'f'},
        {"mac", required_argument, NULL, 'm'},
        {"channel", required_argument, NULL, 'n'},
        {"help", no_argument, NULL, '?'},
        {0, 0, 0, 0}
    };

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "--config") == 0 || strcmp(argv[i], "-c") == 0) && i + 1 < argc) {
            config_path = argv[i + 1];
            break;
        }
    }

    load_config_file(config_path, &cfg);

    int opt;
    while ((opt = getopt_long(argc, argv, "c:d:w:h:f:m:n:?", options, NULL)) != -1) {
        switch (opt) {
            case 'c': config_path = optarg; break;
            case 'd': snprintf(cfg.camera_path, sizeof(cfg.camera_path), "%s", optarg); break;
            case 'w': cfg.width = atoi(optarg); break;
            case 'h': cfg.height = atoi(optarg); break;
            case 'f': cfg.fps = atoi(optarg); break;
            case 'm': snprintf(cfg.aml.mac, sizeof(cfg.aml.mac), "%s", optarg); break;
            case 'n': cfg.aml.channel = (uint8_t)atoi(optarg); break;
            case '?':
            default: usage(argv[0]); return 1;
        }
    }

    if (cfg.aml.mac[0] == '\0') {
        fprintf(stderr, "AML005 MAC is required. Set AML005_MAC in %s or use --mac.\n", config_path);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    aml005_client_t *aml = aml005_create(&cfg.aml);
    if (!aml) {
        fprintf(stderr, "Unable to create AML005 client.\n");
        return 1;
    }

    aml005_status_t rc = aml005_connect(aml);
    if (rc != AML005_OK) {
        fprintf(stderr, "AML005 connect failed: %s\n", aml005_status_str(rc));
        aml005_destroy(aml);
        return 1;
    }

    rc = aml005_handshake(aml, 0x16);
    if (rc != AML005_OK) {
        fprintf(stderr, "AML005 handshake failed: %s\n", aml005_status_str(rc));
        aml005_destroy(aml);
        return 1;
    }

    aml005_set_time_now(aml);
    aml005_enable_light(aml, AML005_LIGHT_TARGET_MOOD);

    v4l2_ctx_t cam;
    if (v4l2_open_camera(&cam, &cfg) != 0) {
        aml005_disable_light(aml, AML005_LIGHT_TARGET_MOOD);
        aml005_destroy(aml);
        return 1;
    }

    colorf_t current = {0, 0, 0};
    uint8_t last_r = 255, last_g = 255, last_b = 255;
    unsigned long frames = 0;

    while (g_running) {
        colorf_t target;
        int read_rc = v4l2_read_color(&cam, &cfg, &target);
        if (read_rc < 0) {
            fprintf(stderr, "Camera read failed, exiting.\n");
            break;
        }
        if (read_rc > 0) {
            continue;
        }

        current = smooth_color(current, target, cfg.color_smooth);

        uint8_t r = clamp_u8((int)lround(current.r));
        uint8_t g = clamp_u8((int)lround(current.g));
        uint8_t b = clamp_u8((int)lround(current.b));

        if (r != last_r || g != last_g || b != last_b) {
            rc = aml005_set_mood_color(aml, r, g, b);
            if (rc != AML005_OK) {
                fprintf(stderr, "AML005 set color failed: %s\n", aml005_status_str(rc));
                break;
            }
            last_r = r;
            last_g = g;
            last_b = b;
        }

        frames++;
        if (cfg.log_every > 0 && (frames % (unsigned long)cfg.log_every) == 0) {
            fprintf(stderr, "ambient rgb=(%u,%u,%u)\n", r, g, b);
        }
    }

    aml005_set_mood_color(aml, 0, 0, 0);
    aml005_disable_light(aml, AML005_LIGHT_TARGET_MOOD);
    v4l2_close_camera(&cam);
    aml005_destroy(aml);
    return 0;
}
