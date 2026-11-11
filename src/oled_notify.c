/*
 * SPDX-License-Identifier: MIT
 *
 * CODEKEEB: short notices on the right screen for things only the left
 * half knows about (Bluetooth profile, cleared bond, USB/BLE output,
 * Caps Lock, ZMK Studio unlocked).
 *
 * The left half is the central: it owns the Bluetooth profiles, the
 * output and the host's lock LEDs. It listens for those events and sends
 * a small code to the right half through the `oledmsg` split behavior;
 * the right half turns the code into text and shows it under its
 * animation (zmk-nice-oled, widgets/toast.c). The RGB keys announce
 * themselves the same way from the rgbsync handler (fx/control_group.c).
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* param1 = code << 8 | arg */
#define NOTIFY_BT_PROFILE 1 /* arg: profile index (shown 1-based) */
#define NOTIFY_BT_CLEAR 2   /* arg: profile index */
#define NOTIFY_OUTPUT 3     /* arg: 0 USB, 1 BLE */
#define NOTIFY_CAPS 4       /* arg: 0 off, 1 on */
#define NOTIFY_STUDIO 5     /* arg: 0 locked, 1 unlocked */

#define NOTIFY_MS 1500

/* ---- right half: show the notice ---- */

#if IS_ENABLED(CONFIG_NICE_OLED_TOAST)
#include <nice_oled/toast.h>

#define DT_DRV_COMPAT zmk_behavior_oled_notify

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int oled_notify_pressed(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    uint8_t code = (binding->param1 >> 8) & 0xff;
    uint8_t arg = binding->param1 & 0xff;
    char msg[12];

    switch (code) {
    case NOTIFY_BT_PROFILE:
        snprintf(msg, sizeof(msg), "BT %d", arg + 1);
        break;
    case NOTIFY_BT_CLEAR:
        snprintf(msg, sizeof(msg), "BT%d clr", arg + 1);
        break;
    case NOTIFY_OUTPUT:
        strcpy(msg, arg ? "BLE" : "USB");
        break;
    case NOTIFY_CAPS:
        strcpy(msg, arg ? "Caps on" : "Caps off");
        break;
    case NOTIFY_STUDIO:
        strcpy(msg, arg ? "Studio" : "Locked");
        break;
    default:
        return ZMK_BEHAVIOR_OPAQUE;
    }

    nice_oled_toast_show("", msg, NOTIFY_MS);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int oled_notify_released(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api oled_notify_driver_api = {
    .binding_pressed = oled_notify_pressed,
    .binding_released = oled_notify_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &oled_notify_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY */
#endif /* CONFIG_NICE_OLED_TOAST */

/* ---- left half: watch the events and send the codes ---- */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

#include <zmk/split/central.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/events/hid_indicators_changed.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_STUDIO)
#include <zmk/studio/core.h>
#endif

/* Events also fire while the keyboard boots and restores its state;
 * nothing was pressed then, so nothing is announced. */
#define NOTIFY_QUIET_BOOT_MS 4000

K_MSGQ_DEFINE(notify_q, sizeof(uint32_t), 8, 4);

static void notify_send_cb(struct k_work *work) {
    uint32_t param;

    while (k_msgq_get(&notify_q, &param, K_NO_WAIT) == 0) {
        struct zmk_behavior_binding binding = {
            .behavior_dev = "oledmsg",
            .param1 = param,
        };
        struct zmk_behavior_binding_event event = {
            .position = 0,
            .timestamp = k_uptime_get(),
        };

        for (uint8_t source = 0; source < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; source++) {
            zmk_split_central_invoke_behavior(source, &binding, event, true);
        }
    }
}

static K_WORK_DEFINE(notify_send_work, notify_send_cb);

static void notify(uint8_t code, uint8_t arg) {
    if (k_uptime_get() < NOTIFY_QUIET_BOOT_MS) {
        return;
    }

    uint32_t param = ((uint32_t)code << 8) | arg;

    k_msgq_put(&notify_q, &param, K_NO_WAIT);
    k_work_submit(&notify_send_work);
}

static int last_profile = -1;
static bool last_profile_open;
static int last_transport = -1;
static int last_caps = -1;

static int oled_notify_listener(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *bt = as_zmk_ble_active_profile_changed(eh);
    if (bt) {
        bool open = zmk_ble_active_profile_is_open();

        if (bt->index != last_profile) {
            if (last_profile >= 0) {
                notify(NOTIFY_BT_PROFILE, bt->index);
            }
        } else if (open && !last_profile_open) {
            /* same profile, its bond just went away: BT_CLR */
            notify(NOTIFY_BT_CLEAR, bt->index);
        }

        last_profile = bt->index;
        last_profile_open = open;
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_endpoint_changed *ep = as_zmk_endpoint_changed(eh);
    if (ep) {
        int transport = ep->endpoint.transport == ZMK_TRANSPORT_BLE ? 1 : 0;

        if (last_transport >= 0 && transport != last_transport) {
            notify(NOTIFY_OUTPUT, transport);
        }
        last_transport = transport;
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    const struct zmk_hid_indicators_changed *ind = as_zmk_hid_indicators_changed(eh);
    if (ind) {
        int caps = (ind->indicators & BIT(1)) ? 1 : 0; /* HID LED usage 2 = Caps Lock */

        if (last_caps >= 0 && caps != last_caps) {
            notify(NOTIFY_CAPS, caps);
        }
        last_caps = caps;
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_STUDIO)
    const struct zmk_studio_core_lock_state_changed *lock =
        as_zmk_studio_core_lock_state_changed(eh);
    if (lock) {
        notify(NOTIFY_STUDIO, lock->state == ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED ? 1 : 0);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(oled_notify, oled_notify_listener);
ZMK_SUBSCRIPTION(oled_notify, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(oled_notify, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
ZMK_SUBSCRIPTION(oled_notify, zmk_hid_indicators_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_STUDIO)
ZMK_SUBSCRIPTION(oled_notify, zmk_studio_core_lock_state_changed);
#endif

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
