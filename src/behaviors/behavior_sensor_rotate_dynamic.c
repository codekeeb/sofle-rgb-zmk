/*
 * SPDX-License-Identifier: MIT
 *
 * CODEKEEB PATCH: sensor (encoder) rotation binding, editable at runtime
 * and persisted in flash -- ZMK's stock zmk,behavior-sensor-rotate-var
 * bakes its cw/ccw bindings in forever at compile time from devicetree,
 * with no public way to change them. This behavior keeps the exact same
 * rotation-processing logic (vendored from
 * app/src/behaviors/behavior_sensor_rotate_common.c, tap-based dispatch)
 * but stores its cw/ccw bindings in mutable per-instance data instead of
 * const devicetree config, and exposes a lookup table keyed by
 * (layer_id, sensor_idx) so the studio subsystem can find and rewrite
 * the right instance, with changes persisted via settings the same way
 * regular key bindings already do.
 *
 * CENTRAL-ONLY, on purpose: ZMK itself never runs sensor-rotate behavior
 * logic on a split peripheral -- app/CMakeLists.txt guards
 * behavior_sensor_rotate_common.c (and behavior_queue.c, which
 * zmk_behavior_queue_add lives in) behind
 * `if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)`. A
 * peripheral's own EC11 still reports raw rotation, but that raw sensor
 * event is forwarded to the central over the split link (ZMK's own,
 * already-existing mechanism -- confirmed via USB logging) and it's the
 * CENTRAL that decides what to do with it for both encoders. So this
 * whole file -- the behavior device itself -- only needs to exist there;
 * confirmed by a real CI failure (`undefined reference to
 * zmk_behavior_queue_add`) building this behavior into the peripheral.
 */

#define DT_DRV_COMPAT zmk_behavior_sensor_rotate_dynamic

/* IS_ENABLED() itself comes from Zephyr's sys/util_macro.h -- needs to be
 * included BEFORE the #if below can use it (unlike CONFIG_* macros
 * themselves, which the build already provides to every file via
 * -imacros autoconf.h). */
#include <zephyr/sys/util_macro.h>

#if (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/keymap.h>
#include <zmk/sensors.h>
#include <zmk/virtual_key_position.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/sensor_rotate_dynamic.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct sensor_rotate_dynamic_config {
    uint8_t layer_id;
    uint8_t sensor_idx;
    int tap_ms;
};

struct sensor_rotate_dynamic_data {
    struct zmk_behavior_binding cw_binding;
    struct zmk_behavior_binding ccw_binding;
    struct sensor_value remainder[ZMK_KEYMAP_LAYERS_LEN];
    int triggers[ZMK_KEYMAP_LAYERS_LEN];
};

/* ---- (layer_id, sensor_idx) -> device lookup, for the studio subsystem ----
 *
 * Only a handful of instances ever exist (layers x sensors), so a small
 * static registry populated at init time and scanned linearly is plenty. */
#define MAX_SENSOR_ROTATE_DYNAMIC_INSTANCES (ZMK_KEYMAP_LAYERS_LEN * 4)

static const struct device *registry[MAX_SENSOR_ROTATE_DYNAMIC_INSTANCES];
static size_t registry_len;

static void register_instance(const struct device *dev) {
    if (registry_len >= MAX_SENSOR_ROTATE_DYNAMIC_INSTANCES) {
        LOG_ERR("sensor_rotate_dynamic registry full, dropping instance %s", dev->name);
        return;
    }
    registry[registry_len++] = dev;
}

static const struct device *lookup_instance(uint8_t layer_id, uint8_t sensor_idx) {
    for (size_t i = 0; i < registry_len; i++) {
        const struct sensor_rotate_dynamic_config *cfg = registry[i]->config;
        if (cfg->layer_id == layer_id && cfg->sensor_idx == sensor_idx) {
            return registry[i];
        }
    }
    return NULL;
}

/* ---- public read/write API (used by the studio subsystem) ---- */

const struct zmk_behavior_binding *
sensor_rotate_dynamic_get_binding(uint8_t layer_id, uint8_t sensor_idx, bool cw) {
    const struct device *dev = lookup_instance(layer_id, sensor_idx);
    if (!dev) {
        return NULL;
    }
    struct sensor_rotate_dynamic_data *data = dev->data;
    return cw ? &data->cw_binding : &data->ccw_binding;
}

