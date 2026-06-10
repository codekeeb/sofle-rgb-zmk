/*
 * Behavior que actúa como canal de datos: la central lo invoca sobre el
 * periférico vía split con el estado de uso empaquetado en param1/param2.
 * En el periférico, este handler desempaqueta y actualiza el widget.
 */

#define DT_DRV_COMPAT qolera_claude_usage

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk_claude_usage/claude_usage.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    struct zmk_claude_usage_state state =
        zmk_claude_usage_unpack(binding->param1, binding->param2);

    LOG_DBG("claude usage: sesion %u%% (reset %u min), semana %u%% (reset %u min)",
            state.session_pct, state.session_reset_min, state.weekly_pct,
            state.weekly_reset_min);

#ifdef CONFIG_ZMK_CLAUDE_USAGE_WIDGET
    zmk_claude_usage_widget_update(state);
#endif

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_claude_usage_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_claude_usage_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
