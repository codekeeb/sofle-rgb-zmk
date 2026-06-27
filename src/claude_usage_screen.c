/*
 * Pantalla de estado personalizada para la OLED del Sofle, en VERTICAL.
 *
 * Técnica idéntica a la del shield nice_oled (probada en este hardware: se ve
 * vertical en la mitad izquierda): se dibuja todo en un lv_canvas cuadrado en
 * orientación vertical y se rota el canvas entero con lv_canvas_transform.
 *
 * El cable USB de la mitad derecha sale por el lado que debe quedar ARRIBA
 * (donde va el fantasma), lo que corresponde a una rotación de 270°.
 *
 * Lienzo lógico vertical (32 ancho x 128 alto), de arriba a abajo:
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

/* Canvas cuadrado del lado mayor de la pantalla (128), como nice_oled. */
#define CANVAS_SIDE 128
#define LOG_W 32  /* ancho lógico vertical  */
#define LOG_H 128 /* alto  lógico vertical  */

/* La OLED del Sofle tiene "inversion-on" en su devicetree: el panel invierte
 * todo a nivel hardware. El canvas escribe los píxeles en crudo (no pasa por
 * el tema mono de ZMK), así que hay que invertir los colores aquí para
 * compensar: fondo "blanco" lógico -> negro en pantalla; frente "negro"
 * lógico -> blanco en pantalla. */
#define COL_BG lv_color_white()
#define COL_FG lv_color_black()

/* Bicho de Claude Code, 24x16, 1 bit (MSB-first). */
#define GHOST_W 24
#define GHOST_H 16
static const uint8_t ghost_map[] = {
    0x00, 0x00, 0x00, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe,
    0x7e, 0x3c, 0x7e, 0x7e, 0x3c, 0x7e, 0x7e, 0x3c, 0x7e, 0x7f, 0xff, 0xfe,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe,
    0x18, 0x66, 0x18, 0x18, 0x66, 0x18, 0x18, 0x66, 0x18, 0x00, 0x00, 0x00,
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

static void draw_battery(lv_obj_t *cv, int x, int y, int w, int seg_h, int gap, uint8_t pct) {
    lv_draw_rect_dsc_t border;
    lv_draw_rect_dsc_init(&border);
    border.bg_opa = LV_OPA_TRANSP;
    border.border_color = COL_FG;
    border.border_width = 1;
    border.radius = 0;

    int inner_h = BAR_SEGMENTS * seg_h + (BAR_SEGMENTS - 1) * gap;
    lv_canvas_draw_rect(cv, x, y, w, inner_h + 4, &border);

    int filled = (pct == ZMK_CLAUDE_USAGE_PCT_UNKNOWN) ? 0 : (pct * BAR_SEGMENTS + 50) / 100;

    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.bg_color = COL_FG;
    fill.bg_opa = LV_OPA_COVER;
    fill.radius = 0;

    int inner_w = w - 4;
    for (int i = 0; i < filled; i++) {
        int sy = y + 2 + (BAR_SEGMENTS - 1 - i) * (seg_h + gap);
        lv_canvas_draw_rect(cv, x + 2, sy, inner_w, seg_h, &fill);
    }
}

/* Dibuja el bicho leyendo su bitmap 1bpp y pintando cada píxel encendido con
 * un rect 1x1. Usamos esto en vez de lv_canvas_draw_img+recolor, que no
 * renderiza en este canvas depth-1. */
static void draw_ghost(lv_obj_t *cv, int x0, int y0) {
    lv_draw_rect_dsc_t px;
    lv_draw_rect_dsc_init(&px);
    px.bg_color = COL_FG;
    px.bg_opa = LV_OPA_COVER;
    px.radius = 0;

    int stride = (GHOST_W + 7) / 8; /* bytes por fila */
    for (int row = 0; row < GHOST_H; row++) {
        for (int col = 0; col < GHOST_W; col++) {
            int byte = ghost_map[row * stride + (col / 8)];
            int bit = (byte >> (7 - (col % 8))) & 1;
            if (bit) {
                lv_canvas_draw_rect(cv, x0 + col, y0 + row, 1, 1, &px);
            }
        }
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

/* Dibuja todo el contenido en orientación vertical y rota el canvas 270°. */
static void render(struct zmk_claude_usage_state state, bool is_stale) {
    if (!ui_ready) {
        return;
    }

    /* Fondo: rect de tamaño completo (como draw_background de nice_oled);
     * fill_bg no rellena fiable en depth-1 con este flujo. */
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = COL_BG;
    bg.bg_opa = LV_OPA_COVER;
    bg.radius = 0;
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIDE, CANVAS_SIDE, &bg);

    char buf[8];
    int y = 0;

    /* Bicho centrado en el ancho lógico (32), dibujado con rects. */
    draw_ghost(canvas, (LOG_W - GHOST_W) / 2, y);
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
    y += 4;

    /* 7D: solo texto (sin barra), para que la interfaz no sea tan alta. */
    draw_text(canvas, 0, y, LOG_W, "7D");
    y += 9;
    format_pct(buf, sizeof(buf), is_stale ? ZMK_CLAUDE_USAGE_PCT_UNKNOWN : state.weekly_pct);
    draw_text(canvas, 0, y, LOG_W, buf);

    /* Rotar 270° (antihorario) copiando píxel a píxel. Evitamos
     * lv_canvas_transform porque en color depth 1 aplica chroma-key (el verde
     * 0x00ff00 colapsa a un color mono) y hace desaparecer el contenido.
     *
     * Sólo importa la franja útil: el contenido vivo está en el rectángulo
     * lógico [0..LOG_W) x [0..LOG_H). Lo volcamos rotado a la zona física
     * 128x32 de la esquina superior izquierda.
     *
     * Rotación 270° antihoraria del punto lógico (lx,ly):
     *   px = ly
     *   py = (LOG_W-1) - lx
     */
    static lv_color_t src[CANVAS_SIDE * CANVAS_SIDE];
    memcpy(src, cbuf, sizeof(src));

    /* Limpiar el canvas a fondo antes de volcar la versión rotada. */
    lv_draw_rect_dsc_t bg2;
    lv_draw_rect_dsc_init(&bg2);
    bg2.bg_color = COL_BG;
    bg2.bg_opa = LV_OPA_COVER;
    bg2.radius = 0;
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIDE, CANVAS_SIDE, &bg2);

    for (int ly = 0; ly < LOG_H; ly++) {
        for (int lx = 0; lx < LOG_W; lx++) {
            lv_color_t c = src[ly * CANVAS_SIDE + lx];
            int px = (LOG_H - 1) - ly;
            int py = lx;
            lv_canvas_set_px(canvas, px, py, c);
        }
    }
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

    /* Estructura idéntica a nice_oled: canvas directo en un obj del tamaño
     * físico de la pantalla; el buffer es cuadrado para poder rotar. */
    canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(canvas, cbuf, CANVAS_SIDE, CANVAS_SIDE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    ui_ready = true;
    render(current_state, true);

    return screen;
}
