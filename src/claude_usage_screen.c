/*
 * Pantalla de estado personalizada para la OLED del Sofle.
 *
 * La OLED es 128x32 nativa (horizontal). En lugar de rotar el display por
 * software (LVGL lo ignora en ZMK v0.3 porque la rotación llega tarde y el
 * driver SSD1306 no la reconfigura), rotamos NOSOTROS cada elemento 90°:
 * el contenido se diseña "a lo alto" y cada objeto se gira para que, con la
 * pantalla montada en vertical, se lea correctamente de arriba a abajo.
 *
 * Distribución lógica (vertical, 32 ancho x 128 alto):
 *   - bicho de Claude Code
 *   - "5H" + batería segmentada + % + cuenta atrás
 *   - separador
 *   - "7D" + batería segmentada + %
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk_claude_usage/claude_usage.h>

/* Pantalla física. */
#define PHYS_W 128
#define PHYS_H 32

/* Bicho de Claude Code, 24x16, 1 bit (MSB-first). */
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

static const lv_img_dsc_t ghost_img = {
    .header.cf = LV_IMG_CF_ALPHA_1BIT,
    .header.always_zero = 0,
    .header.w = GHOST_W,
    .header.h = GHOST_H,
    .data_size = sizeof(ghost_map),
    .data = ghost_map,
};

#define BAR_SEGMENTS 10

/*
 * Mapeo lógico->físico (rotación 90° horaria del contenido).
 * Lienzo lógico: lx en [0,32), ly en [0,128) (32 ancho, 128 alto).
 * Físico: px en [0,128), py en [0,32).
 *   px = ly
 *   py = (32 - 1) - lx
 * El elemento se coloca por su esquina; como además rotamos el objeto 90°,
 * posicionamos por el punto que tras rotar queda donde toca (ver helpers).
 */

static struct zmk_claude_usage_state current_state = {
    .session_pct = ZMK_CLAUDE_USAGE_PCT_UNKNOWN,
    .weekly_pct = ZMK_CLAUDE_USAGE_PCT_UNKNOWN,
    .session_reset_min = ZMK_CLAUDE_USAGE_MIN_UNKNOWN,
    .weekly_reset_min = ZMK_CLAUDE_USAGE_MIN_UNKNOWN,
};
static bool stale = true;
static struct k_spinlock state_lock;

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

