/*
 * SPDX-License-Identifier: MIT
 *
 * CODEKEEB PATCH: &dmac -- runtime-editable macro slots.
 *
 * ZMK's own macros are compile-time devicetree nodes, so ZMK Studio can
 * only ever re-point a key AT an existing macro, never author one. This
 * behavior is a bank of slots whose step sequences live in mutable data
 * and in flash, letting the CODE/KEEB keymap editor create "type my
 * email", "n-tilde", "→" and so on with no rebuild.
 *
 * Steps are replayed from a work queue rather than inline: a macro can
 * contain waits, and the Unicode sequences below are long (a dozen key
 * events), so blocking the caller would stall the keyboard.
 *
 * CENTRAL-ONLY: emitting HID needs the endpoint the central owns, and
 * &dmac is BEHAVIOR_LOCALITY_CENTRAL, so the peripheral never runs it.
 */

#define DT_DRV_COMPAT zmk_behavior_dynamic_macro

/* IS_ENABLED() lives in Zephyr's sys/util_macro.h and must be included
 * before any file-level #if can use it (CONFIG_* come via -imacros, the
 * macro itself does not). */
#include <zephyr/sys/util_macro.h>

#if (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#include <stdio.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/keys.h>
#include <zmk/hid.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/dynamic_macro.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DMAC_SLOTS CONFIG_ZMK_DYNAMIC_MACRO_SLOTS
/* Gap between the press and release of each emitted key. Long enough that
 * hosts and remote-desktop sessions don't swallow keys, short enough that
 * a sentence-length macro still feels instant. */
#define DMAC_TAP_MS 8

struct dmac_slot {
    struct zmk_dmac_step steps[ZMK_DMAC_MAX_STEPS];
    uint8_t len;
};

static struct dmac_slot slots[DMAC_SLOTS];
static uint8_t unicode_mode = ZMK_DMAC_UNICODE_LINUX;

/* ---- playback ----
 *
 * One player, one queue: macros are typed by a human, so overlapping
 * playback isn't worth the complexity. A new trigger while busy is
 * dropped rather than interleaved (which would emit garbage). */

struct dmac_player {
    struct k_work_delayable work;
    const struct zmk_dmac_step *steps;
    uint8_t len;
    uint8_t idx;
    /* Sub-step cursor, used while spelling out a Unicode sequence. */
    uint8_t sub;
    bool busy;
};

static struct dmac_player player;

static void tap_now(uint32_t encoded) {
    raise_zmk_keycode_state_changed_from_encoded(encoded, true, k_uptime_get());
    raise_zmk_keycode_state_changed_from_encoded(encoded, false, k_uptime_get());
}

static void press_now(uint32_t encoded, bool pressed) {
    raise_zmk_keycode_state_changed_from_encoded(encoded, pressed, k_uptime_get());
}

/* Hex digit -> HID keycode, for the Unicode input sequences. */
static uint32_t hex_keycode(uint8_t nibble) {
    static const uint32_t digits[] = {HID_USAGE_KEY_KEYBOARD_0_AND_RIGHT_PARENTHESIS,
                                      HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION,
                                      HID_USAGE_KEY_KEYBOARD_2_AND_AT,
                                      HID_USAGE_KEY_KEYBOARD_3_AND_HASH,
                                      HID_USAGE_KEY_KEYBOARD_4_AND_DOLLAR,
                                      HID_USAGE_KEY_KEYBOARD_5_AND_PERCENT,
                                      HID_USAGE_KEY_KEYBOARD_6_AND_CARET,
                                      HID_USAGE_KEY_KEYBOARD_7_AND_AMPERSAND,
                                      HID_USAGE_KEY_KEYBOARD_8_AND_ASTERISK,
                                      HID_USAGE_KEY_KEYBOARD_9_AND_LEFT_PARENTHESIS};
    static const uint32_t letters[] = {
        HID_USAGE_KEY_KEYBOARD_A, HID_USAGE_KEY_KEYBOARD_B, HID_USAGE_KEY_KEYBOARD_C,
        HID_USAGE_KEY_KEYBOARD_D, HID_USAGE_KEY_KEYBOARD_E, HID_USAGE_KEY_KEYBOARD_F};

    uint32_t usage = (nibble < 10) ? digits[nibble] : letters[nibble - 10];
    return ZMK_HID_USAGE(HID_USAGE_KEY, usage);
}

