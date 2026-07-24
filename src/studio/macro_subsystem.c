/*
 * SPDX-License-Identifier: MIT
 *
 * CODEKEEB PATCH: ZMK Studio RPC handlers for the runtime-editable macro
 * slots (&dmac). These add handlers to the EXISTING `keymap` subsystem
 * (already registered by ZMK's app/src/studio/keymap_subsystem.c) for the
 * request types this repo's zmk-studio-messages fork adds:
 * get_macros / set_macro / set_unicode_mode.
 *
 * See src/behaviors/behavior_dynamic_macro.c for the storage/playback side.
 */

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <zmk/studio/rpc.h>
#include <zmk/dynamic_macro.h>

#include <pb_encode.h>

#define KEYMAP_RESPONSE(type, ...) ZMK_RPC_RESPONSE(keymap, type, __VA_ARGS__)

/* Wire layout of one step inside the packed `steps` blob: 1 byte type
 * followed by a little-endian uint32 value. Kept deliberately dumb so the
 * browser can build it with a DataView and no protobuf library. */
#define STEP_WIRE_SIZE 5

/* ---- get_macros ---- */

static bool encode_macro_steps(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    uint8_t slot = (uint8_t)(uintptr_t)*arg;
    uint8_t len = 0;
    const struct zmk_dmac_step *steps = zmk_dmac_get_steps(slot, &len);

    for (uint8_t i = 0; i < len; i++) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        zmk_keymap_MacroStep st = zmk_keymap_MacroStep_init_zero;
        st.type = steps[i].type;
        st.value = steps[i].value;
        if (!pb_encode_submessage(stream, &zmk_keymap_MacroStep_msg, &st)) {
            return false;
        }
    }
    return true;
}

static bool encode_macros(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    for (uint8_t slot = 0; slot < CONFIG_ZMK_DYNAMIC_MACRO_SLOTS; slot++) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        zmk_keymap_Macro m = zmk_keymap_Macro_init_zero;
        m.slot = slot;
        m.steps.funcs.encode = encode_macro_steps;
        m.steps.arg = (void *)(uintptr_t)slot;

        if (!pb_encode_submessage(stream, &zmk_keymap_Macro_msg, &m)) {
            return false;
        }
    }
    return true;
}

zmk_studio_Response get_macros(const zmk_studio_Request *req) {
    zmk_keymap_GetMacrosResponse resp = zmk_keymap_GetMacrosResponse_init_zero;
    resp.macros.funcs.encode = encode_macros;
    resp.unicode_mode = zmk_dmac_get_unicode_mode();
    resp.max_steps = ZMK_DMAC_MAX_STEPS;
    return KEYMAP_RESPONSE(get_macros, resp);
}

/* ---- set_macro ---- */

zmk_studio_Response set_macro(const zmk_studio_Request *req) {
    const zmk_keymap_SetMacroRequest *set_req = &req->subsystem.keymap.request_type.set_macro;

    if (set_req->steps.size % STEP_WIRE_SIZE) {
        LOG_WRN("set_macro: %d bytes is not a whole number of steps", set_req->steps.size);
        return KEYMAP_RESPONSE(set_macro,
                               zmk_keymap_SetMacroResponse_SET_MACRO_RESP_NOT_SUPPORTED);
    }

    uint32_t count = set_req->steps.size / STEP_WIRE_SIZE;
    if (count > ZMK_DMAC_MAX_STEPS) {
        return KEYMAP_RESPONSE(set_macro, zmk_keymap_SetMacroResponse_SET_MACRO_RESP_TOO_LONG);
    }

    struct zmk_dmac_step steps[ZMK_DMAC_MAX_STEPS];
    const uint8_t *b = set_req->steps.bytes;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = b + (i * STEP_WIRE_SIZE);
        steps[i].type = p[0];
        steps[i].value = (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 16) |
                         ((uint32_t)p[4] << 24);
    }

    int ret = zmk_dmac_set_steps((uint8_t)set_req->slot, steps, (uint8_t)count);

    switch (ret) {
    case 0:
        raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
            .notification = ZMK_RPC_NOTIFICATION(keymap, unsaved_changes_status_changed, true)});
        return KEYMAP_RESPONSE(set_macro, zmk_keymap_SetMacroResponse_SET_MACRO_RESP_OK);
    case -ENODEV:
        return KEYMAP_RESPONSE(set_macro, zmk_keymap_SetMacroResponse_SET_MACRO_RESP_INVALID_SLOT);
    case -E2BIG:
        return KEYMAP_RESPONSE(set_macro, zmk_keymap_SetMacroResponse_SET_MACRO_RESP_TOO_LONG);
    default:
        return KEYMAP_RESPONSE(set_macro,
                               zmk_keymap_SetMacroResponse_SET_MACRO_RESP_NOT_SUPPORTED);
    }
}

/* ---- set_unicode_mode ---- */

zmk_studio_Response set_unicode_mode(const zmk_studio_Request *req) {
    const zmk_keymap_SetUnicodeModeRequest *m =
        &req->subsystem.keymap.request_type.set_unicode_mode;

    if (zmk_dmac_set_unicode_mode((uint8_t)m->mode) < 0) {
        return KEYMAP_RESPONSE(set_unicode_mode,
                               zmk_keymap_SetMacroResponse_SET_MACRO_RESP_NOT_SUPPORTED);
    }
    return KEYMAP_RESPONSE(set_unicode_mode, zmk_keymap_SetMacroResponse_SET_MACRO_RESP_OK);
}

ZMK_RPC_SUBSYSTEM_HANDLER(keymap, get_macros, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(keymap, set_macro, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(keymap, set_unicode_mode, ZMK_STUDIO_RPC_HANDLER_SECURED);
