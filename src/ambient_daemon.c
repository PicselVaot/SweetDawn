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
#include <unistd.h>

/* ===================== Defaults ===================== */

#define DEFAULT_CAMERA "/dev/video0"
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480
#define DEFAULT_FPS 15
#define DEFAULT_STRIDE 1
#define DEFAULT_BORDER_RATIO 0.15
#define DEFAULT_COLOR_SMOOTH 0.45
#define DEFAULT_RECONNECT_ATTEMPTS 0
#define DEFAULT_LOG_EVERY 120
#define DEFAULT_REFERENCE_PATH "/etc/aml005-reference.yuyv"

/* Python USB algo constants from main.py */
#define PY_DS_W 64
#define PY_DS_H 36
#define PY_CROP_TOP 0.05
#define PY_CROP_BOTTOM 0.90
#define PY_CROP_LEFT 0.05
#define PY_CROP_RIGHT 0.95
#define PY_EDGE_RATIO 0.15
#define PY_VALID_S_MIN 0.20
#define PY_VALID_V_MIN 0.08
#define PY_BOOST_S_MUL 2.2
#define PY_BOOST_V_FLOOR 0.12
#define PY_BOOST_V_GAMMA 1.6

typedef struct {
    aml005_config_t aml;

    char camera_path[256];
    int width;
    int height;
    int fps;

    int stride;
    double border_ratio;
    double color_smooth;
    int reconnect_attempts;
    int log_every;
} config_t;

typedef struct {
    void *start;
    size_t length;
} buffer_t;

typedef struct {
    int fd;
    buffer_t *buffers;
    unsigned int n_buffers;
    int width;
    int height;
    uint32_t pixfmt;
} v4l2_ctx_t;

typedef struct {
    double r;
    double g;
    double b;
} colorf_t;

typedef struct {
    uint8_t *data;
    size_t size;
} reference_frame_t;

static volatile sig_atomic_t g_running = 1;

/* ===================== Signals ===================== */

static void handle_signal(int signo) {
    (void)signo;
    g_running = 0;
}

/* ===================== Utils ===================== */

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
    if (!s || !out) return -1;

    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || !end || *end != '\0') return -1;

    *out = (int)v;
    return 0;
}

static int parse_double(const char *s, double *out) {
    if (!s || !out) return -1;

    char *end = NULL;
    double v = strtod(s, &end);
    if (*s == '\0' || !end || *end != '\0') return -1;

    *out = v;
    return 0;
}

