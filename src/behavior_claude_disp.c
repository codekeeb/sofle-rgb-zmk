/*
 * Behavior para alternar la pantalla del widget de Claude entre la pantalla
 * de reposo (el bicho) y los datos de uso. Es LOCAL: se ejecuta en la misma
 * mitad donde se pulsa la tecla. Como la tecla (DEL, esquina superior derecha)
 * y el widget están ambos en la mitad derecha (periférico), el toggle actúa
 * directamente sobre el widget sin pasar por el split.
 */

#define DT_DRV_COMPAT qolera_claude_disp

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk_claude_usage/claude_usage.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
#ifdef CONFIG_ZMK_CLAUDE_USAGE_WIDGET
    zmk_claude_usage_widget_toggle();
#endif
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_claude_disp_driver_api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
    /* GLOBAL: el keymap se procesa en la central, pero el widget vive en el
     * periférico (derecha). Con GLOBAL la central reenvía la invocación a
     * todos los periféricos por el split, así que el toggle llega a la mitad
     * que tiene la pantalla. En la central también se ejecuta, pero allí el
     * toggle es no-op (sin widget). */
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_claude_disp_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
