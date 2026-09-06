/*
 * SPDX-License-Identifier: MIT
 *
 * CODEKEEB PATCH: ZMK Studio RPC handlers for the lighting.
 *
 * Upstream Studio only speaks core/behaviors/keymap, so a client can
 * rewrite the keymap but cannot touch the RGB or the OLED animation --
 * the only way to change those is to press a key bound to &rgbfx or
 * &oledanim. These add three handlers to the existing `keymap` RPC
 * subsystem -- get_lighting, set_rgb and set_animation, added to this
 * repo's zmk-studio-messages fork. They live in `keymap` rather than a
 * subsystem of their own because ZMK's CMakeLists hardcodes the list of
 * .proto files it compiles, so a new file would mean patching ZMK too;
 * the sensor-binding and macro patches in this repo do the same.
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

/* Los handlers viven en el subsistema `keymap`, que ZMK ya registra: el
 * CMakeLists de ZMK lleva cableada la lista de .proto que compila, asi
 * que un subsistema nuevo habria exigido parchear ZMK. Los parches
 * anteriores de este repo (sensor bindings, macros) hacen lo mismo. */
#define LIGHTING_RESPONSE(type, ...) ZMK_RPC_RESPONSE(keymap, type, __VA_ARGS__)

static zmk_studio_Response get_lighting(const zmk_studio_Request *req) {
    zmk_keymap_LightingState state = zmk_keymap_LightingState_init_zero;

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

    return LIGHTING_RESPONSE(get_lighting, state);
}

static zmk_studio_Response set_rgb(const zmk_studio_Request *req) {
#if DT_HAS_CHOSEN(zmk_rgb_fx)
    const zmk_keymap_SetRgbRequest *r = &req->subsystem.keymap.request_type.set_rgb;
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
    uint32_t idx = req->subsystem.keymap.request_type.set_animation;

    return LIGHTING_RESPONSE(set_animation, nice_oled_anim_set((uint8_t)idx) == 0);
#else
    return LIGHTING_RESPONSE(set_animation, false);
#endif
}

/* Reading the state is harmless, so it stays open; the two writes need
 * the keyboard unlocked, like every other change Studio can make. */
ZMK_RPC_SUBSYSTEM_HANDLER(keymap, get_lighting, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(keymap, set_rgb, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(keymap, set_animation, ZMK_STUDIO_RPC_HANDLER_SECURED);
