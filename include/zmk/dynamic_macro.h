/*
 * SPDX-License-Identifier: MIT
 *
 * CODEKEEB PATCH: runtime-editable macros (&dmac).
 *
 * ZMK macros are devicetree nodes: their key sequence is baked in at
 * compile time and ZMK Studio has no way to create or rewrite one. This
 * provides a fixed bank of macro slots whose contents ARE editable at
 * runtime over the Studio protocol and persisted in flash, so an end user
 * can build "type my email", "n-tilde", "arrow glyph" etc. from the web
 * editor without ever recompiling.
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/keys.h>

/* Keep these in sync with the app + the Kconfig defaults. Each slot costs
 * ZMK_DMAC_MAX_STEPS * 4 bytes of RAM and the same in a settings record. */
#define ZMK_DMAC_MAX_STEPS 24

enum zmk_dmac_step_type {
    /* Tap a HID keycode (with its modifiers), e.g. LS(N) for a capital N. */
    ZMK_DMAC_STEP_TAP = 0,
    /* Emit a Unicode codepoint using the configured OS method. */
    ZMK_DMAC_STEP_UNICODE = 1,
    /* Pause for value milliseconds. */
    ZMK_DMAC_STEP_WAIT = 2,
    /* A cased pair: the low 21 bits are the lowercase codepoint, the top
     * 11 bits hold the uppercase one as an offset from it. Shift picks
     * which one is typed, the way ZMK_UNICODE_PAIR does in a hand
     * written keymap -- without it, Shift+<key> had no second codepoint
     * to reach for. */
    ZMK_DMAC_STEP_UNICODE_PAIR = 3,
};

/* Pack/unpack helpers for ZMK_DMAC_STEP_UNICODE_PAIR.
 *
 * The offset is SIGNED: for Latin-1 the uppercase codepoint is lower
 * than the lowercase one (n-tilde is 00F1 -> 00D1, so -0x20), which is
 * why the 11 bits are sign-extended on the way out. Pairs whose offset
 * does not fit -- eszett, 00DF -> 1E9E -- are stored as a plain
 * ZMK_DMAC_STEP_UNICODE instead by whoever writes the slot. */
#define ZMK_DMAC_PAIR_OFF_MIN (-1024)
#define ZMK_DMAC_PAIR_OFF_MAX (1023)
#define ZMK_DMAC_PAIR_LOWER(v) ((v) & 0x1FFFFF)
#define ZMK_DMAC_PAIR_UPPER(v)                                                                         (ZMK_DMAC_PAIR_LOWER(v) +                                                                           (int32_t)((((v) >> 21) & 0x7FF) | ((((v) >> 21) & 0x400) ? ~0x7FF : 0)))
#define ZMK_DMAC_PAIR_PACK(lo, up)                                                                     (((lo) & 0x1FFFFF) | ((uint32_t)(((up) - (lo)) & 0x7FF) << 21))

struct zmk_dmac_step {
    uint8_t type;   /* enum zmk_dmac_step_type */
    uint32_t value; /* keycode, codepoint or milliseconds */
} __packed;

/* How to type an arbitrary Unicode codepoint. This is inherently
 * OS-specific: there is no HID usage for "insert codepoint". */
enum zmk_dmac_unicode_mode {
    /* Linux/GTK/IBus: Ctrl+Shift+U, hex digits, Enter. */
    ZMK_DMAC_UNICODE_LINUX = 0,
    /* Windows with WinCompose installed: RAlt+U, hex digits, Enter. */
    ZMK_DMAC_UNICODE_WIN_COMPOSE = 1,
    /* macOS with the Unicode Hex Input layout selected: Option held while
     * typing the hex digits. */
    ZMK_DMAC_UNICODE_MACOS = 2,
};

/**
 * @brief Overwrite the step sequence of a macro slot and persist it.
 *
 * @param slot Macro slot index (0 .. CONFIG_ZMK_DYNAMIC_MACRO_SLOTS-1).
 * @param steps Steps to store; may be NULL when len is 0 (clears the slot).
 * @param len Number of steps, up to ZMK_DMAC_MAX_STEPS.
 * @retval 0 on success, -ENODEV for an unknown slot, -E2BIG if len is too
 *         large.
 */
int zmk_dmac_set_steps(uint8_t slot, const struct zmk_dmac_step *steps, uint8_t len);

/**
 * @brief Read back a macro slot's steps.
 *
 * @param len Out: number of steps stored in the slot.
 * @return Pointer to the slot's step array, or NULL if the slot is unknown.
 */
const struct zmk_dmac_step *zmk_dmac_get_steps(uint8_t slot, uint8_t *len);

/** @brief Current Unicode input mode (enum zmk_dmac_unicode_mode). */
uint8_t zmk_dmac_get_unicode_mode(void);

/** @brief Set and persist the Unicode input mode. */
int zmk_dmac_set_unicode_mode(uint8_t mode);
