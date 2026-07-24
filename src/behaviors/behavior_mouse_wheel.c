/*
 * SPDX-License-Identifier: MIT
 *
 * CODEKEEB PATCH: &mwheel -- one discrete mouse-wheel tick per press.
 *
 * Why this exists instead of ZMK's stock &msc: a sensor-rotate behavior
 * dispatches its binding as a TAP (press now, release tap-ms later), but
 * &msc (zmk,behavior-input-two-axis) only emits scroll from its own
 * delayed work loop and needs the press held across several work cycles.
 * Driving it from an encoder produced literally zero HID reports at any
 * tap-ms -- confirmed with USB logging. A real mouse wheel sends one
 * complete wheel event per detent, so that is exactly what this does:
 * set the scroll delta, send the report, then send a zeroing report so
 * the host doesn't keep scrolling. Works from an encoder and from a
 * normal key alike.
 */

#define DT_DRV_COMPAT zmk_behavior_mouse_wheel

/* IS_ENABLED() comes from Zephyr's sys/util_macro.h and must be included
 * before the #if below can use it -- CONFIG_* macros are injected into
 * every file via -imacros autoconf.h, but the IS_ENABLED macro itself is
 * not. (Same trap that broke behavior_sensor_rotate_dynamic.c once.) */
#include <zephyr/sys/util_macro.h>

/* CENTRAL-ONLY: the HID stack (zmk_hid_mouse_scroll_set,
 * zmk_endpoints_send_mouse_report) only exists in a central build; a split
 * peripheral just forwards raw key events. Guarding the whole file the
 * same way ZMK guards its own HID-sending code. */
#if (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* param1 values. Kept as small positive constants so they survive the
 * uint32 round-trip through the ZMK Studio protocol; the sign is applied
 * here, not carried in the parameter. */
#define MWHEEL_UP 0
#define MWHEEL_DOWN 1
#define MWHEEL_LEFT 2
#define MWHEEL_RIGHT 3

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    int16_t hwheel = 0, wheel = 0;

    switch (binding->param1) {
    case MWHEEL_UP:
        wheel = 1;
        break;
    case MWHEEL_DOWN:
        wheel = -1;
        break;
    case MWHEEL_LEFT:
        hwheel = -1;
        break;
    case MWHEEL_RIGHT:
        hwheel = 1;
        break;
    default:
        LOG_WRN("mwheel: unknown param1 %d", binding->param1);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    /* One full wheel event, then zero it again: the HID mouse report is
     * a level, not an edge, so leaving a non-zero delta in it would make
     * the host scroll forever. */
    zmk_hid_mouse_scroll_set(hwheel, wheel);
    zmk_endpoints_send_mouse_report();
    zmk_hid_mouse_scroll_set(0, 0);
    zmk_endpoints_send_mouse_report();

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

/* Human-readable choices, so the behavior is pickable from ZMK Studio
 * (and from the CODE/KEEB keymap editor) instead of needing a raw number. */
static const struct behavior_parameter_value_metadata param_values[] = {
    {
        .display_name = "Scroll Up",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = MWHEEL_UP,
    },
    {
        .display_name = "Scroll Down",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = MWHEEL_DOWN,
    },
    {
        .display_name = "Scroll Left",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = MWHEEL_LEFT,
    },
    {
        .display_name = "Scroll Right",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = MWHEEL_RIGHT,
    },
};

static const struct behavior_parameter_metadata_set param_set = {
    .param1_values = param_values,
    .param1_values_len = ARRAY_SIZE(param_values),
};

static const struct behavior_parameter_metadata_set sets[] = {param_set};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(sets),
    .sets = sets,
};

#endif /* IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA) */

static const struct behavior_driver_api behavior_mouse_wheel_driver_api = {
    /* Scroll must be emitted by the half that owns the USB/BLE endpoint,
     * i.e. the central -- same as every other HID-sending behavior. */
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

#define MWHEEL_INST(n)                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_mouse_wheel_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MWHEEL_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */

#endif /* (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) */
