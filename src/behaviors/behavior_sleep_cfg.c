/*
 * SPDX-License-Identifier: MIT
 *
 * CODEKEEB PATCH: &sleepcfg <activado> <minutos>.
 *
 * Relay interno central->periferico para el ajuste de deep sleep. No se
 * pone en ninguna tecla: lo invoca el handler RPC de ZMK Studio, igual
 * que &oledset con la animacion del OLED.
 *
 * Hace falta porque el ajuste es local a cada mitad: las dos vigilan su
 * propia inactividad (ver src/codekeeb_sleep.c), asi que cambiarlo solo
 * en la central dejaria al periferico durmiendo con el plazo viejo --
 * o sin dormir, gastando bateria.
 */

#define DT_DRV_COMPAT zmk_behavior_sleep_cfg

#include <zephyr/sys/util_macro.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/codekeeb_sleep.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    zmk_codekeeb_sleep_set(binding->param1 != 0, (uint16_t)binding->param2);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static int behavior_sleep_cfg_init(const struct device *dev) { return 0; }

static const struct behavior_driver_api behavior_sleep_cfg_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, behavior_sleep_cfg_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_sleep_cfg_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
