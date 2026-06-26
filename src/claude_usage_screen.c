/*
 * Pantalla de estado personalizada para la OLED del Sofle, en vertical
 * (32x128: la pantalla nativa 128x32 se rota 90 grados por software).
 *
 * Distribución de arriba a abajo:
 *   - bicho de Claude Code (pixel art 1 bit)
 *   - "5H" + batería segmentada (se llena con el uso) + % + cuenta atrás
 *   - separador
 *   - "7D" + batería segmentada + %
 *
 * Si no llegan datos en ZMK_CLAUDE_USAGE_STALE_TIMEOUT_S, los textos
 * vuelven a "--" (las barras conservan el último valor).
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk_claude_usage/claude_usage.h>

/* Lienzo lógico tras rotar 90 grados. */
#define SCR_W 32
#define SCR_H 128

/* Bicho de Claude Code, 24x16, 1 bit (MSB-first). Generado a mano. */
#define GHOST_W 24
#define GHOST_H 16
static const uint8_t ghost_map[] = {
    0x00, 0x00, 0x00,
    0x7f, 0xff, 0xfe,
    0x7f, 0xff, 0xfe,
    0x7f, 0xff, 0xfe,
    0x7e, 0x3c, 0x7e,
    0x7e, 0x3c, 0x7e,
    0x7e, 0x3c, 0x7e,
    0x7f, 0xff, 0xfe,
    0xff, 0xff, 0xff,
    0xff, 0xff, 0xff,
    0x7f, 0xff, 0xfe,
    0x7f, 0xff, 0xfe,
    0x18, 0x66, 0x18,
    0x18, 0x66, 0x18,
    0x18, 0x66, 0x18,
    0x00, 0x00, 0x00,
};

/* Descriptor de imagen LVGL 8: 1 bit con alpha (1 = pixel blanco). */
static const lv_img_dsc_t ghost_img = {
    .header.cf = LV_IMG_CF_ALPHA_1BIT,
    .header.always_zero = 0,
    .header.w = GHOST_W,
    .header.h = GHOST_H,
    .data_size = sizeof(ghost_map),
    .data = ghost_map,
};

/* Número de segmentos de cada batería. */
#define BAR_SEGMENTS 10

static struct zmk_claude_usage_state current_state = {
    .session_pct = ZMK_CLAUDE_USAGE_PCT_UNKNOWN,
    .weekly_pct = ZMK_CLAUDE_USAGE_PCT_UNKNOWN,
    .session_reset_min = ZMK_CLAUDE_USAGE_MIN_UNKNOWN,
    .weekly_reset_min = ZMK_CLAUDE_USAGE_MIN_UNKNOWN,
};
static bool stale = true;
static struct k_spinlock state_lock;

/* Objetos de la pantalla. */
static lv_obj_t *session_segs[BAR_SEGMENTS];
static lv_obj_t *weekly_segs[BAR_SEGMENTS];
static lv_obj_t *session_pct_label;
static lv_obj_t *session_reset_label;
static lv_obj_t *weekly_pct_label;
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

/* Pinta una batería: los segmentos se llenan de abajo a arriba según el
 * uso. segs[0] es el de más abajo. */
static void apply_battery(lv_obj_t *const segs[], uint8_t pct) {
    int filled = (pct == ZMK_CLAUDE_USAGE_PCT_UNKNOWN)
                     ? 0
                     : (pct * BAR_SEGMENTS + 50) / 100; /* redondeo */
    for (int i = 0; i < BAR_SEGMENTS; i++) {
        lv_opa_t opa = (i < filled) ? LV_OPA_COVER : LV_OPA_TRANSP;
        lv_obj_set_style_bg_opa(segs[i], opa, LV_PART_MAIN);
    }
}

