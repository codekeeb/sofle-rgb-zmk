/*
 * Pantalla de estado personalizada (OLED 128x32) con el uso de Claude:
 * dos filas (sesión de 5 h y semana) con barra de progreso, porcentaje
 * y cuenta atrás hasta el reinicio. Si no llegan datos en
 * ZMK_CLAUDE_USAGE_STALE_TIMEOUT_S, los textos vuelven a "--".
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk_claude_usage/claude_usage.h>

static struct zmk_claude_usage_state current_state = {
    .session_pct = ZMK_CLAUDE_USAGE_PCT_UNKNOWN,
    .weekly_pct = ZMK_CLAUDE_USAGE_PCT_UNKNOWN,
    .session_reset_min = ZMK_CLAUDE_USAGE_MIN_UNKNOWN,
    .weekly_reset_min = ZMK_CLAUDE_USAGE_MIN_UNKNOWN,
};
static bool stale = true;
static struct k_spinlock state_lock;

static lv_obj_t *session_bar;
static lv_obj_t *weekly_bar;
static lv_obj_t *session_pct_label;
static lv_obj_t *weekly_pct_label;
static lv_obj_t *session_reset_label;
static lv_obj_t *weekly_reset_label;

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

static void apply_row(lv_obj_t *bar, lv_obj_t *pct_label, lv_obj_t *reset_label, uint8_t pct,
                      uint16_t reset_min, bool is_stale) {
    char buf[8];

    lv_bar_set_value(bar, pct == ZMK_CLAUDE_USAGE_PCT_UNKNOWN ? 0 : pct, LV_ANIM_OFF);

    format_pct(buf, sizeof(buf), is_stale ? ZMK_CLAUDE_USAGE_PCT_UNKNOWN : pct);
    lv_label_set_text(pct_label, buf);

    format_reset(buf, sizeof(buf), is_stale ? ZMK_CLAUDE_USAGE_MIN_UNKNOWN : reset_min);
    lv_label_set_text(reset_label, buf);
}

static void update_ui_cb(struct k_work *work) {
    if (session_bar == NULL) {
        return;
    }

    struct zmk_claude_usage_state state;
    bool is_stale;

    k_spinlock_key_t key = k_spin_lock(&state_lock);
    state = current_state;
    is_stale = stale;
    k_spin_unlock(&state_lock, key);

    apply_row(session_bar, session_pct_label, session_reset_label, state.session_pct,
              state.session_reset_min, is_stale);
    apply_row(weekly_bar, weekly_pct_label, weekly_reset_label, state.weekly_pct,
              state.weekly_reset_min, is_stale);
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

static void make_row(lv_obj_t *parent, const char *title, int y, lv_obj_t **bar,
                     lv_obj_t **pct_label, lv_obj_t **reset_label) {
    lv_obj_t *name = lv_label_create(parent);
    lv_obj_set_style_text_font(name, &lv_font_unscii_8, LV_PART_MAIN);
    lv_label_set_text(name, title);
    lv_obj_set_pos(name, 0, y + 2);

    *bar = lv_bar_create(parent);
    lv_obj_set_size(*bar, 42, 10);
    lv_obj_set_pos(*bar, 18, y + 1);
    lv_bar_set_range(*bar, 0, 100);
    lv_bar_set_value(*bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(*bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(*bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(*bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(*bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(*bar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(*bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(*bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(*bar, lv_color_white(), LV_PART_INDICATOR);

    *pct_label = lv_label_create(parent);
    lv_obj_set_style_text_font(*pct_label, &lv_font_unscii_8, LV_PART_MAIN);
    lv_obj_set_pos(*pct_label, 62, y + 2);
    lv_obj_set_width(*pct_label, 32);
    lv_obj_set_style_text_align(*pct_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_text(*pct_label, "--%");

    *reset_label = lv_label_create(parent);
    lv_obj_set_style_text_font(*reset_label, &lv_font_unscii_8, LV_PART_MAIN);
    lv_obj_set_pos(*reset_label, 96, y + 2);
    lv_obj_set_width(*reset_label, 32);
    lv_obj_set_style_text_align(*reset_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_text(*reset_label, "--");
}

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    make_row(screen, "5H", 1, &session_bar, &session_pct_label, &session_reset_label);
    make_row(screen, "7D", 17, &weekly_bar, &weekly_pct_label, &weekly_reset_label);

    return screen;
}