#if IS_ENABLED(CONFIG_SETTINGS)
static void save_binding(const struct device *dev, bool cw) {
    const struct sensor_rotate_dynamic_config *cfg = dev->config;
    struct sensor_rotate_dynamic_data *data = dev->data;
    const struct zmk_behavior_binding *binding = cw ? &data->cw_binding : &data->ccw_binding;

    struct {
        zmk_behavior_local_id_t behavior_local_id;
        uint32_t param1;
        uint32_t param2;
    } __packed setting = {
        .behavior_local_id = zmk_behavior_get_local_id(binding->behavior_dev),
        .param1 = binding->param1,
        .param2 = binding->param2,
    };

    char key[40];
    snprintf(key, sizeof(key), "sensrot/%d/%d/%d", cfg->layer_id, cfg->sensor_idx, cw ? 1 : 0);
    settings_save_one(key, &setting, sizeof(setting));
}
#endif /* IS_ENABLED(CONFIG_SETTINGS) */

int sensor_rotate_dynamic_set_binding(uint8_t layer_id, uint8_t sensor_idx, bool cw,
                                      struct zmk_behavior_binding binding) {
    const struct device *dev = lookup_instance(layer_id, sensor_idx);
    if (!dev) {
        return SENSOR_ROTATE_DYNAMIC_SET_ERR_NOT_FOUND;
    }

    if (zmk_behavior_validate_binding(&binding) < 0) {
        return SENSOR_ROTATE_DYNAMIC_SET_ERR_INVALID_BEHAVIOR;
    }

    struct sensor_rotate_dynamic_data *data = dev->data;
    if (cw) {
        data->cw_binding = binding;
    } else {
        data->ccw_binding = binding;
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    save_binding(dev, cw);
#endif

    return SENSOR_ROTATE_DYNAMIC_SET_OK;
}

/* ---- settings load ---- */

#if IS_ENABLED(CONFIG_SETTINGS)

struct sensrot_setting_wire {
    zmk_behavior_local_id_t behavior_local_id;
    uint32_t param1;
    uint32_t param2;
} __packed;

static int sensor_rotate_dynamic_settings_set(const char *name, size_t len,
                                              settings_read_cb read_cb, void *cb_arg) {
    /* name is "<layer_id>/<sensor_idx>/<cw>" (the "sensrot/" prefix is
     * already stripped by the settings subsystem for our handler). */
    char *endptr;
    uint8_t layer_id = strtoul(name, &endptr, 10);
    if (*endptr != '/') {
        return -EINVAL;
    }
    uint8_t sensor_idx = strtoul(endptr + 1, &endptr, 10);
    if (*endptr != '/') {
        return -EINVAL;
    }
    bool cw = strtoul(endptr + 1, &endptr, 10) != 0;
    if (*endptr != '\0') {
        return -EINVAL;
    }

    const struct device *dev = lookup_instance(layer_id, sensor_idx);
    if (!dev) {
        LOG_WRN("Loaded sensrot setting for unknown (layer %d, sensor %d), ignoring", layer_id,
                sensor_idx);
        return 0;
    }

    struct sensrot_setting_wire wire = {0};
    int err = read_cb(cb_arg, &wire, MIN(len, sizeof(wire)));
    if (err <= 0) {
        return err;
    }

    const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(wire.behavior_local_id);
    if (!behavior_name) {
        LOG_WRN("sensrot setting (layer %d, sensor %d, cw %d) names an unknown behavior, ignoring",
                layer_id, sensor_idx, cw);
        return 0;
    }

    struct sensor_rotate_dynamic_data *data = dev->data;
    struct zmk_behavior_binding loaded = {
        .behavior_dev = behavior_name,
        .param1 = wire.param1,
        .param2 = wire.param2,
    };
    if (cw) {
        data->cw_binding = loaded;
    } else {
        data->ccw_binding = loaded;
    }

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(sensrot, "sensrot", NULL, sensor_rotate_dynamic_settings_set, NULL,
                               NULL);

#endif /* IS_ENABLED(CONFIG_SETTINGS) */

/* ---- rotation processing (vendored from behavior_sensor_rotate_common.c) ---- */

static int on_sensor_binding_accept_data(struct zmk_behavior_binding *binding,
                                         struct zmk_behavior_binding_event event,
                                         const struct zmk_sensor_config *sensor_config,
                                         size_t channel_data_size,
                                         const struct zmk_sensor_channel_data *channel_data) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct sensor_rotate_dynamic_data *data = dev->data;

    const struct sensor_value value = channel_data[0].value;
    int triggers;

    if (value.val1 == 0) {
        triggers = value.val2;
    } else {
        struct sensor_value remainder = data->remainder[event.layer];

        remainder.val1 += value.val1;
        remainder.val2 += value.val2;

        if (remainder.val2 >= 1000000 || remainder.val2 <= 1000000) {
            remainder.val1 += remainder.val2 / 1000000;
            remainder.val2 %= 1000000;
        }

        int trigger_degrees = 360 / sensor_config->triggers_per_rotation;
        triggers = remainder.val1 / trigger_degrees;
        remainder.val1 %= trigger_degrees;

        data->remainder[event.layer] = remainder;
    }

    data->triggers[event.layer] = triggers;
    return 0;
}

static int on_sensor_binding_process(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event,
                                     enum behavior_sensor_binding_process_mode mode) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct sensor_rotate_dynamic_config *cfg = dev->config;
    struct sensor_rotate_dynamic_data *data = dev->data;

    if (mode != BEHAVIOR_SENSOR_BINDING_PROCESS_MODE_TRIGGER) {
        data->triggers[event.layer] = 0;
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    int triggers = data->triggers[event.layer];

    struct zmk_behavior_binding triggered_binding;
    if (triggers > 0) {
        triggered_binding = data->cw_binding;
    } else if (triggers < 0) {
        triggers = -triggers;
        triggered_binding = data->ccw_binding;
    } else {
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    event.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL;
#endif

    for (int i = 0; i < triggers; i++) {
        zmk_behavior_queue_add(&event, triggered_binding, true, cfg->tap_ms);
        zmk_behavior_queue_add(&event, triggered_binding, false, 0);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api sensor_rotate_dynamic_driver_api = {
    .sensor_binding_accept_data = on_sensor_binding_accept_data,
    .sensor_binding_process = on_sensor_binding_process,
};

static int sensor_rotate_dynamic_init(const struct device *dev) {
    register_instance(dev);
    return 0;
}

/* Extracts the full {behavior_dev, param1, param2} for bindings entry
 * `idx`, same as keymap.c's _TRANSFORM_SENSOR_ENTRY/ZMK_KEYMAP_EXTRACT_BINDING
 * -- NOT just the phandle name like zmk,behavior-sensor-rotate-var does,
 * since our factory bindings (e.g. &rgbfx RGBFX_NEXT(0)) carry real
 * params that a name-only extraction would silently drop to 0. */
#define _SENSOR_ROTATE_DYNAMIC_BINDING(n, idx)                                                     \
    {                                                                                              \
        .behavior_dev = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, bindings, idx)),                  \
        .param1 = COND_CODE_0(DT_INST_PHA_HAS_CELL_AT_IDX(n, bindings, idx, param1), (0),          \
                              (DT_INST_PHA_BY_IDX(n, bindings, idx, param1))),                      \
        .param2 = COND_CODE_0(DT_INST_PHA_HAS_CELL_AT_IDX(n, bindings, idx, param2), (0),          \
                              (DT_INST_PHA_BY_IDX(n, bindings, idx, param2))),                      \
    }

#define SENSOR_ROTATE_DYNAMIC_INST(n)                                                             \
    static struct sensor_rotate_dynamic_config sensor_rotate_dynamic_config_##n = {                \
        .layer_id = DT_INST_PROP(n, layer_id),                                                    \
        .sensor_idx = DT_INST_PROP(n, sensor_index),                                              \
        .tap_ms = DT_INST_PROP(n, tap_ms),                                                        \
    };                                                                                            \
    static struct sensor_rotate_dynamic_data sensor_rotate_dynamic_data_##n = {                    \
        .cw_binding = _SENSOR_ROTATE_DYNAMIC_BINDING(n, 0),                                        \
        .ccw_binding = _SENSOR_ROTATE_DYNAMIC_BINDING(n, 1),                                       \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, sensor_rotate_dynamic_init, NULL,                                  \
                            &sensor_rotate_dynamic_data_##n, &sensor_rotate_dynamic_config_##n,    \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
                            &sensor_rotate_dynamic_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SENSOR_ROTATE_DYNAMIC_INST)

#endif /* (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) */
