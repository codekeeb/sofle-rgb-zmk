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
/* Tres frames del bicho: A normal, B patas movidas, C parpadeo. */
static const uint8_t ghost_A[] = {
    0x00, 0x00, 0x00, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe, 0x73, 0xff, 0x9e,
    0x73, 0xff, 0x9e, 0x73, 0xff, 0x9e, 0x73, 0xff, 0x9e, 0x73, 0xff, 0x9e,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe,
    0x1b, 0x00, 0xd8, 0x1b, 0x00, 0xd8, 0x1b, 0x00, 0xd8, 0x00, 0x00, 0x00,
};
static const uint8_t ghost_B[] = {
    0x00, 0x00, 0x00, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe, 0x73, 0xff, 0x9e,
    0x73, 0xff, 0x9e, 0x73, 0xff, 0x9e, 0x73, 0xff, 0x9e, 0x73, 0xff, 0x9e,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe,
    0x0d, 0x81, 0xb0, 0x0d, 0x81, 0xb0, 0x0d, 0x81, 0xb0, 0x00, 0x00, 0x00,
};
static const uint8_t ghost_C[] = {
    0x00, 0x00, 0x00, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe,
    0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe,
    0x1b, 0x00, 0xd8, 0x1b, 0x00, 0xd8, 0x1b, 0x00, 0xd8, 0x00, 0x00, 0x00,
};
/* Bicho "muerto": ojos en X. Se usa quieto cuando la sesión está al 100%. */
static const uint8_t ghost_dead[] = {
    0x00, 0x00, 0x00, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe, 0x7d, 0xdd, 0xde,
    0x7e, 0xbe, 0xbe, 0x7f, 0x7f, 0x7e, 0x7e, 0xbe, 0xbe, 0x7d, 0xdd, 0xde,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xfe, 0x7f, 0xff, 0xfe,
    0x1b, 0x00, 0xd8, 0x1b, 0x00, 0xd8, 0x1b, 0x00, 0xd8, 0x00, 0x00, 0x00,
};

#define BAR_SEGMENTS 10

/* Modo de pantalla: reposo (bicho grande) o datos (claude-usage). */
enum screen_mode { MODE_REST, MODE_DATA };
static enum screen_mode screen_mode = MODE_REST; /* por defecto: reposo */

/* Secuencia de animación. El parpadeo (C) aparece sólo una vez cada ciclo
 * largo para que no sea molesto; el resto alterna normal (A) y patas (B).
 * La flotación va sincronizada con índices de flotación suaves. */
static const uint8_t *const ghost_seq[] = {
    ghost_A, ghost_B, ghost_A, ghost_B, ghost_A, ghost_B,
    ghost_A, ghost_B, ghost_A, ghost_B, ghost_C, ghost_A,
};
static const int8_t ghost_float[] = {0, 1, 2, 1, 0, 1, 2, 1, 0, 1, 2, 1};
#define GHOST_ANIM_FRAMES (sizeof(ghost_seq) / sizeof(ghost_seq[0]))
static uint8_t ghost_frame;

/* Cuando la sesión de 5h llega al 100% (sin tokens), el bicho aparece
 * "muerto": frame ghost_dead, quieto y sin flotación. */
static bool ghost_dead_mode;

/* Bitmap y offset de flotación del frame actual (muerto = quieto). */
static const uint8_t *ghost_cur_map(void) {
    return ghost_dead_mode ? ghost_dead : ghost_seq[ghost_frame];
}
static int ghost_cur_float(void) {
    return ghost_dead_mode ? 0 : ghost_float[ghost_frame];
}

/* Estrellitas de la pantalla de reposo: posición fija (x, y base) y tamaño.
 * Suben con un desplazamiento global (star_scroll) que crece cada tick; al
 * salir por arriba reaparecen por abajo (módulo LOG_H). */
struct star { uint8_t x; uint8_t y; uint8_t size; };
static const struct star stars[] = {
    {5, 10, 1},  {26, 24, 1}, {12, 40, 2}, {28, 58, 1}, {7, 72, 1},
    {22, 88, 2}, {15, 104, 1}, {3, 118, 1}, {29, 12, 1}, {18, 66, 1},
    {9, 96, 2},  {24, 50, 1},
};
#define STAR_COUNT (sizeof(stars) / sizeof(stars[0]))
static uint16_t star_scroll;

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
static void draw_ghost(lv_obj_t *cv, const uint8_t *map, int x0, int y0) {
    lv_draw_rect_dsc_t px;
    lv_draw_rect_dsc_init(&px);
    px.bg_color = COL_FG;
    px.bg_opa = LV_OPA_COVER;
    px.radius = 0;

    int stride = (GHOST_W + 7) / 8; /* bytes por fila */
    for (int row = 0; row < GHOST_H; row++) {
        for (int col = 0; col < GHOST_W; col++) {
            int byte = map[row * stride + (col / 8)];
            int bit = (byte >> (7 - (col % 8))) & 1;
            if (bit) {
                lv_canvas_draw_rect(cv, x0 + col, y0 + row, 1, 1, &px);
            }
        }
    }
}

