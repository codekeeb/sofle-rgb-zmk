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
};

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