static double clampd(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint8_t clamp_u8_int(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static uint8_t clamp_u8_double(double v) {
    if (v < 0.0) return 0;
    if (v > 255.0) return 255;
    return (uint8_t)lrint(v);
}

static void config_init(config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    aml005_config_init(&cfg->aml);

    snprintf(cfg->camera_path, sizeof(cfg->camera_path), "%s", DEFAULT_CAMERA);
    cfg->width = DEFAULT_WIDTH;
    cfg->height = DEFAULT_HEIGHT;
    cfg->fps = DEFAULT_FPS;
    cfg->stride = DEFAULT_STRIDE;
    cfg->border_ratio = DEFAULT_BORDER_RATIO;
    cfg->color_smooth = DEFAULT_COLOR_SMOOTH;
    cfg->reconnect_attempts = DEFAULT_RECONNECT_ATTEMPTS;
    cfg->log_every = DEFAULT_LOG_EVERY;
}

static int load_config(const char *path, config_t *cfg) {
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

/* ===================== Reference frame ===================== */

static int save_reference_frame(const char *path, const uint8_t *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror("fopen save_reference_frame");
        return -1;
    }

    if (fwrite(data, 1, size, f) != size) {
        perror("fwrite save_reference_frame");
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

static int load_reference_frame(const char *path, reference_frame_t *ref) {
    memset(ref, 0, sizeof(*ref));

    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("fopen load_reference_frame");
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }

    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return -1;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    ref->data = malloc((size_t)sz);
    if (!ref->data) {
        fclose(f);
        return -1;
    }

    ref->size = (size_t)sz;

    if (fread(ref->data, 1, ref->size, f) != ref->size) {
        perror("fread load_reference_frame");
        free(ref->data);
        ref->data = NULL;
        ref->size = 0;
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

static void free_reference_frame(reference_frame_t *ref) {
    if (!ref) return;
    free(ref->data);
    ref->data = NULL;
    ref->size = 0;
}

static bool frame_matches_reference_exact(const uint8_t *frame, size_t frame_size, const reference_frame_t *ref) {
    if (!frame || !ref || !ref->data) return false;
    if (frame_size != ref->size) return false;
    return memcmp(frame, ref->data, frame_size) == 0;
}

/* ===================== Color helpers ===================== */

static void yuv_to_rgb_pixel(uint8_t y, uint8_t u, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b) {
    int c = (int)y - 16;
    int d = (int)u - 128;
    int e = (int)v - 128;

    if (c < 0) c = 0;

    int rr = (298 * c + 409 * e + 128) >> 8;
    int gg = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int bb = (298 * c + 516 * d + 128) >> 8;

    *r = clamp_u8_int(rr);
    *g = clamp_u8_int(gg);
    *b = clamp_u8_int(bb);
}

static void rgb_to_hsv01_u8(uint8_t r8, uint8_t g8, uint8_t b8, double *h, double *s, double *v) {
    double r = r8 / 255.0;
    double g = g8 / 255.0;
    double b = b8 / 255.0;

    double cmax = fmax(r, fmax(g, b));
    double cmin = fmin(r, fmin(g, b));
    double delta = cmax - cmin;

    double hh = 0.0;
    if (delta == 0.0) {
        hh = 0.0;
    } else if (cmax == r) {
        hh = fmod((g - b) / delta, 6.0);
    } else if (cmax == g) {
        hh = ((b - r) / delta) + 2.0;
    } else {
        hh = ((r - g) / delta) + 4.0;
    }

    hh /= 6.0;
    if (hh < 0.0) hh += 1.0;

    *h = hh;
    *s = (cmax == 0.0) ? 0.0 : (delta / cmax);
    *v = cmax;
}

static void hsv01_to_rgb(double h, double s, double v, uint8_t *r8, uint8_t *g8, uint8_t *b8) {
    h = fmod(h, 1.0);
    if (h < 0.0) h += 1.0;
    s = clampd(s, 0.0, 1.0);
    v = clampd(v, 0.0, 1.0);

    int i = (int)(h * 6.0);
    double f = h * 6.0 - i;
    double p = v * (1.0 - s);
    double q = v * (1.0 - f * s);
    double t = v * (1.0 - (1.0 - f) * s);
    i %= 6;

    double r, g, b;
    switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }

    *r8 = clamp_u8_double(r * 255.0);
    *g8 = clamp_u8_double(g * 255.0);
    *b8 = clamp_u8_double(b * 255.0);
}

static colorf_t boost_image_color_like_python(uint8_t r, uint8_t g, uint8_t b) {
    double h, s, v;
    rgb_to_hsv01_u8(r, g, b, &h, &s, &v);

    s = clampd(s * PY_BOOST_S_MUL, 0.4, 1.0);

    if (s < 0.15 || v < PY_BOOST_V_FLOOR) {
        return (colorf_t){0.0, 0.0, 0.0};
    }

    v = (v - PY_BOOST_V_FLOOR) / (1.0 - PY_BOOST_V_FLOOR);
    v = clampd(v, 0.0, 1.0);
    v = pow(v, PY_BOOST_V_GAMMA);

    uint8_t rr, gg, bb;
    hsv01_to_rgb(h, s, v, &rr, &gg, &bb);

    return (colorf_t){
        rr / 255.0,
        gg / 255.0,
        bb / 255.0
    };
}

static colorf_t smooth_colorf(colorf_t current, colorf_t target, double alpha) {
    alpha = clampd(alpha, 0.0, 1.0);
    colorf_t out;
    out.r = current.r + (target.r - current.r) * alpha;
    out.g = current.g + (target.g - current.g) * alpha;
    out.b = current.b + (target.b - current.b) * alpha;
    return out;
}

/* ===================== Python-like ambient algo ===================== */

static colorf_t compute_ambient_yuyv(const uint8_t *data, int width, int height, int stride, double border_ratio) {
    (void)stride;
    (void)border_ratio;

    int top_cut = (int)(PY_DS_H * PY_CROP_TOP);
    int bottom_cut = (int)(PY_DS_H * PY_CROP_BOTTOM);
    int left_cut = (int)(PY_DS_W * PY_CROP_LEFT);
    int right_cut = (int)(PY_DS_W * PY_CROP_RIGHT);

    if (bottom_cut <= top_cut) bottom_cut = top_cut + 1;
    if (right_cut <= left_cut) right_cut = left_cut + 1;

    int cropped_h = bottom_cut - top_cut;
    int cropped_w = right_cut - left_cut;

    int edge_h = (int)(cropped_h * PY_EDGE_RATIO);
    int edge_w = (int)(cropped_w * PY_EDGE_RATIO);
    if (edge_h < 1) edge_h = 1;
    if (edge_w < 1) edge_w = 1;

    double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0, sum_w = 0.0;
    double fallback_r = 0.0, fallback_g = 0.0, fallback_b = 0.0, fallback_n = 0.0;

    for (int sy = 0; sy < PY_DS_H; ++sy) {
        int src_y = (int)lrint((double)sy * (height - 1) / (double)(PY_DS_H - 1));

        if (sy < top_cut || sy >= bottom_cut) continue;
        int cy = sy - top_cut;
        bool on_border_y = (cy < edge_h) || (cy >= cropped_h - edge_h);

        for (int sx = 0; sx < PY_DS_W; ++sx) {
            int src_x = (int)lrint((double)sx * (width - 1) / (double)(PY_DS_W - 1));

            if (sx < left_cut || sx >= right_cut) continue;
            int cx = sx - left_cut;
            bool on_border = on_border_y || (cx < edge_w) || (cx >= cropped_w - edge_w);
            if (!on_border) continue;

            int pair_x = src_x & ~1;
            int idx = (src_y * width + pair_x) * 2;

            uint8_t y0 = data[idx + 0];
            uint8_t u  = data[idx + 1];
            uint8_t y1 = data[idx + 2];
            uint8_t v  = data[idx + 3];

            uint8_t r, g, b;
            if ((src_x & 1) == 0) {
                yuv_to_rgb_pixel(y0, u, v, &r, &g, &b);
            } else {
                yuv_to_rgb_pixel(y1, u, v, &r, &g, &b);
            }

            fallback_r += r;
            fallback_g += g;
            fallback_b += b;
            fallback_n += 1.0;

            double h, s, val;
            rgb_to_hsv01_u8(r, g, b, &h, &s, &val);

            if (!(s > PY_VALID_S_MIN && val > PY_VALID_V_MIN)) {
                continue;
            }

            double w = pow(s, 2.2) * pow(val, 1.2);
            sum_r += r * w;
            sum_g += g * w;
            sum_b += b * w;
            sum_w += w;
        }
    }

    uint8_t out_r, out_g, out_b;

    if (sum_w > 0.0) {
        out_r = clamp_u8_double(sum_r / sum_w);
        out_g = clamp_u8_double(sum_g / sum_w);
        out_b = clamp_u8_double(sum_b / sum_w);
    } else if (fallback_n > 0.0) {
        out_r = clamp_u8_double(fallback_r / fallback_n);
        out_g = clamp_u8_double(fallback_g / fallback_n);
        out_b = clamp_u8_double(fallback_b / fallback_n);
    } else {
        return (colorf_t){0.0, 0.0, 0.0};
    }

    return boost_image_color_like_python(out_r, out_g, out_b);
}

/* ===================== V4L2 ===================== */

static int xioctl(int fd, unsigned long request, void *arg) {
    int rc;
    do {
        rc = ioctl(fd, request, arg);
    } while (rc == -1 && errno == EINTR);
    return rc;
}

static void v4l2_close_camera(v4l2_ctx_t *ctx) {
    if (!ctx) return;

    if (ctx->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);

        if (ctx->buffers) {
            for (unsigned int i = 0; i < ctx->n_buffers; ++i) {
                if (ctx->buffers[i].start && ctx->buffers[i].length) {
                    munmap(ctx->buffers[i].start, ctx->buffers[i].length);
                }
            }
            free(ctx->buffers);
        }

        close(ctx->fd);
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
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
    memset(&cap, 0, sizeof(cap));
    if (xioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) != 0) {
        perror("VIDIOC_QUERYCAP");
        v4l2_close_camera(ctx);
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Camera does not support V4L2 capture + streaming.\n");
        v4l2_close_camera(ctx);
        return -1;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = (uint32_t)cfg->width;
    fmt.fmt.pix.height = (uint32_t)cfg->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(ctx->fd, VIDIOC_S_FMT, &fmt) != 0) {
        perror("VIDIOC_S_FMT");
        v4l2_close_camera(ctx);
        return -1;
    }

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        fprintf(stderr,
                "Camera refused YUYV. Current format is %.4s. This implementation expects YUYV.\n",
                (char *)&fmt.fmt.pix.pixelformat);
        v4l2_close_camera(ctx);
        return -1;
    }

    if (cfg->fps > 0) {
        struct v4l2_streamparm parm;
        memset(&parm, 0, sizeof(parm));
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = (uint32_t)cfg->fps;
        xioctl(ctx->fd, VIDIOC_S_PARM, &parm);
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) != 0) {
        perror("VIDIOC_REQBUFS");
        v4l2_close_camera(ctx);
        return -1;
    }

    if (req.count < 2) {
        fprintf(stderr, "Insufficient buffer memory.\n");
        v4l2_close_camera(ctx);
        return -1;
    }

    ctx->buffers = calloc(req.count, sizeof(buffer_t));
    if (!ctx->buffers) {
        perror("calloc");
        v4l2_close_camera(ctx);
        return -1;
    }

    ctx->n_buffers = req.count;
    for (unsigned int i = 0; i < ctx->n_buffers; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) != 0) {
            perror("VIDIOC_QUERYBUF");
            v4l2_close_camera(ctx);
            return -1;
        }

        ctx->buffers[i].length = buf.length;
        ctx->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, buf.m.offset);
        if (ctx->buffers[i].start == MAP_FAILED) {
            perror("mmap");
            v4l2_close_camera(ctx);
            return -1;
        }
    }

    for (unsigned int i = 0; i < ctx->n_buffers; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) != 0) {
            perror("VIDIOC_QBUF");
            v4l2_close_camera(ctx);
            return -1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) != 0) {
        perror("VIDIOC_STREAMON");
        v4l2_close_camera(ctx);
        return -1;
    }

    ctx->width = (int)fmt.fmt.pix.width;
    ctx->height = (int)fmt.fmt.pix.height;
    ctx->pixfmt = fmt.fmt.pix.pixelformat;

    fprintf(stderr, "Camera opened: %s (%dx%d YUYV)\n", cfg->camera_path, ctx->width, ctx->height);
    return 0;
}