/* Types a codepoint as hex digits, most significant nibble first, skipping
 * leading zeros (every method below accepts a short form). */
static void type_hex(uint32_t cp) {
    bool started = false;
    for (int shift = 20; shift >= 0; shift -= 4) {
        uint8_t nib = (cp >> shift) & 0xF;
        if (!started && nib == 0 && shift > 0) {
            continue;
        }
        started = true;
        tap_now(hex_keycode(nib));
    }
}

static void emit_unicode(uint32_t cp) {
    const uint32_t k_u = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_U);
    const uint32_t k_enter = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_RETURN_ENTER);
    const uint32_t k_lctrl = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_LEFTCONTROL);
    const uint32_t k_lshft = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_LEFTSHIFT);
    const uint32_t k_lalt = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_LEFTALT);
    const uint32_t k_ralt = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_RIGHTALT);

    switch (unicode_mode) {
    case ZMK_DMAC_UNICODE_WIN_COMPOSE:
        /* WinCompose: RAlt+U, then hex, then Enter. */
        press_now(k_ralt, true);
        tap_now(k_u);
        press_now(k_ralt, false);
        type_hex(cp);
        tap_now(k_enter);
        break;
    case ZMK_DMAC_UNICODE_MACOS:
        /* "Unicode Hex Input" layout: Option held across the digits. */
        press_now(k_lalt, true);
        type_hex(cp);
        press_now(k_lalt, false);
        break;
    case ZMK_DMAC_UNICODE_LINUX:
    default:
        /* IBus/GTK: Ctrl+Shift+U, hex, Enter. */
        press_now(k_lctrl, true);
        press_now(k_lshft, true);
        tap_now(k_u);
        press_now(k_lshft, false);
        press_now(k_lctrl, false);
        type_hex(cp);
        tap_now(k_enter);
        break;
    }
}

static void dmac_work_cb(struct k_work *work) {
    if (player.idx >= player.len) {
        player.busy = false;
        return;
    }

    const struct zmk_dmac_step *s = &player.steps[player.idx++];
    uint32_t delay = DMAC_TAP_MS;

    switch (s->type) {
    case ZMK_DMAC_STEP_TAP:
        tap_now(s->value);
        break;
    case ZMK_DMAC_STEP_UNICODE:
        emit_unicode(s->value);
        /* The sequence above is many events; give the host a beat to
         * digest it before the next step. */
        delay = DMAC_TAP_MS * 4;
        break;
    case ZMK_DMAC_STEP_WAIT:
        delay = s->value;
        break;
    default:
        LOG_WRN("dmac: unknown step type %d", s->type);
        break;
    }

    k_work_schedule(&player.work, K_MSEC(delay));
}

/* ---- persistence ---- */

#if IS_ENABLED(CONFIG_SETTINGS)
static void save_slot(uint8_t slot) {
    char key[32];
    snprintf(key, sizeof(key), "dmac/s/%d", slot);
    /* Store only the used prefix: a mostly-empty bank shouldn't burn
     * flash writing zeros. */
    size_t sz = sizeof(uint8_t) + slots[slot].len * sizeof(struct zmk_dmac_step);
    struct {
        uint8_t len;
        struct zmk_dmac_step steps[ZMK_DMAC_MAX_STEPS];
    } __packed rec;
    rec.len = slots[slot].len;
    memcpy(rec.steps, slots[slot].steps, slots[slot].len * sizeof(struct zmk_dmac_step));
    settings_save_one(key, &rec, sz);
}

static void save_unicode_mode(void) {
    settings_save_one("dmac/um", &unicode_mode, sizeof(unicode_mode));
}
#endif /* IS_ENABLED(CONFIG_SETTINGS) */

/* ---- public API ---- */

