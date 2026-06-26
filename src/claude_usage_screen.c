/*
 * Pantalla de estado personalizada para la OLED del Sofle, en VERTICAL.
 *
 * Técnica (la misma que usa el shield nice_oled, probada en este hardware):
 * se dibuja todo en un lv_canvas cuadrado en orientación vertical y luego se
 * rota el canvas entero 90° con lv_canvas_transform. El display sigue siendo
 * 128x32 nativo; no se rota el driver (eso LVGL lo ignora en ZMK v0.3).
 *
 * Lienzo lógico vertical: 32 de ancho x 128 de alto. De arriba a abajo:
 *   - bicho de Claude Code (pixel art)
 *   - "5H" + batería segmentada + % + cuenta atrás
 *   - separador
 *   - "7D" + batería segmentada + %
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk_claude_usage/claude_usage.h>

/* Lienzo lógico (antes de rotar). El canvas es cuadrado del lado mayor para
 * que la rotación con pivote central quepa, igual que en nice_oled. */
#define CANVAS_SIDE 128
#define LOG_W 32  /* ancho lógico (vertical) */
#define LOG_H 128 /* alto lógico  (vertical) */

#define COL_BG lv_color_black()
#define COL_FG lv_color_white()

/* Bicho de Claude Code, 24x16, 1 bit (MSB-first). */
#define GHOST_W 24
#define GHOST_H 16
static const uint8_t ghost_map[] = {
    0x00, 0x00, 0x00, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe,
    0x7e, 0x3c, 0x7e, 0x7e, 0x3c, 0x7e, 0x7e, 0x3c, 0x7e, 0x7f, 0xff, 0xfe,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe,
    0x18, 0x66, 0x18, 0x18, 0x66, 0x18, 0x18, 0x66, 0x18, 0x00, 0x00, 0x00,
};
static const lv_img_dsc_t ghost_img = {
    .header.cf = LV_IMG_CF_ALPHA_1BIT,
    .header.always_zero = 0,
    .header.w = GHOST_W,
    .header.h = GHOST_H,
    .data_size = sizeof(ghost_map),
    .data = ghost_map,
};

#define BAR_SEGMENTS 10

static struct zmk_claude_usage_state current_state = {
    .session_pct = ZMK_CLAUDE_USAGE_PCT_UNKNOWN,
    .weekly_pct = ZMK_CLAUDE_USAGE_PCT_UNKNOWN,
    .session_reset_min = ZMK_CLAUDE_USAGE_MIN_UNKNOWN,
    .weekly_reset_min = ZMK_CLAUDE_USAGE_MIN_UNKNOWN,
};
static bool stale = true;
static struct k_spinlock state_lock;

/* Buffer del canvas (TRUE_COLOR como nice_oled, para poder rotar). */
static lv_color_t cbuf[CANVAS_SIDE * CANVAS_SIDE];
static lv_obj_t *canvas;
static bool ui_ready;

static void format_pct(char *buf, size_t size, uint8_t pct) {
    if (pct == ZMK_CLAUDE_USAGE_PCT_UNKNOWN) {
        snprintf(buf, size, "--%%");
    } else {
        snprintf(buf, size, "%u%%", pct);
    }
}

static void format_reset(char *buf, size_t size, uint16_t minutes) {
    if (minutes == ZMK_CLAUDE_USAGE_MIN_UNKNOWN) {
        snprintf(buf, size, "--");
    } else if (minutes >= 24 * 60) {
        snprintf(buf, size, "%ud%02u", minutes / (24 * 60), (minutes % (24 * 60)) / 60);
    } else if (minutes >= 60) {
        snprintf(buf, size, "%uh%02u", minutes / 60, minutes % 60);
    } else {
        snprintf(buf, size, "%um", minutes);
    }
}

/* Dibuja una batería segmentada vertical en el canvas (coords lógicas).
 * x,y = esquina superior izquierda; w = ancho; los segmentos se apilan hacia
 * abajo pero se rellenan de abajo (idx 0) a arriba según el uso. */
static void draw_battery(lv_obj_t *cv, int x, int y, int w, int seg_h, int gap, uint8_t pct) {
    lv_draw_rect_dsc_t border;
    lv_draw_rect_dsc_init(&border);
    border.bg_opa = LV_OPA_TRANSP;
    border.border_color = COL_FG;
    border.border_width = 1;
    border.radius = 0;

    int inner_h = BAR_SEGMENTS * seg_h + (BAR_SEGMENTS - 1) * gap;
    int total_h = inner_h + 4;
    lv_canvas_draw_rect(cv, x, y, w, total_h, &border);

    int filled = (pct == ZMK_CLAUDE_USAGE_PCT_UNKNOWN) ? 0 : (pct * BAR_SEGMENTS + 50) / 100;

    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.bg_color = COL_FG;
    fill.bg_opa = LV_OPA_COVER;
    fill.radius = 0;

    int inner_w = w - 4;
    for (int i = 0; i < filled; i++) {
        /* idx 0 = abajo del todo. */
        int sy = y + 2 + (BAR_SEGMENTS - 1 - i) * (seg_h + gap);
        lv_canvas_draw_rect(cv, x + 2, sy, inner_w, seg_h, &fill);
    }
}

static void draw_text(lv_obj_t *cv, int x, int y, int w, const char *txt) {
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = COL_FG;
    dsc.font = &lv_font_unscii_8;
    dsc.align = LV_TEXT_ALIGN_CENTER;
    lv_canvas_draw_text(cv, x, y, w, &dsc, txt);
}

