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
#include <zmk/behavior.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

#if DT_HAS_CHOSEN(zmk_rgb_fx)
#include <zmk/rgb_fx_control_group.h>
#define RGB_DEV DEVICE_DT_GET(DT_CHOSEN(zmk_rgb_fx))
#endif

/* La animacion del OLED la aporta el modulo zmk-nice-oled. Antes esto
 * dependia de __has_include, pero el modulo exponia su include/ solo a
 * su propia libreria, asi que la cabecera no se encontraba y los dos
 * handlers del OLED se compilaban vacios EN SILENCIO: la web mandaba la
 * peticion, recibia "false" y la animacion no cambiaba nunca.
 * Ahora se decide por Kconfig, que es lo fiable, y las funciones se
 * declaran aqui por si el include path no llega. */
#if IS_ENABLED(CONFIG_NICE_OLED_ON)
#define HAS_OLED_ANIM 1
#define NICE_OLED_ANIM_COUNT 6
uint8_t nice_oled_anim_get(void);
int nice_oled_anim_set(uint8_t idx);
#endif

/* La vista de WPM solo existe en la mitad que lleva la pantalla con el
 * ciclo compilado (el central: es la que habla con Studio), de ahi que
 * dependa de su propio Kconfig y no de NICE_OLED_ON. Se declara aqui
 * por el mismo motivo que arriba. */
#if IS_ENABLED(CONFIG_NICE_OLED_WPM_VIEW_SELECTABLE)
#define HAS_WPM_VIEW 1
#define NICE_OLED_WPM_VIEW_COUNT 5
uint8_t nice_oled_wpm_view_get(void);
int nice_oled_wpm_view_set(uint8_t idx);
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

#if IS_ENABLED(HAS_WPM_VIEW)
    state.wpm_view_available = true;
    state.wpm_view = nice_oled_wpm_view_get();
    state.wpm_view_count = NICE_OLED_WPM_VIEW_COUNT;
#endif

    return LIGHTING_RESPONSE(get_lighting, state);
}

static zmk_studio_Response set_rgb(const zmk_studio_Request *req) {
#if DT_HAS_CHOSEN(zmk_rgb_fx)
    const zmk_keymap_SetRgbRequest *r = &req->subsystem.keymap.request_type.set_rgb;
    const struct device *dev = RGB_DEV;

    /* Only the fields the client actually sent are applied, so changing
     * the brightness alone does not disturb the rest. They go in a single
     * apply() rather than one command each: separate commands meant a
     * flash write and a sync to the peripheral per field, and a bad
     * effect index aborted everything after it. */
    struct zmk_rgb_fx_set set = {
        .active = r->has_active ? (r->active ? 1 : 0) : -1,
        .brightness = r->has_brightness ? (int16_t)r->brightness : -1,
        .effect = r->has_effect ? (int16_t)r->effect : -1,
        .hue = r->has_hue ? (int16_t)(r->hue % 360) : -1,
        .speed = r->has_speed ? (int16_t)r->speed : -1,
    };

    return LIGHTING_RESPONSE(set_rgb, zmk_rgb_fx_control_apply(dev, &set) == 0);
#else
    return LIGHTING_RESPONSE(set_rgb, false);
#endif
}

static zmk_studio_Response set_animation(const zmk_studio_Request *req) {
#if IS_ENABLED(HAS_OLED_ANIM)
    uint32_t idx = req->subsystem.keymap.request_type.set_animation;

    if (idx >= NICE_OLED_ANIM_COUNT) {
        return LIGHTING_RESPONSE(set_animation, false);
    }

    /* La pantalla animada la lleva el PERIFERICO, y el estado de la
     * animacion es local a cada mitad: llamar aqui a
     * nice_oled_anim_set() solo cambiaba la copia del central, que no
     * tiene esa pantalla, asi que desde Studio no se veia nada. La
     * tecla &oledanim si funciona porque ZMK la reenvia (locality
     * global). Se hace lo mismo a mano, igual que el RGB con "rgbsync":
     * se aplica en local y se invoca el behavior en el periferico. */
    nice_oled_anim_set((uint8_t)idx);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    struct zmk_behavior_binding binding = {
        .behavior_dev = "oledset",
        .param1 = idx,
    };
    struct zmk_behavior_binding_event event = {
        .position = 0,
        .timestamp = k_uptime_get(),
    };

    for (uint8_t source = 0; source < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; source++) {
        zmk_split_central_invoke_behavior(source, &binding, event, true);
    }
#endif

    return LIGHTING_RESPONSE(set_animation, true);
#else
    return LIGHTING_RESPONSE(set_animation, false);
#endif
}

/* Reading the state is harmless, so it stays open; the two writes need
 * the keyboard unlocked, like every other change Studio can make. */
/* A diferencia de set_animation, aqui no hay relay: la pantalla del
 * ciclo de WPM es la del propio central, que es quien atiende el RPC. */
static zmk_studio_Response set_wpm_view(const zmk_studio_Request *req) {
#if IS_ENABLED(HAS_WPM_VIEW)
    uint32_t idx = req->subsystem.keymap.request_type.set_wpm_view;

    if (idx >= NICE_OLED_WPM_VIEW_COUNT) {
        return LIGHTING_RESPONSE(set_wpm_view, false);
    }

    return LIGHTING_RESPONSE(set_wpm_view, nice_oled_wpm_view_set((uint8_t)idx) == 0);
#else
    return LIGHTING_RESPONSE(set_wpm_view, false);
#endif
}

ZMK_RPC_SUBSYSTEM_HANDLER(keymap, get_lighting, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(keymap, set_rgb, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(keymap, set_animation, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(keymap, set_wpm_view, ZMK_STUDIO_RPC_HANDLER_SECURED);
