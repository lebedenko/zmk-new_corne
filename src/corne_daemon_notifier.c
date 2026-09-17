/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_keymap

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/behavior.h>
#include <zmk/matrix.h>
#include <zmk/sensors.h>
#include <raw_hid/events.h>

#include "protocol.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#ifndef CORNE_BUILD_ID
#define CORNE_BUILD_ID "unknown0"
#endif

BUILD_ASSERT(sizeof(struct zmk_layer_state_report) == ZMK_RAW_REPORT_SIZE, "zmk_layer_state_report size mismatch");
BUILD_ASSERT(sizeof(struct zmk_keymap_summary_report) == ZMK_RAW_REPORT_SIZE, "zmk_keymap_summary_report size mismatch");
BUILD_ASSERT(sizeof(struct zmk_layer_info_report) == ZMK_RAW_REPORT_SIZE, "zmk_layer_info_report size mismatch");
BUILD_ASSERT(sizeof(struct zmk_layer_binding_report) == ZMK_RAW_REPORT_SIZE, "zmk_layer_binding_report size mismatch");
BUILD_ASSERT(sizeof(struct zmk_sensor_binding_report) == ZMK_RAW_REPORT_SIZE, "zmk_sensor_binding_report size mismatch");
BUILD_ASSERT(sizeof(struct zmk_set_layer_cmd) == ZMK_RAW_REPORT_SIZE, "zmk_set_layer_cmd size mismatch");
BUILD_ASSERT(sizeof(struct zmk_clear_layer_cmd) == ZMK_RAW_REPORT_SIZE, "zmk_clear_layer_cmd size mismatch");
BUILD_ASSERT(sizeof(struct zmk_cmd_ack_report) == ZMK_RAW_REPORT_SIZE, "zmk_cmd_ack_report size mismatch");

#if ZMK_KEYMAP_HAS_SENSORS
#define _CORNE_TRANSFORM_SENSOR_ENTRY(idx, layer)                                                  \
    {                                                                                              \
        .behavior_dev = DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(layer, sensor_bindings, idx)),            \
        .param1 = COND_CODE_0(DT_PHA_HAS_CELL_AT_IDX(layer, sensor_bindings, idx, param1), (0),    \
                              (DT_PHA_BY_IDX(layer, sensor_bindings, idx, param1))),               \
        .param2 = COND_CODE_0(DT_PHA_HAS_CELL_AT_IDX(layer, sensor_bindings, idx, param2), (0),    \
                              (DT_PHA_BY_IDX(layer, sensor_bindings, idx, param2))),               \
    }

#define CORNE_SENSOR_LAYER(node)                                                                   \
    COND_CODE_1(                                                                                   \
        DT_NODE_HAS_PROP(node, sensor_bindings),                                                   \
        ({LISTIFY(DT_PROP_LEN(node, sensor_bindings), _CORNE_TRANSFORM_SENSOR_ENTRY, (, ), node)}), \
        ({}))

static const struct zmk_behavior_binding
    corne_sensor_keymap[ZMK_KEYMAP_LAYERS_LEN][ZMK_KEYMAP_SENSORS_LEN] = {
        DT_INST_FOREACH_CHILD_SEP(0, CORNE_SENSOR_LAYER, (, ))};
#endif

static uint8_t hid_tx_buf[ZMK_RAW_REPORT_SIZE];

/* Active context layer & lease timer state */
static zmk_keymap_layer_id_t active_context_layer_id = ZMK_KEYMAP_LAYER_ID_INVAL;

static void context_lease_timer_handler(struct k_timer *timer);
static K_TIMER_DEFINE(context_lease_timer, context_lease_timer_handler, NULL);

static void context_lease_timer_handler(struct k_timer *timer) {
    ARG_UNUSED(timer);
    if (active_context_layer_id != ZMK_KEYMAP_LAYER_ID_INVAL) {
        LOG_INF("Corne notifier: context layer id=%u lease expired, deactivating",
                active_context_layer_id);
        zmk_keymap_layer_deactivate(active_context_layer_id);
        active_context_layer_id = ZMK_KEYMAP_LAYER_ID_INVAL;
    }
}

static void corne_send_ack(uint8_t cmd, uint8_t status, uint8_t seq) {
    memset(hid_tx_buf, 0, sizeof(hid_tx_buf));
    struct zmk_cmd_ack_report *report = (struct zmk_cmd_ack_report *)hid_tx_buf;
    report->msg_type = CORNE_MSG_CMD_ACK;
    report->cmd_type = cmd;
    report->status = status;
    report->seq = seq;

    raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = hid_tx_buf,
        .length = sizeof(hid_tx_buf),
    });
}

