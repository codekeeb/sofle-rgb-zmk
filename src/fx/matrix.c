/*
 * Copyright (c) 2026 CodeKeeb
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * "Digital rain": drops fall down the key columns, a bright head followed
 * by a trail that fades out. Each column runs its own drop at its own
 * speed, with a random gap before the next one.
 *
 * Columns are not hardcoded: pixels are grouped by their position across
 * the fall direction, in buckets `column-width` wide. That is what lets
 * the global rotation turn the rain sideways or upwards -- the same
 * grouping simply runs on the other axis.
 *
 * The color goes through zmk_hsl_to_rgb(), so the global hue tints it
 * (green with the hue at 0) and the global speed scales the frame rate
 * like every other effect.
 */

#define DT_DRV_COMPAT zmk_rgb_fx_matrix

#include <math.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/rgb_fx.h>

#include <zmk/rgb_fx.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MATRIX_MAX_COLUMNS 16

/* Length of the bright head, in position units (a key is ~19). */
#define MATRIX_HEAD 8.0f

struct fx_matrix_config {
    size_t *pixel_map;
    size_t pixel_map_size;
    struct zmk_color_hsl *color;
    struct zmk_color_hsl *head_color;
    uint16_t trail;
    uint16_t column_width;
};

struct fx_matrix_column {
    float head;  /* position of the head along the fall axis */
    float speed; /* units per frame */
};

struct fx_matrix_data {
    uint8_t *pixel_column; /* column of each mapped pixel */
    float *pixel_v;        /* position of each mapped pixel along the fall axis */
    struct fx_matrix_column columns[MATRIX_MAX_COLUMNS];
    uint8_t num_columns;
    float v_min;
    float v_max;
    int8_t rotation; /* rotation the geometry was built for, -1 = not built */
    uint32_t rng;
};

/* xorshift32. Not rand(): unseeded it gives both halves the very same
 * sequence, and the two sides would rain in lockstep. */
static uint32_t fx_matrix_rand(struct fx_matrix_data *data) {
    uint32_t x = data->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    data->rng = x;
    return x;
}

static float fx_matrix_randf(struct fx_matrix_data *data, float lo, float hi) {
    return lo + (hi - lo) * (float)(fx_matrix_rand(data) % 1000) / 1000.0f;
}

/* Send a new drop down a column, after a random gap. */
static void fx_matrix_respawn(struct fx_matrix_data *data, struct fx_matrix_column *col,
                              const struct fx_matrix_config *config) {
    col->head = data->v_min - fx_matrix_randf(data, 0.0f, 2.0f * config->trail);
    col->speed = fx_matrix_randf(data, 1.0f, 2.4f);
}

/* Fall direction for each quarter turn: down, left, up, right. Screen
 * coordinates, y grows downwards. */
static const int8_t fall_dx[4] = {0, -1, 0, 1};
static const int8_t fall_dy[4] = {1, 0, -1, 0};

static void fx_matrix_build(const struct device *dev, struct rgb_fx_pixel *pixels,
                            uint8_t rotation) {
    const struct fx_matrix_config *config = dev->config;
    struct fx_matrix_data *data = dev->data;

    int dx = fall_dx[rotation];
    int dy = fall_dy[rotation];

    float u_min = INFINITY;
    data->v_min = INFINITY;
    data->v_max = -INFINITY;

    for (size_t i = 0; i < config->pixel_map_size; ++i) {
        const struct rgb_fx_pixel *px = &pixels[config->pixel_map[i]];
        float v = px->position_x * dx + px->position_y * dy;
        float u = px->position_x * -dy + px->position_y * dx;

        data->pixel_v[i] = v;
        u_min = MIN(u_min, u);
        data->v_min = MIN(data->v_min, v);
        data->v_max = MAX(data->v_max, v);
    }

    data->num_columns = 0;

    for (size_t i = 0; i < config->pixel_map_size; ++i) {
        const struct rgb_fx_pixel *px = &pixels[config->pixel_map[i]];
        float u = px->position_x * -dy + px->position_y * dx;
        int col = (int)((u - u_min) / config->column_width + 0.5f);

        col = MIN(col, MATRIX_MAX_COLUMNS - 1);
        data->pixel_column[i] = col;
        data->num_columns = MAX(data->num_columns, col + 1);
    }

    for (uint8_t c = 0; c < data->num_columns; ++c) {
        fx_matrix_respawn(data, &data->columns[c], config);
        /* Stagger the first drops so they do not all start together. */
        data->columns[c].head += fx_matrix_randf(data, 0.0f, data->v_max - data->v_min);
    }

    data->rotation = rotation;
}