/* Redibuja todo el contenido y rota el canvas 90°. */
static void render(struct zmk_claude_usage_state state, bool is_stale) {
    if (!ui_ready) {
        return;
    }

    /* Fondo negro. */
    lv_canvas_fill_bg(canvas, COL_BG, LV_OPA_COVER);

    char buf[8];
    int y = 0;

    /* Bicho centrado (ancho lógico 32). */
    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);
    img_dsc.recolor = COL_FG;
    img_dsc.recolor_opa = LV_OPA_COVER;
    lv_canvas_draw_img(canvas, (LOG_W - GHOST_W) / 2, y, &ghost_img, &img_dsc);
    y += GHOST_H + 1;

    /* 5H */
    draw_text(canvas, 0, y, LOG_W, "5H");
    y += 9;
    draw_battery(canvas, 6, y, 20, 2, 1, state.session_pct);
    y += (BAR_SEGMENTS * 2 + (BAR_SEGMENTS - 1) * 1 + 4) + 1;
    format_pct(buf, sizeof(buf), is_stale ? ZMK_CLAUDE_USAGE_PCT_UNKNOWN : state.session_pct);
    draw_text(canvas, 0, y, LOG_W, buf);
    y += 9;
    format_reset(buf, sizeof(buf),
                 is_stale ? ZMK_CLAUDE_USAGE_MIN_UNKNOWN : state.session_reset_min);
    draw_text(canvas, 0, y, LOG_W, buf);
    y += 9;

    /* separador */
    lv_draw_rect_dsc_t line;
    lv_draw_rect_dsc_init(&line);
    line.bg_color = COL_FG;
    line.bg_opa = LV_OPA_COVER;
    lv_canvas_draw_rect(canvas, 4, y, LOG_W - 8, 1, &line);
    y += 3;

    /* 7D */
    draw_text(canvas, 0, y, LOG_W, "7D");
    y += 9;
    draw_battery(canvas, 6, y, 20, 2, 1, state.weekly_pct);
    y += (BAR_SEGMENTS * 2 + (BAR_SEGMENTS - 1) * 1 + 4) + 1;
    format_pct(buf, sizeof(buf), is_stale ? ZMK_CLAUDE_USAGE_PCT_UNKNOWN : state.weekly_pct);
    draw_text(canvas, 0, y, LOG_W, buf);

    /* Rotar el canvas 90° (como nice_oled). Copiamos el buffer a uno temporal
     * y lo volcamos rotado sobre el canvas con pivote central. */
    static lv_color_t cbuf_tmp[CANVAS_SIDE * CANVAS_SIDE];
    memcpy(cbuf_tmp, cbuf, sizeof(cbuf_tmp));

    lv_img_dsc_t img;
    img.data = (void *)cbuf_tmp;
    img.header.cf = LV_IMG_CF_TRUE_COLOR;
    img.header.always_zero = 0;
    img.header.w = CANVAS_SIDE;
    img.header.h = CANVAS_SIDE;

    lv_canvas_fill_bg(canvas, COL_BG, LV_OPA_COVER);
    /* Mismos parámetros que rotate_canvas() de nice_oled: ángulo 900 (=90°),
     * offset_x = -1 (ajuste de la rotación), pivote en el centro del canvas. */
    lv_canvas_transform(canvas, &img, 900, LV_IMG_ZOOM_NONE, -1, 0, CANVAS_SIDE / 2,
                        CANVAS_SIDE / 2, false);
}

static void update_ui_cb(struct k_work *work) {
    struct zmk_claude_usage_state state;
    bool is_stale;

    k_spinlock_key_t key = k_spin_lock(&state_lock);
    state = current_state;
    is_stale = stale;
    k_spin_unlock(&state_lock, key);

    render(state, is_stale);
}

static K_WORK_DEFINE(update_ui_work, update_ui_cb);

static void stale_work_cb(struct k_work *work) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    stale = true;
    k_spin_unlock(&state_lock, key);

    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &update_ui_work);
    }
}

static K_WORK_DELAYABLE_DEFINE(stale_work, stale_work_cb);

void zmk_claude_usage_widget_update(struct zmk_claude_usage_state state) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    current_state = state;
    stale = false;
    k_spin_unlock(&state_lock, key);

    k_work_reschedule(&stale_work, K_SECONDS(CONFIG_ZMK_CLAUDE_USAGE_STALE_TIMEOUT_S));

    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &update_ui_work);
    }
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    /* Contenedor del tamaño físico de la pantalla (128 ancho x 32 alto),
     * igual que nice_oled: lv_obj_set_size(obj, lado_largo, lado_corto). */
    lv_obj_t *holder = lv_obj_create(screen);
    lv_obj_set_size(holder, CANVAS_SIDE, LOG_W);
    lv_obj_set_scrollbar_mode(holder, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(holder, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(holder, 0, LV_PART_MAIN);
    lv_obj_align(holder, LV_ALIGN_TOP_LEFT, 0, 0);

    canvas = lv_canvas_create(holder);
    lv_canvas_set_buffer(canvas, cbuf, CANVAS_SIDE, CANVAS_SIDE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    ui_ready = true;

    /* Primer render con el estado inicial ("--"). */
    render(current_state, true);

    return screen;
}