static int v4l2_wait_and_dequeue(v4l2_ctx_t *ctx, struct v4l2_buffer *buf, int timeout_ms) {
    struct pollfd pfd = {
        .fd = ctx->fd,
        .events = POLLIN,
        .revents = 0
    };

    int rc;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);

    if (rc <= 0) {
        return -1;
    }

    memset(buf, 0, sizeof(*buf));
    buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf->memory = V4L2_MEMORY_MMAP;

    if (xioctl(ctx->fd, VIDIOC_DQBUF, buf) != 0) {
        if (errno == EAGAIN) return -1;
        perror("VIDIOC_DQBUF");
        return -1;
    }

    return 0;
}

static int v4l2_requeue(v4l2_ctx_t *ctx, struct v4l2_buffer *buf) {
    if (xioctl(ctx->fd, VIDIOC_QBUF, buf) != 0) {
        perror("VIDIOC_QBUF");
        return -1;
    }
    return 0;
}

/* ===================== AML005 ===================== */

static int aml_connect_ready(aml005_client_t **client_out, const config_t *cfg) {
    aml005_client_t *client = aml005_create(&cfg->aml);
    if (!client) {
        fprintf(stderr, "Failed to create AML005 client.\n");
        return -1;
    }

    aml005_status_t rc = aml005_connect(client);
    if (rc != AML005_OK) {
        fprintf(stderr, "connect failed: %s\n", aml005_status_str(rc));
        aml005_destroy(client);
        return -1;
    }

    rc = aml005_handshake(client, 0x16);
    if (rc != AML005_OK) {
        fprintf(stderr, "handshake failed: %s\n", aml005_status_str(rc));
        aml005_destroy(client);
        return -1;
    }

    rc = aml005_enable_light(client, AML005_LIGHT_TARGET_MOOD);
    if (rc != AML005_OK) {
        fprintf(stderr, "enable light failed: %s\n", aml005_status_str(rc));
        aml005_destroy(client);
        return -1;
    }

    *client_out = client;
    return 0;
}

