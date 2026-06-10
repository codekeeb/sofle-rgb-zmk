/*
 * Servicio GATT en la mitad central: el daemon del PC escribe aquí el
 * paquete de uso (6 bytes LE: pct sesión, pct semana, u16 min reset
 * sesión, u16 min reset semana) y se reenvía al periférico invocando
 * el behavior claude_usage a través del transporte split.
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/split/central.h>
#include <zmk_claude_usage/claude_usage.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(qolera_claude_usage)

#define CLAUDE_USAGE_BEHAVIOR_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(qolera_claude_usage)
#define CLAUDE_USAGE_BEHAVIOR_NAME DEVICE_DT_NAME(CLAUDE_USAGE_BEHAVIOR_NODE)

#define ZMK_CLAUDE_USAGE_PAYLOAD_LEN 6

#define BT_UUID_CLAUDE_USAGE_SERVICE                                                               \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0xc1a0de00, 0x2b5e, 0x4f0c, 0x9c8a, 0x3f2a1d4e5b6c))
#define BT_UUID_CLAUDE_USAGE_DATA                                                                  \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0xc1a0de01, 0x2b5e, 0x4f0c, 0x9c8a, 0x3f2a1d4e5b6c))

static struct zmk_claude_usage_state pending_state;
static struct k_spinlock pending_lock;

static void relay_work_cb(struct k_work *work) {
    struct zmk_claude_usage_state state;

    k_spinlock_key_t key = k_spin_lock(&pending_lock);
    state = pending_state;
    k_spin_unlock(&pending_lock, key);

#ifdef CONFIG_ZMK_CLAUDE_USAGE_WIDGET
    /* Si esta mitad también tiene pantalla, actualizarla localmente. */
    zmk_claude_usage_widget_update(state);
#endif

    uint32_t param1, param2;
    zmk_claude_usage_pack(&state, &param1, &param2);

    struct zmk_behavior_binding binding = {
        .behavior_dev = CLAUDE_USAGE_BEHAVIOR_NAME,
        .param1 = param1,
        .param2 = param2,
    };
    struct zmk_behavior_binding_event event = {
        .position = 0,
        .timestamp = k_uptime_get(),
    };

    for (uint8_t source = 0; source < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; source++) {
        int err = zmk_split_central_invoke_behavior(source, &binding, event, true);
        if (err < 0) {
            LOG_WRN("No se pudo reenviar el uso al periférico %u (%d)", source, err);
        }
    }
}

static K_WORK_DEFINE(relay_work, relay_work_cb);

static ssize_t write_usage(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                           uint16_t len, uint16_t offset, uint8_t flags) {
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    if (len < ZMK_CLAUDE_USAGE_PAYLOAD_LEN) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const uint8_t *data = buf;

    k_spinlock_key_t key = k_spin_lock(&pending_lock);
    pending_state.session_pct = data[0];
    pending_state.weekly_pct = data[1];
    pending_state.session_reset_min = sys_get_le16(&data[2]);
    pending_state.weekly_reset_min = sys_get_le16(&data[4]);
    k_spin_unlock(&pending_lock, key);

    k_work_submit(&relay_work);

    return len;
}

BT_GATT_SERVICE_DEFINE(
    claude_usage_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_CLAUDE_USAGE_SERVICE),
    BT_GATT_CHARACTERISTIC(BT_UUID_CLAUDE_USAGE_DATA,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_usage, NULL));

#endif /* DT_HAS_COMPAT_STATUS_OKAY(qolera_claude_usage) */
