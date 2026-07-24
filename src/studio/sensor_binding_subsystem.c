/*
 * SPDX-License-Identifier: MIT
 *
 * CODEKEEB PATCH: ZMK Studio RPC handlers for reading/writing sensor
 * (encoder) rotation bindings. These add two more handlers to the
 * existing `keymap` RPC subsystem (already registered by ZMK's own
 * app/src/studio/keymap_subsystem.c via ZMK_RPC_SUBSYSTEM(keymap)) --
 * they do NOT redefine the subsystem itself, only add handlers for the
 * two new request types this repo's zmk-studio-messages fork adds
 * (get_sensor_bindings, set_sensor_binding). See
 * dts/bindings/behaviors/zmk,behavior-sensor-rotate-dynamic.yaml and
 * src/behaviors/behavior_sensor_rotate_dynamic.c for the behavior side.
 */

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/sensors.h>
#include <zmk/studio/rpc.h>
#include <zmk/sensor_rotate_dynamic.h>

#include <pb_encode.h>

#define KEYMAP_RESPONSE(type, ...) ZMK_RPC_RESPONSE(keymap, type, __VA_ARGS__)

/* Every (layer_id, sensor_idx) pair that has a sensor_rotate_dynamic
 * instance registered is considered "editable"; get_sensor_bindings
 * reports exactly those, nothing more. We rebuild this list by asking
 * the behavior layer, which already keeps its own registry -- to avoid
 * a second parallel registry here, we just probe every (layer, sensor)
 * combination that could plausibly exist and keep the ones that resolve.
 */
static bool bindings_exist_for(uint8_t layer_id, uint8_t sensor_idx) {
    return sensor_rotate_dynamic_get_binding(layer_id, sensor_idx, true) != NULL;
}

static bool encode_sensor_bindings_list(pb_ostream_t *stream, const pb_field_t *field,
                                        void *const *arg) {
    for (uint8_t layer_idx = 0; layer_idx < ZMK_KEYMAP_LAYERS_LEN; layer_idx++) {
        zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(layer_idx);
        if (layer_id == UINT8_MAX) {
            break;
        }

        for (uint8_t sensor_idx = 0; sensor_idx < ZMK_KEYMAP_SENSORS_LEN; sensor_idx++) {
            if (!bindings_exist_for(layer_id, sensor_idx)) {
                continue;
            }

            if (!pb_encode_tag_for_field(stream, field)) {
                return false;
            }

            const struct zmk_behavior_binding *cw =
                sensor_rotate_dynamic_get_binding(layer_id, sensor_idx, true);
            const struct zmk_behavior_binding *ccw =
                sensor_rotate_dynamic_get_binding(layer_id, sensor_idx, false);

            zmk_keymap_SensorBinding sb = zmk_keymap_SensorBinding_init_zero;
            sb.layer_id = layer_id;
            sb.sensor_index = sensor_idx;

            zmk_keymap_BehaviorBinding cw_msg = zmk_keymap_BehaviorBinding_init_zero;
            if (cw && cw->behavior_dev) {
                cw_msg.behavior_id = zmk_behavior_get_local_id(cw->behavior_dev);
                cw_msg.param1 = cw->param1;
                cw_msg.param2 = cw->param2;
            }
            sb.has_clockwise = true;
            sb.clockwise = cw_msg;

            zmk_keymap_BehaviorBinding ccw_msg = zmk_keymap_BehaviorBinding_init_zero;
            if (ccw && ccw->behavior_dev) {
                ccw_msg.behavior_id = zmk_behavior_get_local_id(ccw->behavior_dev);
                ccw_msg.param1 = ccw->param1;
                ccw_msg.param2 = ccw->param2;
            }
            sb.has_counter_clockwise = true;
            sb.counter_clockwise = ccw_msg;

            if (!pb_encode_submessage(stream, &zmk_keymap_SensorBinding_msg, &sb)) {
                return false;
            }
        }
    }

    return true;
}

zmk_studio_Response get_sensor_bindings(const zmk_studio_Request *req) {
    zmk_keymap_GetSensorBindingsResponse resp = zmk_keymap_GetSensorBindingsResponse_init_zero;

    resp.sensor_bindings.funcs.encode = encode_sensor_bindings_list;

    return KEYMAP_RESPONSE(get_sensor_bindings, resp);
}

zmk_studio_Response set_sensor_binding(const zmk_studio_Request *req) {
    const zmk_keymap_SetSensorBindingRequest *set_req =
        &req->subsystem.keymap.request_type.set_sensor_binding;

    if (!set_req->has_binding) {
        return KEYMAP_RESPONSE(set_sensor_binding,
                               zmk_keymap_SetSensorBindingResponse_SET_SENSOR_BINDING_RESP_INVALID_PARAMETERS);
    }

    zmk_behavior_local_id_t bid = set_req->binding.behavior_id;
    const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(bid);

    if (!behavior_name) {
        return KEYMAP_RESPONSE(set_sensor_binding,
                               zmk_keymap_SetSensorBindingResponse_SET_SENSOR_BINDING_RESP_INVALID_BEHAVIOR);
    }

    struct zmk_behavior_binding binding = (struct zmk_behavior_binding){
        .behavior_dev = behavior_name,
        .param1 = set_req->binding.param1,
        .param2 = set_req->binding.param2,
    };

    int ret = sensor_rotate_dynamic_set_binding(set_req->layer_id, set_req->sensor_index,
                                                set_req->clockwise, binding);

    switch (ret) {
    case SENSOR_ROTATE_DYNAMIC_SET_OK:
        raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
            .notification = ZMK_RPC_NOTIFICATION(keymap, unsaved_changes_status_changed, true)});
        return KEYMAP_RESPONSE(set_sensor_binding,
                               zmk_keymap_SetSensorBindingResponse_SET_SENSOR_BINDING_RESP_OK);
    case SENSOR_ROTATE_DYNAMIC_SET_ERR_NOT_FOUND:
        return KEYMAP_RESPONSE(set_sensor_binding,
                               zmk_keymap_SetSensorBindingResponse_SET_SENSOR_BINDING_RESP_INVALID_LOCATION);
    case SENSOR_ROTATE_DYNAMIC_SET_ERR_INVALID_BEHAVIOR:
        return KEYMAP_RESPONSE(set_sensor_binding,
                               zmk_keymap_SetSensorBindingResponse_SET_SENSOR_BINDING_RESP_INVALID_PARAMETERS);
    default:
        return KEYMAP_RESPONSE(set_sensor_binding,
                               zmk_keymap_SetSensorBindingResponse_SET_SENSOR_BINDING_RESP_NOT_SUPPORTED);
    }
}

ZMK_RPC_SUBSYSTEM_HANDLER(keymap, get_sensor_bindings, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(keymap, set_sensor_binding, ZMK_STUDIO_RPC_HANDLER_SECURED);
