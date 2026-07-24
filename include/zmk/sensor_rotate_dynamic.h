/*
 * SPDX-License-Identifier: MIT
 *
 * CODEKEEB PATCH: runtime-editable sensor (encoder) rotation bindings.
 * See dts/bindings/behaviors/zmk,behavior-sensor-rotate-dynamic.yaml and
 * src/studio/sensor_binding_subsystem.c for the full picture.
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/behavior.h>

enum sensor_rotate_dynamic_set_result {
    SENSOR_ROTATE_DYNAMIC_SET_OK = 0,
    SENSOR_ROTATE_DYNAMIC_SET_ERR_NOT_FOUND = -1,
    SENSOR_ROTATE_DYNAMIC_SET_ERR_INVALID_BEHAVIOR = -2,
};

/**
 * @brief Overwrite the clockwise or counter-clockwise binding of the
 * sensor_rotate_dynamic instance registered for (layer_id, sensor_idx).
 *
 * @param cw true to set the clockwise binding, false for counter-clockwise.
 * @return SENSOR_ROTATE_DYNAMIC_SET_OK, or a negative
 *         sensor_rotate_dynamic_set_result on failure.
 */
int sensor_rotate_dynamic_set_binding(uint8_t layer_id, uint8_t sensor_idx, bool cw,
                                      struct zmk_behavior_binding binding);

/**
 * @brief Read back the clockwise or counter-clockwise binding currently
 * assigned to the sensor_rotate_dynamic instance for (layer_id, sensor_idx).
 *
 * @return NULL if no instance is registered for that (layer, sensor) pair.
 */
const struct zmk_behavior_binding *
sensor_rotate_dynamic_get_binding(uint8_t layer_id, uint8_t sensor_idx, bool cw);