static void aml_disconnect_release(aml005_client_t **client_io) {
    if (!client_io || !*client_io) return;

    (void)aml005_set_mood_color(*client_io, 0, 0, 0);
    (void)aml005_disable_light(*client_io, AML005_LIGHT_TARGET_MOOD);
    aml005_destroy(*client_io);
    *client_io = NULL;
}

/* ===================== Main ===================== */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --config /etc/aml005.conf [options]\n"
        "  --config PATH              Config path\n"
        "  --camera /dev/videoX       Camera path\n"
        "  --width N                  Capture width\n"
        "  --height N                 Capture height\n"
        "  --fps N                    Capture fps\n"
        "  --mac XX:XX:XX:XX:XX:XX    AML005 MAC\n"
        "  --channel N                RFCOMM channel\n"
        "  --reference PATH           Load exact-match reference frame\n"
        "  --capture-reference PATH   Capture current frame as reference and exit\n",
        prog);
}

int main(int argc, char **argv) {
    const char *config_path = "/etc/aml005.conf";
    const char *reference_path = DEFAULT_REFERENCE_PATH;
    bool capture_reference = false;

    static const struct option options[] = {
        {"config", required_argument, NULL, 'c'},
        {"camera", required_argument, NULL, 'd'},
        {"width", required_argument, NULL, 'w'},
        {"height", required_argument, NULL, 'h'},
        {"fps", required_argument, NULL, 'f'},
        {"mac", required_argument, NULL, 'm'},
        {"channel", required_argument, NULL, 'n'},
        {"reference", required_argument, NULL, 'r'},
        {"capture-reference", required_argument, NULL, 'R'},
        {0, 0, 0, 0}
    };

    config_t cfg;
    config_init(&cfg);

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "--config") == 0 || strcmp(argv[i], "-c") == 0) && i + 1 < argc) {
            config_path = argv[i + 1];
            break;
        }
    }

    load_config(config_path, &cfg);

    int opt;
    while ((opt = getopt_long(argc, argv, "c:d:w:h:f:m:n:r:R:", options, NULL)) != -1) {
        switch (opt) {
            case 'c':
                config_path = optarg;
                break;
            case 'd':
                snprintf(cfg.camera_path, sizeof(cfg.camera_path), "%s", optarg);
                break;
            case 'w':
                cfg.width = atoi(optarg);
                break;
            case 'h':
                cfg.height = atoi(optarg);
                break;
            case 'f':
                cfg.fps = atoi(optarg);
                break;
            case 'm':
                snprintf(cfg.aml.mac, sizeof(cfg.aml.mac), "%s", optarg);
                break;
            case 'n':
                cfg.aml.channel = (uint8_t)atoi(optarg);
                break;
            case 'r':
                reference_path = optarg;
                break;
            case 'R':
                reference_path = optarg;
                capture_reference = true;
                break;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (cfg.aml.mac[0] == '\0' && !capture_reference) {
        fprintf(stderr, "AML005 MAC is required. Set AML005_MAC in %s or use --mac.\n", config_path);
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    v4l2_ctx_t cam;
    if (v4l2_open_camera(&cam, &cfg) != 0) {
        return 1;
    }

    reference_frame_t reference = {0};

    if (!capture_reference) {
        if (load_reference_frame(reference_path, &reference) != 0) {
            fprintf(stderr, "Failed to load reference frame: %s\n", reference_path);
            v4l2_close_camera(&cam);
            return 1;
        }
    }

    struct v4l2_buffer first_buf;
    if (v4l2_wait_and_dequeue(&cam, &first_buf, 2000) != 0) {
        fprintf(stderr, "Failed to wait first frame.\n");
        free_reference_frame(&reference);
        v4l2_close_camera(&cam);
        return 1;
    }

    if (capture_reference) {
        if (save_reference_frame(reference_path,
                                 (const uint8_t *)cam.buffers[first_buf.index].start,
                                 first_buf.bytesused) != 0) {
            fprintf(stderr, "Failed to save reference frame: %s\n", reference_path);
            v4l2_requeue(&cam, &first_buf);
            v4l2_close_camera(&cam);
            return 1;
        }

        fprintf(stderr, "Reference frame saved to: %s\n", reference_path);
        v4l2_requeue(&cam, &first_buf);
        v4l2_close_camera(&cam);
        return 0;
    }

    if (v4l2_requeue(&cam, &first_buf) != 0) {
        free_reference_frame(&reference);
        v4l2_close_camera(&cam);
        return 1;
    }

    aml005_client_t *client = NULL;
    colorf_t current = {0.0, 0.0, 0.0};
    int frame_count = 0;
    bool last_is_reference = false;

    while (g_running) {
        struct v4l2_buffer buf;
        if (v4l2_wait_and_dequeue(&cam, &buf, 2000) != 0) {
            fprintf(stderr, "Camera read failed, exiting.\n");
            break;
        }

        const uint8_t *raw = (const uint8_t *)cam.buffers[buf.index].start;
        size_t raw_size = buf.bytesused;

        bool is_reference = frame_matches_reference_exact(raw, raw_size, &reference);

        if (is_reference) {
            if (!last_is_reference) {
                fprintf(stderr, "Reference frame detected -> Bluetooth DISCONNECT\n");
            }

            if (client != NULL) {
                aml_disconnect_release(&client);
            }
        } else {
            if (last_is_reference) {
                fprintf(stderr, "Non-reference frame detected -> Bluetooth CONNECT\n");
            }

            if (client == NULL) {
                if (aml_connect_ready(&client, &cfg) != 0) {
                    v4l2_requeue(&cam, &buf);
                    break;
                }
            }

            colorf_t sampled = compute_ambient_yuyv(raw, cam.width, cam.height, cfg.stride, cfg.border_ratio);
            current = smooth_colorf(current, sampled, cfg.color_smooth);

            uint8_t r = clamp_u8_double(current.r * 255.0);
            uint8_t g = clamp_u8_double(current.g * 255.0);
            uint8_t b = clamp_u8_double(current.b * 255.0);

            aml005_status_t rc = aml005_set_mood_color(client, r, g, b);
            if (rc != AML005_OK) {
                fprintf(stderr, "set_mood_color failed: %s\n", aml005_status_str(rc));
                aml_disconnect_release(&client);
                v4l2_requeue(&cam, &buf);
                break;
            }

            frame_count++;
            if (cfg.log_every > 0 && (frame_count % cfg.log_every) == 0) {
                fprintf(stderr, "ambient rgb=(%u,%u,%u)\n", r, g, b);
            }
        }

        last_is_reference = is_reference;

        if (v4l2_requeue(&cam, &buf) != 0) {
            break;
        }
    }

    aml_disconnect_release(&client);
    free_reference_frame(&reference);
    v4l2_close_camera(&cam);
    return 0;
}
