/*
 * Copyright (c) 2024 Kuba Birecki
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <zephyr/kernel.h>

/**
 * Animation control commands
 */
#define RGB_FX_CMD_TOGGLE 0
#define RGB_FX_CMD_NEXT 1
#define RGB_FX_CMD_PREVIOUS 2
#define RGB_FX_CMD_SELECT 3
#define RGB_FX_CMD_BRIGHTEN 4
#define RGB_FX_CMD_DIM 5
#define RGB_FX_CMD_NEXT_CONTROL_ZONE 6
#define RGB_FX_CMD_SPEED_UP 10
#define RGB_FX_CMD_SPEED_DOWN 11
#define RGB_FX_CMD_HUE_UP 8
#define RGB_FX_CMD_HUE_DOWN 9
#define RGB_FX_CMD_PREVIOUS_CONTROL_ZONE 7

/* CODEKEEB: absolute setters, for ZMK Studio. The commands above are all
 * relative (next, brighten, hue up...), which is what a key press needs,
 * but a slider in the editor has to be able to say "brightness = 3"
 * without stepping there one command at a time. The param carries the
 * value; everything else -- refresh, saving to flash, pushing the state
 * to the peripheral -- is the same path the relative commands take. */
#define RGB_FX_CMD_SET_BRIGHTNESS 20
#define RGB_FX_CMD_SET_HUE 21   /* param = hue / 2, to fit 0..179 in a byte */
#define RGB_FX_CMD_SET_SPEED 22
#define RGB_FX_CMD_SET_ACTIVE 23

int zmk_rgb_fx_control_handle_command(const struct device *dev, uint8_t command, uint8_t param);

/**
 * @brief Read the current RGB state.
 *
 * Lets a client show what the keyboard is actually doing instead of
 * guessing. Returns 0 on success, -ENODEV if there is no control group.
 */
struct zmk_rgb_fx_state {
    bool active;
    uint8_t brightness;      /* 0..brightness_steps */
    uint8_t brightness_max;
    uint8_t effect;          /* index into the control group's fx list */
    uint8_t effect_count;
    uint16_t hue;            /* 0..359 */
    uint8_t speed;           /* 0..4 */
};

int zmk_rgb_fx_control_get_state(const struct device *dev, struct zmk_rgb_fx_state *out);

/**
 * @brief Apply several values at once.
 *
 * Sending them as separate commands meant one refresh, one flash write
 * and one sync to the peripheral EACH, several of them within a few
 * milliseconds, and a SELECT with an out-of-range index aborted the rest.
 * This applies everything first and settles once.
 *
 * Fields with a negative value are left untouched.
 */
struct zmk_rgb_fx_set {
    int16_t active;      /* 0/1, or <0 to leave as is */
    int16_t brightness;
    int16_t effect;
    int16_t hue;         /* 0..359 */
    int16_t speed;
};

int zmk_rgb_fx_control_apply(const struct device *dev, const struct zmk_rgb_fx_set *set);