static void corne_send_layer_state(uint8_t seq) {
    zmk_keymap_layer_index_t highest_idx = zmk_keymap_highest_layer_active();
    zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(highest_idx);
    const char *name = zmk_keymap_layer_name(layer_id);
    if (name == NULL) {
        name = "";
    }

    uint32_t state = zmk_keymap_layer_state();
    uint8_t name_len = (uint8_t)MIN(strlen(name), ZMK_MAX_LAYER_NAME_LEN);

    memset(hid_tx_buf, 0, sizeof(hid_tx_buf));
    struct zmk_layer_state_report *report = (struct zmk_layer_state_report *)hid_tx_buf;
    report->msg_type = CORNE_MSG_LAYER_STATE;
    report->layer_index = highest_idx;
    report->layer_state = state;
    report->name_len = name_len;
    memcpy(report->name, name, name_len);
    strncpy(report->build_id, CORNE_BUILD_ID, ZMK_BUILD_ID_LEN);
    report->seq = seq;

    LOG_INF("Corne notifier sending layer state: idx=%u, name=%s, state=0x%08x, seq=%u, build=%.8s",
            highest_idx, name, state, seq, report->build_id);

    raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = hid_tx_buf,
        .length = sizeof(hid_tx_buf),
    });
}

static void corne_send_keymap_summary(uint8_t seq) {
    memset(hid_tx_buf, 0, sizeof(hid_tx_buf));
    struct zmk_keymap_summary_report *report = (struct zmk_keymap_summary_report *)hid_tx_buf;
    report->msg_type = CORNE_MSG_KEYMAP_SUMMARY;
    report->layer_count = (uint8_t)ZMK_KEYMAP_LAYERS_LEN;
    report->keys_per_layer = (uint8_t)ZMK_KEYMAP_LEN;
    report->default_layer = (uint8_t)zmk_keymap_layer_default();
    report->sensors_per_layer =
#if ZMK_KEYMAP_HAS_SENSORS
        (uint8_t)ZMK_KEYMAP_SENSORS_LEN;
#else
        0;
#endif
    strncpy(report->build_id, CORNE_BUILD_ID, ZMK_BUILD_ID_LEN);
    report->seq = seq;

    LOG_INF("Corne notifier sending keymap summary: layers=%u, keys=%u, sensors=%u, default=%u, seq=%u, build=%.8s",
            report->layer_count, report->keys_per_layer, report->sensors_per_layer,
            report->default_layer, seq, report->build_id);

    raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = hid_tx_buf,
        .length = sizeof(hid_tx_buf),
    });
}

static void corne_send_layer_info(uint8_t layer_idx, uint8_t seq) {
    if (layer_idx >= ZMK_KEYMAP_LAYERS_LEN) {
        LOG_WRN("Corne notifier: requested invalid layer index %u (max %u)",
                layer_idx, ZMK_KEYMAP_LAYERS_LEN - 1);
        return;
    }

    zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(layer_idx);
    const char *name = zmk_keymap_layer_name(layer_id);
    if (name == NULL) {
        name = "";
    }
    uint8_t name_len = (uint8_t)MIN(strlen(name), ZMK_MAX_LAYER_NAME_LEN);
    bool active = zmk_keymap_layer_active(layer_id);

    memset(hid_tx_buf, 0, sizeof(hid_tx_buf));
    struct zmk_layer_info_report *report = (struct zmk_layer_info_report *)hid_tx_buf;
    report->msg_type = CORNE_MSG_LAYER_INFO;
    report->layer_index = layer_idx;
    report->layer_id = layer_id;
    report->name_len = name_len;
    memcpy(report->name, name, name_len);
    report->is_active = active ? 1 : 0;
    report->seq = seq;

    LOG_INF("Corne notifier sending layer info: idx=%u, id=%u, name=%s, active=%d, seq=%u",
            layer_idx, layer_id, name, active, seq);

    raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = hid_tx_buf,
        .length = sizeof(hid_tx_buf),
    });
}

static void corne_send_layer_binding(uint8_t layer_idx, uint8_t binding_idx, uint8_t seq) {
    if (layer_idx >= ZMK_KEYMAP_LAYERS_LEN || binding_idx >= ZMK_KEYMAP_LEN) {
        LOG_WRN("Corne notifier: requested invalid binding idx=%u on layer=%u",
                binding_idx, layer_idx);
        return;
    }

    zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(layer_idx);
    const struct zmk_behavior_binding *binding =
        zmk_keymap_get_layer_binding_at_idx(layer_id, binding_idx);

    memset(hid_tx_buf, 0, sizeof(hid_tx_buf));
    struct zmk_layer_binding_report *report = (struct zmk_layer_binding_report *)hid_tx_buf;
    report->msg_type = CORNE_MSG_LAYER_BINDING;
    report->layer_index = layer_idx;
    report->binding_index = binding_idx;
    report->seq = seq;

    if (binding != NULL) {
        if (binding->behavior_dev != NULL) {
            strncpy(report->behavior_name, binding->behavior_dev, ZMK_MAX_BEHAVIOR_NAME_LEN - 1);
        }
        report->param1 = binding->param1;
        report->param2 = binding->param2;
    }

    LOG_DBG("Corne notifier sending binding: layer=%u, pos=%u, behavior=%s, p1=0x%x, p2=0x%x, seq=%u",
            layer_idx, binding_idx, report->behavior_name, report->param1, report->param2, seq);

    raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = hid_tx_buf,
        .length = sizeof(hid_tx_buf),
    });
}

