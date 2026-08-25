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
/* Gap between the individual keys of a Unicode sequence. The sequence is
 * not plain typing: the host has to notice the activation chord (AltGr+U,
 * Ctrl+Shift+U) and switch into hex-entry mode before the digits arrive.
 * Emitting them in one go made WinCompose miss the chord entirely and
 * take "D1<Enter>" as literal typing. */
#define DMAC_UNI_MS 16

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

/* One step of a Unicode sequence: a key to tap, or a modifier to hold or
 * release. Built up front, then played out one per timer tick so the host
 * sees them as separate keystrokes. */
struct uni_ev {
    uint32_t key;
    int8_t hold;  /* 1 press, -1 release, 0 tap */
    bool chord;   /* part of the activation chord: never mask this one */
};

#define UNI_MAX_EVENTS 16
static struct uni_ev uni_seq[UNI_MAX_EVENTS];
static uint8_t uni_len;

static void uni_add_ex(uint32_t key, int8_t hold, bool chord) {
    if (uni_len < UNI_MAX_EVENTS) {
        uni_seq[uni_len].key = key;
        uni_seq[uni_len].hold = hold;
        uni_seq[uni_len].chord = chord;
        uni_len++;
    }
}

/* Default: a normal key, safe to mask. */
static void uni_add(uint32_t key, int8_t hold) { uni_add_ex(key, hold, false); }
/* The activation chord: must reach the host exactly as sent. */
static void uni_add_chord(uint32_t key, int8_t hold) { uni_add_ex(key, hold, true); }

/* Queues the hex digits, most significant nibble first.
 *
 * Always padded to four digits (six above the BMP), which is what
 * WinCompose documents and what a hand written ZMK_UNICODE macro emits:
 * "00F1", not "F1". The short form is ambiguous when the next character
 * typed is itself a hex digit, and some hosts reject it outright. */
static void uni_add_hex(uint32_t cp) {
    int top = (cp > 0xFFFF) ? 20 : 12;
    for (int shift = top; shift >= 0; shift -= 4) {
        uni_add(hex_keycode((cp >> shift) & 0xF), 0);
    }
}

/* Fills uni_seq with the sequence for one codepoint. Nothing is emitted
 * here -- dmac_work_cb plays it out with a gap between each event. */
static void build_unicode(uint32_t cp) {
    const uint32_t k_u = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_U);
    const uint32_t k_enter = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_RETURN_ENTER);
    const uint32_t k_lctrl = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_LEFTCONTROL);
    const uint32_t k_lshft = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_LEFTSHIFT);
    const uint32_t k_lalt = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_LEFTALT);
    const uint32_t k_ralt = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_RIGHTALT);
    const uint32_t k_space = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_SPACEBAR);

    uni_len = 0;

    switch (unicode_mode) {
    case ZMK_DMAC_UNICODE_WIN_COMPOSE:
        /* WinCompose: the documented sequence is
         *     Compose, u, hex digits, Enter
         * with Compose TAPPED, not held. Its default Compose key is
         * Right Alt. Holding RAlt across the "u" sends AltGr+U instead,
         * which is a different chord: WinCompose never enters compose
         * mode and the digits land as literal text ("00f1"). */
        uni_add_chord(k_ralt, 0);
        uni_add(k_u, 0);
        uni_add_hex(cp);
        uni_add(k_enter, 0);
        break;
    case ZMK_DMAC_UNICODE_MACOS:
        /* "Unicode Hex Input" layout: Option held across the digits. */
        uni_add_chord(k_lalt, 1);
        uni_add_hex(cp);
        uni_add_chord(k_lalt, -1);
        break;
    case ZMK_DMAC_UNICODE_LINUX:
    default:
        /* IBus/GTK: Ctrl+Shift+U, hex, then SPACE to commit.
         * Space rather than Enter, matching zmk-helpers: if the host
         * never entered hex mode, a stray space is far less disruptive
         * than a stray newline (which submits forms and chat boxes). */
        uni_add_chord(k_lctrl, 1);
        uni_add_chord(k_lshft, 1);
        uni_add_chord(k_u, 0);
        uni_add_chord(k_lshft, -1);
        uni_add_chord(k_lctrl, -1);
        uni_add_hex(cp);
        uni_add(k_space, 0);
        break;
    }
}

static void dmac_work_cb(struct k_work *work) {
    if (player.idx >= player.len) {
        /* Belt and braces: a macro that ends while a Unicode step was
         * mid-flight must not leave the user's modifiers masked. */
        zmk_hid_masked_modifiers_clear();
        player.busy = false;
        return;
    }

    const struct zmk_dmac_step *s = &player.steps[player.idx];
    uint32_t delay = DMAC_TAP_MS;

    switch (s->type) {
    case ZMK_DMAC_STEP_TAP:
        player.idx++;
        tap_now(s->value);
        break;
    case ZMK_DMAC_STEP_UNICODE: {
        /* Spelled out one event per tick: the host needs to see the
         * activation chord and switch to hex entry before the digits
         * land. Sent together, WinCompose typed "D1" as literal text. */
        if (player.sub == 0) {
            build_unicode(s->value);
        }
        if (player.sub < uni_len) {
            const struct uni_ev *e = &uni_seq[player.sub++];
            /* Hide the modifiers the user is physically holding for
             * every event EXCEPT the activation chord.
             *
             * Without masking, Shift+<macro key> leaves Shift in the
             * report and "u00F1" arrives as "U))F!" -- the shifted face
             * of each digit. Masking the whole sequence is just as
             * broken the other way: the chord IS made of modifiers
             * (AltGr for WinCompose, Ctrl+Shift for IBus), so hiding it
             * means the host never enters hex-entry mode and the digits
             * land as plain text. Only the chord is exempt. */
            if (e->chord) {
                zmk_hid_masked_modifiers_clear();
            } else {
                zmk_hid_masked_modifiers_set(0xFF);
            }
            if (e->hold == 0) {
                tap_now(e->key);
            } else {
                press_now(e->key, e->hold > 0);
            }
            delay = DMAC_UNI_MS;
            if (player.sub >= uni_len) {
                /* Sequence done: move on, and let the host commit it. */
                zmk_hid_masked_modifiers_clear();
                player.sub = 0;
                player.idx++;
                delay = DMAC_UNI_MS * 2;
            }
        } else {
            zmk_hid_masked_modifiers_clear();
            player.sub = 0;
            player.idx++;
        }
        break;
    }
    case ZMK_DMAC_STEP_WAIT:
        player.idx++;
        delay = s->value;
        break;
    default:
        player.idx++;
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
    /* Start from a clean slate: an interrupted macro may have left a
     * mask in place. */
    zmk_hid_masked_modifiers_clear();
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