static void update_ui_cb(struct k_work *work) {
    if (!ui_ready) {
        return;
    }

    struct zmk_claude_usage_state state;
    bool is_stale;

    k_spinlock_key_t key = k_spin_lock(&state_lock);
    state = current_state;
    is_stale = stale;
    k_spin_unlock(&state_lock, key);

    char buf[8];

    /* Las barras conservan el último valor aunque esté stale. */
    apply_battery(session_segs, state.session_pct);
    apply_battery(weekly_segs, state.weekly_pct);

    format_pct(buf, sizeof(buf), is_stale ? ZMK_CLAUDE_USAGE_PCT_UNKNOWN : state.session_pct);
    lv_label_set_text(session_pct_label, buf);

    format_reset(buf, sizeof(buf),
                 is_stale ? ZMK_CLAUDE_USAGE_MIN_UNKNOWN : state.session_reset_min);
    lv_label_set_text(session_reset_label, buf);

    format_pct(buf, sizeof(buf), is_stale ? ZMK_CLAUDE_USAGE_PCT_UNKNOWN : state.weekly_pct);
    lv_label_set_text(weekly_pct_label, buf);
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

static lv_obj_t *make_text(lv_obj_t *parent, int y, const char *init) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, LV_PART_MAIN);
    lv_obj_set_width(lbl, SCR_W);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(lbl, 0, y);
    lv_label_set_text(lbl, init);
    return lbl;
}

/* Crea una batería segmentada: marco + BAR_SEGMENTS bloques apilados.
 * Devuelve por seg_out[] los segmentos de abajo (idx 0) a arriba. */
static void make_battery(lv_obj_t *parent, int x, int y, int w, int seg_h, int gap,
                         lv_obj_t *seg_out[]) {
    int total_h = BAR_SEGMENTS * seg_h + (BAR_SEGMENTS - 1) * gap;

    lv_obj_t *frame = lv_obj_create(parent);
    lv_obj_set_size(frame, w, total_h + 4);
    lv_obj_set_pos(frame, x, y);
    lv_obj_set_scrollbar_mode(frame, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(frame, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(frame, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(frame, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(frame, 1, LV_PART_MAIN);

    int inner_w = w - 4;
    for (int i = 0; i < BAR_SEGMENTS; i++) {
        lv_obj_t *seg = lv_obj_create(frame);
        lv_obj_set_size(seg, inner_w, seg_h);
        /* idx 0 = abajo del todo, por eso invertimos la posición vertical. */
        int sy = (BAR_SEGMENTS - 1 - i) * (seg_h + gap);
        lv_obj_set_pos(seg, 0, sy);
        lv_obj_set_scrollbar_mode(seg, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_radius(seg, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(seg, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(seg, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, LV_PART_MAIN);
        seg_out[i] = seg;
    }
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    /* Rotación 90 grados: la OLED es 128x32 nativa, la queremos 32x128. */
    lv_disp_t *disp = lv_disp_get_default();
    if (disp != NULL) {
        lv_disp_set_rotation(disp, LV_DISP_ROT_90);
    }

    /* Alto de batería: 10 segmentos de 2px + 9 huecos de 1px + 4 de marco/pad. */
    const int seg_h = 2, seg_gap = 1;
    const int bat_h = BAR_SEGMENTS * seg_h + (BAR_SEGMENTS - 1) * seg_gap + 4; /* 33 */

    int y = 0;

    /* Bicho centrado arriba. */
    lv_obj_t *ghost = lv_img_create(screen);
    lv_img_set_src(ghost, &ghost_img);
    lv_obj_set_style_img_recolor(ghost, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_img_recolor_opa(ghost, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(ghost, (SCR_W - GHOST_W) / 2, y);
    y += GHOST_H; /* 16 */

    /* --- Bloque 5H --- */
    make_text(screen, y, "5H");
    y += 8;
    make_battery(screen, 6, y, 20, seg_h, seg_gap, session_segs);
    y += bat_h;
    session_pct_label = make_text(screen, y, "--%");
    y += 8;
    session_reset_label = make_text(screen, y, "--");
    y += 9;

    /* --- Separador --- */
    lv_obj_t *sep = lv_obj_create(screen);
    lv_obj_set_size(sep, SCR_W - 8, 1);
    lv_obj_set_pos(sep, 4, y);
    lv_obj_set_scrollbar_mode(sep, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(sep, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sep, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
    y += 3;

    /* --- Bloque 7D --- */
    make_text(screen, y, "7D");
    y += 8;
    make_battery(screen, 6, y, 20, seg_h, seg_gap, weekly_segs);
    y += bat_h;
    weekly_pct_label = make_text(screen, y, "--%");

    ui_ready = true;
    return screen;
}