static void corne_send_sensor_binding(uint8_t layer_idx, uint8_t sensor_idx, uint8_t seq) {
#if ZMK_KEYMAP_HAS_SENSORS
    if (layer_idx >= ZMK_KEYMAP_LAYERS_LEN || sensor_idx >= ZMK_KEYMAP_SENSORS_LEN) {
        LOG_WRN("Corne notifier: requested invalid sensor idx=%u on layer=%u",
                sensor_idx, layer_idx);
        return;
    }

    const struct zmk_behavior_binding *binding = &corne_sensor_keymap[layer_idx][sensor_idx];

    memset(hid_tx_buf, 0, sizeof(hid_tx_buf));
    struct zmk_sensor_binding_report *report = (struct zmk_sensor_binding_report *)hid_tx_buf;
    report->msg_type = CORNE_MSG_SENSOR_BINDING;
    report->layer_index = layer_idx;
    report->sensor_index = sensor_idx;
    report->seq = seq;

    if (binding != NULL) {
        if (binding->behavior_dev != NULL) {
            strncpy(report->behavior_name, binding->behavior_dev, ZMK_MAX_BEHAVIOR_NAME_LEN - 1);
        }
        report->param1 = binding->param1;
        report->param2 = binding->param2;
    }

    LOG_DBG("Corne notifier sending sensor binding: layer=%u, sensor=%u, behavior=%s, p1=0x%x, p2=0x%x, seq=%u",
            layer_idx, sensor_idx, report->behavior_name, report->param1, report->param2, seq);

    raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = hid_tx_buf,
        .length = sizeof(hid_tx_buf),
    });
#else
    ARG_UNUSED(layer_idx);
    ARG_UNUSED(sensor_idx);
    ARG_UNUSED(seq);
    LOG_WRN("Corne notifier: sensors not enabled in build");
#endif
}

static void corne_handle_set_layer(uint8_t layer_idx, bool lock, uint16_t lease_ms, uint8_t seq) {
    if (layer_idx >= ZMK_KEYMAP_LAYERS_LEN) {
        LOG_WRN("Corne notifier: set layer invalid index=%u", layer_idx);
        corne_send_ack(CORNE_CMD_SET_LAYER, 1, seq);
        return;
    }

    zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(layer_idx);

    if (lock) {
        if (active_context_layer_id != ZMK_KEYMAP_LAYER_ID_INVAL &&
            active_context_layer_id != layer_id) {
            zmk_keymap_layer_deactivate(active_context_layer_id);
        }

        active_context_layer_id = layer_id;
        zmk_keymap_layer_activate(layer_id);

        if (lease_ms != 0xFFFF) {
            k_timer_start(&context_lease_timer, K_MSEC(lease_ms), K_NO_WAIT);
        } else {
            k_timer_stop(&context_lease_timer);
        }
    } else {
        if (active_context_layer_id == layer_id) {
            k_timer_stop(&context_lease_timer);
            active_context_layer_id = ZMK_KEYMAP_LAYER_ID_INVAL;
        }
        zmk_keymap_layer_deactivate(layer_id);
    }

    corne_send_ack(CORNE_CMD_SET_LAYER, 0, seq);
}

static void corne_handle_clear_layer(uint8_t seq) {
    k_timer_stop(&context_lease_timer);
    if (active_context_layer_id != ZMK_KEYMAP_LAYER_ID_INVAL) {
        zmk_keymap_layer_deactivate(active_context_layer_id);
        active_context_layer_id = ZMK_KEYMAP_LAYER_ID_INVAL;
    }
    corne_send_ack(CORNE_CMD_CLEAR_LAYER, 0, seq);
}

static int layer_state_changed_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    corne_send_layer_state(0);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(corne_daemon_layer_listener, layer_state_changed_listener);
ZMK_SUBSCRIPTION(corne_daemon_layer_listener, zmk_layer_state_changed);

/* Message queue for incoming raw HID queries to prevent drop/race under burst */
struct corne_query_msg {
    uint8_t cmd;
    uint8_t arg1;
    uint8_t arg2;
    uint8_t seq;
    uint16_t lease_ms;
};

K_MSGQ_DEFINE(corne_query_msgq, sizeof(struct corne_query_msg), 32, 4);