int zmk_dmac_set_steps(uint8_t slot, const struct zmk_dmac_step *steps, uint8_t len) {
    if (slot >= DMAC_SLOTS) {
        return -ENODEV;
    }
    if (len > ZMK_DMAC_MAX_STEPS) {
        return -E2BIG;
    }

    slots[slot].len = len;
    if (len && steps) {
        memcpy(slots[slot].steps, steps, len * sizeof(struct zmk_dmac_step));
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    save_slot(slot);
#endif
    LOG_DBG("dmac: slot %d now has %d steps", slot, len);
    return 0;
}

const struct zmk_dmac_step *zmk_dmac_get_steps(uint8_t slot, uint8_t *len) {
    if (slot >= DMAC_SLOTS) {
        return NULL;
    }
    if (len) {
        *len = slots[slot].len;
    }
    return slots[slot].steps;
}

uint8_t zmk_dmac_get_unicode_mode(void) { return unicode_mode; }

int zmk_dmac_set_unicode_mode(uint8_t mode) {
    if (mode > ZMK_DMAC_UNICODE_MACOS) {
        return -EINVAL;
    }
    unicode_mode = mode;
#if IS_ENABLED(CONFIG_SETTINGS)
    save_unicode_mode();
#endif
    return 0;
}

/* ---- settings load ---- */

#if IS_ENABLED(CONFIG_SETTINGS)
static int dmac_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (settings_name_steq(name, "um", NULL)) {
        uint8_t m;
        int err = read_cb(cb_arg, &m, MIN(len, sizeof(m)));
        if (err > 0 && m <= ZMK_DMAC_UNICODE_MACOS) {
            unicode_mode = m;
        }
        return 0;
    }

    const char *next;
    if (!settings_name_steq(name, "s", &next) || !next) {
        return 0;
    }

    uint8_t slot = strtoul(next, NULL, 10);
    if (slot >= DMAC_SLOTS) {
        LOG_WRN("dmac: stored slot %d is out of range, ignoring", slot);
        return 0;
    }

    struct {
        uint8_t len;
        struct zmk_dmac_step steps[ZMK_DMAC_MAX_STEPS];
    } __packed rec = {0};

    int err = read_cb(cb_arg, &rec, MIN(len, sizeof(rec)));
    if (err <= 0) {
        return err;
    }
    if (rec.len > ZMK_DMAC_MAX_STEPS) {
        LOG_WRN("dmac: stored slot %d claims %d steps, ignoring", slot, rec.len);
        return 0;
    }

    slots[slot].len = rec.len;
    memcpy(slots[slot].steps, rec.steps, rec.len * sizeof(struct zmk_dmac_step));
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dmac, "dmac", NULL, dmac_settings_set, NULL, NULL);
#endif /* IS_ENABLED(CONFIG_SETTINGS) */

/* ---- behavior ---- */

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    uint8_t slot = binding->param1;

    if (slot >= DMAC_SLOTS) {
        LOG_WRN("dmac: slot %d out of range", slot);
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (!slots[slot].len) {
        /* Empty slot: nothing configured yet, stay silent. */
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (player.busy) {
        LOG_DBG("dmac: busy, ignoring slot %d", slot);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    player.steps = slots[slot].steps;
    player.len = slots[slot].len;
    player.idx = 0;
    player.sub = 0;
    player.busy = true;
    k_work_schedule(&player.work, K_NO_WAIT);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
/* One entry per slot so Studio shows "Macro 1..N" instead of a raw int. */
#define DMAC_SLOT_META(i, _)                                                                       \
    {.display_name = "Macro " #i, .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = i},

static const struct behavior_parameter_value_metadata param_values[] = {
    LISTIFY(CONFIG_ZMK_DYNAMIC_MACRO_SLOTS, DMAC_SLOT_META, ())};

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

static const struct behavior_driver_api behavior_dynamic_macro_driver_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

static int dmac_init(const struct device *dev) {
    k_work_init_delayable(&player.work, dmac_work_cb);
    return 0;
}

#define DMAC_INST(n)                                                                               \
    BEHAVIOR_DT_INST_DEFINE(n, dmac_init, NULL, NULL, NULL, POST_KERNEL,                           \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_dynamic_macro_driver_api);

DT_INST_FOREACH_STATUS_OKAY(DMAC_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */

#endif /* (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) */