/* Zona lógica que ocupa el bicho (incluida la flotación). Debe coincidir con
 * dónde lo dibuja render(). */
#define GHOST_X ((LOG_W - GHOST_W) / 2)  /* 4 */
#define GHOST_Y0 4                       /* y inicial en render() */
#define GHOST_AREA_H (GHOST_H + 3)       /* alto + rango de flotación */

/* Anima SOLO el bicho sin rehacer todo el render. El canvas mostrado ya está
 * rotado, así que dibujamos el frame del bicho en un mini-buffer lógico
 * propio (32 x GHOST_AREA_H) y volcamos ese rectángulo rotado directamente a
 * la zona física del bicho con set_px. ~24x19 px, no los 4096 del render. */
static void animate_ghost(void) {
    if (!ui_ready) {
        return;
    }

    /* Mini-lienzo lógico para el bicho: 1 byte por píxel (depth1 -> color1). */
    static uint8_t gbuf[LOG_W * GHOST_AREA_H]; /* 1 = frente, 0 = fondo */
    memset(gbuf, 0, sizeof(gbuf));

    /* Pintar el frame actual (con flotación) en gbuf. */
    const uint8_t *map = ghost_cur_map();
    int stride = (GHOST_W + 7) / 8;
    int oy = ghost_cur_float();
    for (int row = 0; row < GHOST_H; row++) {
        int gy = row + oy;
        if (gy < 0 || gy >= GHOST_AREA_H) {
            continue;
        }
        for (int col = 0; col < GHOST_W; col++) {
            int bit = (map[row * stride + (col / 8)] >> (7 - (col % 8))) & 1;
            if (bit) {
                gbuf[gy * LOG_W + (GHOST_X + col)] = 1;
            }
        }
    }

    /* Volcar rotado a la zona física del bicho. Mapeo de render:
     * px = LOG_H-1-ly, py = lx, con ly = GHOST_Y0 + gy. */
    for (int gy = 0; gy < GHOST_AREA_H; gy++) {
        int px = (LOG_H - 1) - (GHOST_Y0 + gy);
        for (int lx = 0; lx < LOG_W; lx++) {
            lv_color_t c = gbuf[gy * LOG_W + lx] ? COL_FG : COL_BG;
            lv_canvas_set_px(canvas, px, lx, c);
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

/* Vuelca la franja lógica [0,LOG_W) x [0,LOG_H) del canvas a su destino
 * físico rotado 90° horario (px=LOG_H-1-ly, py=lx). El contenido se dibuja
 * primero en esas coords lógicas y luego se llama a esto. */
static void rotate_logical_to_physical(void) {
    static lv_color_t src[CANVAS_SIDE * CANVAS_SIDE];
    memcpy(src, cbuf, sizeof(src));
    for (int px = 0; px < LOG_H; px++) {
        int ly = (LOG_H - 1) - px;
        for (int py = 0; py < LOG_W; py++) {
            lv_canvas_set_px(canvas, px, py, src[ly * CANVAS_SIDE + py]);
        }
    }
}

/* Pantalla de reposo: el bicho centrado en toda la pantalla, animado. Sin
 * datos, es el protagonista. Reusa el bicho normal (24x16) centrado. */
static void render_rest(void) {
    if (!ui_ready) {
        return;
    }

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = COL_BG;
    bg.bg_opa = LV_OPA_COVER;
    bg.radius = 0;
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIDE, CANVAS_SIDE, &bg);

    /* Estrellitas subiendo (scroll infinito hacia arriba). */
    lv_draw_rect_dsc_t star_dsc;
    lv_draw_rect_dsc_init(&star_dsc);
    star_dsc.bg_color = COL_FG;
    star_dsc.bg_opa = LV_OPA_COVER;
    star_dsc.radius = 0;
    for (size_t i = 0; i < STAR_COUNT; i++) {
        int sy = ((int)stars[i].y - star_scroll) % LOG_H;
        if (sy < 0) {
            sy += LOG_H;
        }
        lv_canvas_draw_rect(canvas, stars[i].x, sy, stars[i].size, stars[i].size,
                            &star_dsc);
    }

    int gx = (LOG_W - GHOST_W) / 2;
    int gy = (LOG_H - GHOST_H) / 2 + ghost_cur_float();
    draw_ghost(canvas, ghost_cur_map(), gx, gy);

    rotate_logical_to_physical();
}

/* Dibuja todo el contenido en orientación vertical y rota el canvas 270°. */
static void render(struct zmk_claude_usage_state state, bool is_stale) {
    if (!ui_ready) {
        return;
    }

    if (screen_mode == MODE_REST) {
        render_rest();
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

    /* Bloque centrado verticalmente. Alturas: bicho 19, 5H 9, batería 34,
     * % 9, reset 9, sep 4, 7D 9 = ~93. Centramos en 128 con un margen
     * superior, y dejamos un poco más de aire arriba para la flotación. */
    int y = GHOST_Y0;

    /* Bicho centrado: frame actual + flotación (o muerto, quieto). */
    draw_ghost(canvas, ghost_cur_map(), GHOST_X, y + ghost_cur_float());
    y += GHOST_AREA_H + 2; /* zona del bicho + separación del bloque 5H */

    /* 5H */
    draw_text(canvas, 0, y, LOG_W, "5H");
    y += 9;
    draw_battery(canvas, 6, y, 20, 2, 1, state.session_pct);
    y += (BAR_SEGMENTS * 2 + (BAR_SEGMENTS - 1) * 1 + 4) + 2;
    format_pct(buf, sizeof(buf), is_stale ? ZMK_CLAUDE_USAGE_PCT_UNKNOWN : state.session_pct);
    draw_text(canvas, 0, y, LOG_W, buf);
    y += 9;
    format_reset(buf, sizeof(buf),
                 is_stale ? ZMK_CLAUDE_USAGE_MIN_UNKNOWN : state.session_reset_min);
    draw_text(canvas, 0, y, LOG_W, buf);
    y += 11;

    /* separador */
    lv_draw_rect_dsc_t line;
    lv_draw_rect_dsc_init(&line);
    line.bg_color = COL_FG;
    line.bg_opa = LV_OPA_COVER;
    lv_canvas_draw_rect(canvas, 4, y, LOG_W - 8, 1, &line);
    y += 4;

    /* 7D: "7D" y el % en dos líneas pegadas. En la franja lógica de 32px de
     * ancho sólo caben 4 caracteres de unscii_8, así que "7D 46%" (6) no
     * entra en una línea; se parte para que el % no se corte. */
    char wpct[8];
    draw_text(canvas, 0, y, LOG_W, "7D");
    y += 9;
    format_pct(wpct, sizeof(wpct), is_stale ? ZMK_CLAUDE_USAGE_PCT_UNKNOWN : state.weekly_pct);
    draw_text(canvas, 0, y, LOG_W, wpct);

    /* Rotar copiando sólo la franja útil (32x128 lógico -> 128x32 físico).
     * Recorremos el destino físico (128x32 = 4096 px) en vez del canvas
     * entero, y leemos del buffer origen con índice directo (sin llamar a
     * lv_canvas_get_px, que es caro). Mucho más ligero que recorrer 128x128.
     *
     * Inverso de la rotación 90° horaria usada antes:
     *   destino (px,py) <- origen (lx,ly) con  lx = py,  ly = (LOG_H-1)-px
     */
    static lv_color_t src[CANVAS_SIDE * CANVAS_SIDE];
    memcpy(src, cbuf, sizeof(src));

    for (int px = 0; px < LOG_H; px++) {        /* 0..127 */
        int ly = (LOG_H - 1) - px;
        for (int py = 0; py < LOG_W; py++) {    /* 0..31 */
            int lx = py;
            lv_color_t c = src[ly * CANVAS_SIDE + lx];
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

/* Animación: avanza un frame de flotación y repinta. Lo dispara un timer
 * periódico; el repintado se hace en la cola del display. */
static void anim_work_cb(struct k_work *work) {
    ghost_frame = (ghost_frame + 1) % GHOST_ANIM_FRAMES;
    if (screen_mode == MODE_REST) {
        /* Estrellas suben 1px cada 2 ticks (más lento que la flotación). */
        if (ghost_frame % 2 == 0) {
            star_scroll = (star_scroll + 1) % LOG_H;
        }
        render_rest(); /* bicho centrado + estrellas, barato */
    } else {
        animate_ghost(); /* en modo datos, redibuja sólo la zona del bicho */
    }
}
static K_WORK_DEFINE(anim_work, anim_work_cb);

static void anim_timer_cb(struct k_timer *t) {
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &anim_work);
    }
}
static K_TIMER_DEFINE(anim_timer, anim_timer_cb, NULL);

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
    /* Bicho muerto cuando la sesión de 5h llega al 100% (sin tokens). */
    ghost_dead_mode = (state.session_pct != ZMK_CLAUDE_USAGE_PCT_UNKNOWN &&
                       state.session_pct >= 100);
    k_spin_unlock(&state_lock, key);

    k_work_reschedule(&stale_work, K_SECONDS(CONFIG_ZMK_CLAUDE_USAGE_STALE_TIMEOUT_S));

    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &update_ui_work);
    }
}

void zmk_claude_usage_widget_toggle(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    screen_mode = (screen_mode == MODE_REST) ? MODE_DATA : MODE_REST;
    k_spin_unlock(&state_lock, key);

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

    /* Animación de flotación más fluida: ~4 fps. Ahora cada tick sólo
     * redibuja la zona del bicho (animate_ghost), no toda la pantalla, así
     * que sube la frecuencia sin penalizar el escaneo del teclado. */
    k_timer_start(&anim_timer, K_MSEC(250), K_MSEC(250));

    return screen;
}