static void query_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    struct corne_query_msg msg;
    while (k_msgq_get(&corne_query_msgq, &msg, K_NO_WAIT) == 0) {
        switch (msg.cmd) {
        case CORNE_CMD_GET_CURRENT_LAYER:
            LOG_DBG("Corne notifier: processing query for current layer (seq=%u)", msg.seq);
            corne_send_layer_state(msg.seq);
            break;

        case CORNE_CMD_GET_KEYMAP_SUMMARY:
            LOG_DBG("Corne notifier: processing query for keymap summary (seq=%u)", msg.seq);
            corne_send_keymap_summary(msg.seq);
            break;

        case CORNE_CMD_GET_LAYER_INFO:
            LOG_DBG("Corne notifier: processing query for layer info: idx=%u (seq=%u)", msg.arg1, msg.seq);
            corne_send_layer_info(msg.arg1, msg.seq);
            break;

        case CORNE_CMD_GET_LAYER_BINDING:
            LOG_DBG("Corne notifier: processing query for layer binding: layer=%u, pos=%u (seq=%u)",
                    msg.arg1, msg.arg2, msg.seq);
            corne_send_layer_binding(msg.arg1, msg.arg2, msg.seq);
            break;

        case CORNE_CMD_GET_SENSOR_BINDING:
            LOG_DBG("Corne notifier: processing query for sensor binding: layer=%u, sensor=%u (seq=%u)",
                    msg.arg1, msg.arg2, msg.seq);
            corne_send_sensor_binding(msg.arg1, msg.arg2, msg.seq);
            break;

        case CORNE_CMD_SET_LAYER:
            LOG_INF("Corne notifier: set layer request: layer=%u, lock=%u, lease=%ums (seq=%u)",
                    msg.arg1, msg.arg2, msg.lease_ms, msg.seq);
            corne_handle_set_layer(msg.arg1, msg.arg2 != 0, msg.lease_ms, msg.seq);
            break;

        case CORNE_CMD_CLEAR_LAYER:
            LOG_INF("Corne notifier: clear layer request (seq=%u)", msg.seq);
            corne_handle_clear_layer(msg.seq);
            break;

        default:
            LOG_WRN("Corne notifier: unknown command 0x%02x", msg.cmd);
            break;
        }
    }
}

static K_WORK_DEFINE(query_work, query_work_handler);

static int raw_hid_received_listener(const zmk_event_t *eh) {
    const struct raw_hid_received_event *ev = as_raw_hid_received_event(eh);
    if (ev == NULL || ev->length == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const uint8_t *data = ev->data;
    uint8_t len = ev->length;

    /* Handle unstripped dummy report ID (0x00) sent by Linux BLE HoG unnumbered reports */
    if (len > 1 && data[0] == 0x00) {
        data++;
        len--;
    }

    struct corne_query_msg msg = {0};
    msg.cmd = data[0];

    if (msg.cmd == CORNE_CMD_SET_LAYER) {
        msg.arg1 = (len > 1) ? data[1] : 0;
        msg.arg2 = (len > 2) ? data[2] : 1;
        msg.seq  = (len > 3) ? data[3] : 0;
        msg.lease_ms = (len > 5) ? ((uint16_t)data[4] | ((uint16_t)data[5] << 8)) : 5000;
        if (msg.lease_ms == 0) {
            msg.lease_ms = 5000;
        }
    } else if (msg.cmd == CORNE_CMD_CLEAR_LAYER) {
        msg.seq  = (len > 1) ? data[1] : 0;
    } else if (msg.cmd == CORNE_CMD_GET_CURRENT_LAYER || msg.cmd == CORNE_CMD_GET_KEYMAP_SUMMARY) {
        msg.seq  = (len > 1) ? data[1] : 0;
    } else if (msg.cmd == CORNE_CMD_GET_LAYER_INFO) {
        msg.arg1 = (len > 1) ? data[1] : 0;
        msg.seq  = (len > 2) ? data[2] : 0;
    } else if (msg.cmd == CORNE_CMD_GET_LAYER_BINDING || msg.cmd == CORNE_CMD_GET_SENSOR_BINDING) {
        msg.arg1 = (len > 1) ? data[1] : 0;
        msg.arg2 = (len > 2) ? data[2] : 0;
        msg.seq  = (len > 3) ? data[3] : 0;
    }

    int ret = k_msgq_put(&corne_query_msgq, &msg, K_NO_WAIT);
    if (ret != 0) {
        LOG_WRN("Corne notifier: query message queue full (ret=%d)", ret);
    }

    k_work_submit(&query_work);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(corne_daemon_hid_rx_listener, raw_hid_received_listener);
ZMK_SUBSCRIPTION(corne_daemon_hid_rx_listener, raw_hid_received_event);