static void fx_matrix_render_frame(const struct device *dev, struct rgb_fx_pixel *pixels,
                                   size_t num_pixels) {
    const struct fx_matrix_config *config = dev->config;
    struct fx_matrix_data *data = dev->data;

    uint8_t rotation = zmk_rgb_fx_rotation_get();
    if (data->rotation != rotation) {
        fx_matrix_build(dev, pixels, rotation);
    }

    /* Converted every frame so a hue change shows up straight away. */
    struct zmk_color_rgb body, head;
    zmk_hsl_to_rgb(config->color, &body);
    zmk_hsl_to_rgb(config->head_color, &head);

    float trail = (float)config->trail;

    for (size_t i = 0; i < config->pixel_map_size; ++i) {
        const struct fx_matrix_column *col = &data->columns[data->pixel_column[i]];
        float d = col->head - data->pixel_v[i];

        if (d < 0.0f || d >= trail) {
            continue; /* left black: the frame starts cleared */
        }

        struct zmk_color_rgb *out = &pixels[config->pixel_map[i]].value;

        if (d < MATRIX_HEAD) {
            *out = head;
            continue;
        }

        float fade = 1.0f - (d - MATRIX_HEAD) / (trail - MATRIX_HEAD);
        fade *= fade;
        out->r = body.r * fade;
        out->g = body.g * fade;
        out->b = body.b * fade;
    }

    for (uint8_t c = 0; c < data->num_columns; ++c) {
        struct fx_matrix_column *col = &data->columns[c];

        col->head += col->speed;
        if (col->head - trail > data->v_max) {
            fx_matrix_respawn(data, col, config);
        }
    }

    zmk_rgb_fx_request_frames(1);
}

static void fx_matrix_start(const struct device *dev) {
    struct fx_matrix_data *data = dev->data;

    /* Seeded at start: each half reaches this at a different cycle count. */
    data->rng = k_cycle_get_32() ^ (k_uptime_get_32() << 7) ^ 0x9e3779b9u;
    if (data->rng == 0) {
        data->rng = 0x12345678u;
    }
    data->rotation = -1; /* rebuilt on the first frame, with the pixels at hand */

    zmk_rgb_fx_request_frames(1);
}

static void fx_matrix_stop(const struct device *dev) {
    // Nothing to do.
}

static int fx_matrix_init(const struct device *dev) { return 0; }

static const struct rgb_fx_api fx_matrix_api = {
    .on_start = fx_matrix_start,
    .on_stop = fx_matrix_stop,
    .render_frame = fx_matrix_render_frame,
};

#define FX_MATRIX_DEVICE(idx)                                                                      \
                                                                                                   \
    static size_t fx_matrix_##idx##_pixel_map[] = DT_INST_PROP(idx, pixels);                       \
    static uint8_t fx_matrix_##idx##_pixel_column[DT_INST_PROP_LEN(idx, pixels)];                  \
    static float fx_matrix_##idx##_pixel_v[DT_INST_PROP_LEN(idx, pixels)];                         \
                                                                                                   \
    static uint32_t fx_matrix_##idx##_color = DT_INST_PROP(idx, color);                            \
    static uint32_t fx_matrix_##idx##_head_color = DT_INST_PROP(idx, head_color);                  \
                                                                                                   \
    static struct fx_matrix_config fx_matrix_##idx##_config = {                                    \
        .pixel_map = fx_matrix_##idx##_pixel_map,                                                  \
        .pixel_map_size = DT_INST_PROP_LEN(idx, pixels),                                           \
        .color = (struct zmk_color_hsl *)&fx_matrix_##idx##_color,                                 \
        .head_color = (struct zmk_color_hsl *)&fx_matrix_##idx##_head_color,                       \
        .trail = DT_INST_PROP(idx, trail),                                                         \
        .column_width = DT_INST_PROP(idx, column_width),                                           \
    };                                                                                             \
                                                                                                   \
    static struct fx_matrix_data fx_matrix_##idx##_data = {                                        \
        .pixel_column = fx_matrix_##idx##_pixel_column,                                            \
        .pixel_v = fx_matrix_##idx##_pixel_v,                                                      \
        .rotation = -1,                                                                            \
        .rng = 0x12345678u,                                                                        \
    };                                                                                             \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(idx, &fx_matrix_init, NULL, &fx_matrix_##idx##_data,                     \
                          &fx_matrix_##idx##_config, POST_KERNEL,                                  \
                          CONFIG_APPLICATION_INIT_PRIORITY, &fx_matrix_api);

DT_INST_FOREACH_STATUS_OKAY(FX_MATRIX_DEVICE);