static void apply_battery(lv_obj_t *const segs[], uint8_t pct) {
    int filled = (pct == ZMK_CLAUDE_USAGE_PCT_UNKNOWN)
                     ? 0
                     : (pct * BAR_SEGMENTS + 50) / 100;
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

/*
 * Crea una etiqueta rotada 90° horaria. (lx, ly) es la esquina superior
 * izquierda en el lienzo lógico vertical; w_log y h_log su tamaño lógico.
 * Tras rotar, posicionamos el objeto en coordenadas físicas.
 *
 * Una etiqueta de tamaño w_log x h_log, rotada 90° horaria sobre su pivote
 * (0,0), ocupa físicamente: su ancho lógico pasa a alto y viceversa. Para
 * que la esquina lógica (lx,ly) caiga en la física correcta, colocamos el
 * objeto en physical (ly, PHYS_H-1-lx-... ) y dejamos que LVGL lo dibuje
 * con el transform. Usamos pivote centrado y ajustamos por traslación.
 */
static lv_obj_t *make_label_rot(lv_obj_t *parent, int lx, int ly, int w_log,
                                const char *init) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, LV_PART_MAIN);
    lv_obj_set_width(lbl, w_log);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(lbl, init);

    /* Rotar 90° horaria sobre el origen (0,0) de la etiqueta. */
    lv_obj_set_style_transform_pivot_x(lbl, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(lbl, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_angle(lbl, 900, LV_PART_MAIN); /* 90.0° */

    /* Tras rotar 90° horaria con pivote (0,0): un punto lógico (u,v) de la
     * etiqueta cae en (-v, u) relativo al origen. Para que la esquina lógica
     * superior-izquierda quede arriba a la izquierda en físico, trasladamos.
     * La esquina superior-derecha de la etiqueta (w_log,0) va a (0,w_log).
     * Colocamos el objeto en físico: px = ly, py = (PHYS_H - 1 - lx). */
    lv_obj_set_pos(lbl, ly, (PHYS_H - 1) - lx);
    return lbl;
}

/* Batería segmentada en el lienzo lógico vertical. Cada segmento es un
 * lv_obj rectangular; los posicionamos directamente en coordenadas físicas
 * porque los rectángulos no necesitan rotar (son simétricos), solo mapear:
 *   un bloque lógico en (lx,ly) de tamaño (lw,lh) ->
 *   físico en (px=ly, py=PHYS_H-1-(lx+lw-1)) de tamaño (lh, lw). */
static void make_battery(lv_obj_t *parent, int lx, int ly, int lw, int seg_h, int gap,
                         lv_obj_t *seg_out[]) {
    /* Marco: lógico lw x (10*seg_h + 9*gap + 4). */
    int frame_lh = BAR_SEGMENTS * seg_h + (BAR_SEGMENTS - 1) * gap + 4;

    lv_obj_t *frame = lv_obj_create(parent);
    /* físico: ancho = frame_lh, alto = lw */
    lv_obj_set_size(frame, frame_lh, lw);
    lv_obj_set_pos(frame, ly, (PHYS_H - 1) - (lx + lw - 1));
    lv_obj_set_scrollbar_mode(frame, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(frame, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(frame, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(frame, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(frame, 1, LV_PART_MAIN);

    int inner_w = lw - 4; /* grosor del segmento (físico = alto del bloque) */
    for (int i = 0; i < BAR_SEGMENTS; i++) {
        lv_obj_t *seg = lv_obj_create(frame);
        /* En el marco rotado: el eje "largo" físico es horizontal. idx 0 = más
         * cerca del reset (abajo lógico = derecha física del marco). */
        lv_obj_set_size(seg, seg_h, inner_w);
        int sx = i * (seg_h + gap); /* avanza por el eje largo (físico x) */
        lv_obj_set_pos(seg, sx, 0);
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

    const int seg_h = 2, seg_gap = 1;
    const int bat_lh = BAR_SEGMENTS * seg_h + (BAR_SEGMENTS - 1) * seg_gap + 4; /* 33 */

    int ly = 0; /* avance vertical lógico (0 arriba .. 127 abajo) */

    /* Bicho centrado horizontalmente en el lienzo lógico (ancho 32). */
    lv_obj_t *ghost = lv_img_create(screen);
    lv_img_set_src(ghost, &ghost_img);
    lv_obj_set_style_img_recolor(ghost, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_img_recolor_opa(ghost, LV_OPA_COVER, LV_PART_MAIN);
    /* Rotar la imagen 90° horaria. */
    lv_img_set_pivot(ghost, 0, 0);
    lv_img_set_angle(ghost, 900);
    /* lógico (lx=(32-GHOST_W)/2, ly): físico px=ly, py=PHYS_H-1-lx-... */
    int gx = (PHYS_H - GHOST_W) / 2; /* centrado en los 32 lógicos */
    lv_obj_set_pos(ghost, ly + GHOST_H, (PHYS_H - 1) - gx);
    ly += GHOST_H + 1;

    /* --- 5H --- */
    make_label_rot(screen, 4, ly, 24, "5H");
    ly += 9;
    make_battery(screen, 6, ly, 20, seg_h, seg_gap, session_segs);
    ly += bat_lh + 1;
    session_pct_label = make_label_rot(screen, 2, ly, 28, "--%");
    ly += 9;
    session_reset_label = make_label_rot(screen, 2, ly, 28, "--");
    ly += 9;

    /* --- separador (línea lógica horizontal = vertical física) --- */
    lv_obj_t *sep = lv_obj_create(screen);
    lv_obj_set_size(sep, 1, PHYS_H - 8); /* físico: 1 ancho, casi todo el alto */
    lv_obj_set_pos(sep, ly, 4);
    lv_obj_set_scrollbar_mode(sep, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(sep, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sep, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
    ly += 3;

    /* --- 7D --- */
    make_label_rot(screen, 4, ly, 24, "7D");
    ly += 9;
    make_battery(screen, 6, ly, 20, seg_h, seg_gap, weekly_segs);
    ly += bat_lh + 1;
    weekly_pct_label = make_label_rot(screen, 2, ly, 28, "--%");

    ui_ready = true;
    return screen;
}
