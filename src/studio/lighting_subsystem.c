/*
 * SPDX-License-Identifier: MIT
 *
 * CODEKEEB PATCH: ZMK Studio RPC handlers for the lighting.
 *
 * Upstream Studio only speaks core/behaviors/keymap, so a client can
 * rewrite the keymap but cannot touch the RGB or the OLED animation --
 * the only way to change those is to press a key bound to &rgbfx or
 * &oledanim. This registers a `lighting` subsystem (added to this repo's
 * zmk-studio-messages fork) with three requests: read the current state,
 * set RGB values, and pick an OLED animation.
 *
 * Nothing here reimplements state handling: every write goes through the
 * same functions a key press uses, so it is clamped, saved to flash and
 * pushed to the peripheral half exactly the same way.
 */

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zmk/studio/rpc.h>

#if DT_HAS_CHOSEN(zmk_rgb_fx)
#include <zmk/rgb_fx_control_group.h>
#define RGB_DEV DEVICE_DT_GET(DT_CHOSEN(zmk_rgb_fx))
#endif

#if __has_include(<nice_oled/oled_anim.h>)
#include <nice_oled/oled_anim.h>
#define HAS_OLED_ANIM 1
#endif

#define LIGHTING_RESPONSE(type, ...) ZMK_RPC_RESPONSE(lighting, type, __VA_ARGS__)

ZMK_RPC_SUBSYSTEM(lighting)

static zmk_studio_Response get_state(const zmk_studio_Request *req) {
    zmk_lighting_State state = zmk_lighting_State_init_zero;

#if DT_HAS_CHOSEN(zmk_rgb_fx)
    struct zmk_rgb_fx_state rgb;
    if (zmk_rgb_fx_control_get_state(RGB_DEV, &rgb) == 0) {
        state.rgb_available = true;
        state.active = rgb.active;
        state.brightness = rgb.brightness;
        state.brightness_max = rgb.brightness_max;
        state.effect = rgb.effect;
        state.effect_count = rgb.effect_count;
        state.hue = rgb.hue;
        state.speed = rgb.speed;
    }
#endif

#if IS_ENABLED(HAS_OLED_ANIM)
    state.animation_available = true;
    state.animation = nice_oled_anim_get();
    state.animation_count = NICE_OLED_ANIM_COUNT;
#endif

    return LIGHTING_RESPONSE(get_state, state);
}

static zmk_studio_Response set_rgb(const zmk_studio_Request *req) {
#if DT_HAS_CHOSEN(zmk_rgb_fx)
    const zmk_lighting_SetRgbRequest *r = &req->subsystem.lighting.request_type.set_rgb;
    const struct device *dev = RGB_DEV;

    /* Only the fields the client actually sent are applied, so changing
     * the brightness alone does not disturb the rest. */
    if (r->has_active) {
        zmk_rgb_fx_control_handle_command(dev, RGB_FX_CMD_SET_ACTIVE, r->active ? 1 : 0);
    }
    if (r->has_effect) {
        zmk_rgb_fx_control_handle_command(dev, RGB_FX_CMD_SELECT, (uint8_t)r->effect);
    }
    if (r->has_brightness) {
        zmk_rgb_fx_control_handle_command(dev, RGB_FX_CMD_SET_BRIGHTNESS, (uint8_t)r->brightness);
    }
    if (r->has_hue) {
        /* The hue travels halved: 0..359 does not fit in the byte the
         * command parameter is. */
        zmk_rgb_fx_control_handle_command(dev, RGB_FX_CMD_SET_HUE, (uint8_t)((r->hue % 360) / 2));
    }
    if (r->has_speed) {
        zmk_rgb_fx_control_handle_command(dev, RGB_FX_CMD_SET_SPEED, (uint8_t)r->speed);
    }

    return LIGHTING_RESPONSE(set_rgb, true);
#else
    return LIGHTING_RESPONSE(set_rgb, false);
#endif
}

static zmk_studio_Response set_animation(const zmk_studio_Request *req) {
#if IS_ENABLED(HAS_OLED_ANIM)
    uint32_t idx = req->subsystem.lighting.request_type.set_animation;

    return LIGHTING_RESPONSE(set_animation, nice_oled_anim_set((uint8_t)idx) == 0);
#else
    return LIGHTING_RESPONSE(set_animation, false);
#endif
}

/* Reading the state is harmless, so it stays open; the two writes need
 * the keyboard unlocked, like every other change Studio can make. */
ZMK_RPC_SUBSYSTEM_HANDLER(lighting, get_state, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(lighting, set_rgb, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(lighting, set_animation, ZMK_STUDIO_RPC_HANDLER_SECURED);
