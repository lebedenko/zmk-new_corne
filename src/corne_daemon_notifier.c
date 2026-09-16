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

static uint8_t hid_tx_buf[ZMK_RAW_REPORT_SIZE];

static void corne_send_layer_state(void) {
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

    LOG_INF("Corne notifier sending layer state: idx=%u, name=%s, state=0x%08x, build=%.8s",
            highest_idx, name, state, report->build_id);

    raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = hid_tx_buf,
        .length = sizeof(hid_tx_buf),
    });
}

static void corne_send_keymap_summary(void) {
    memset(hid_tx_buf, 0, sizeof(hid_tx_buf));
    struct zmk_keymap_summary_report *report = (struct zmk_keymap_summary_report *)hid_tx_buf;
    report->msg_type = CORNE_MSG_KEYMAP_SUMMARY;
    report->layer_count = (uint8_t)ZMK_KEYMAP_LAYERS_LEN;
    report->keys_per_layer = (uint8_t)ZMK_KEYMAP_LEN;
    report->default_layer = (uint8_t)zmk_keymap_layer_default();
    strncpy(report->build_id, CORNE_BUILD_ID, ZMK_BUILD_ID_LEN);

    LOG_INF("Corne notifier sending keymap summary: layers=%u, keys=%u, default=%u, build=%.8s",
            report->layer_count, report->keys_per_layer, report->default_layer, report->build_id);

    raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = hid_tx_buf,
        .length = sizeof(hid_tx_buf),
    });
}

static void corne_send_layer_info(uint8_t layer_idx) {
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

    LOG_INF("Corne notifier sending layer info: idx=%u, id=%u, name=%s, active=%d",
            layer_idx, layer_id, name, active);

    raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = hid_tx_buf,
        .length = sizeof(hid_tx_buf),
    });
}

static void corne_send_layer_binding(uint8_t layer_idx, uint8_t binding_idx) {
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

    if (binding != NULL) {
        if (binding->behavior_dev != NULL) {
            strncpy(report->behavior_name, binding->behavior_dev, ZMK_MAX_BEHAVIOR_NAME_LEN - 1);
        }
        report->param1 = binding->param1;
        report->param2 = binding->param2;
    }

    LOG_DBG("Corne notifier sending binding: layer=%u, pos=%u, behavior=%s, p1=0x%x, p2=0x%x",
            layer_idx, binding_idx, report->behavior_name, report->param1, report->param2);

    raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = hid_tx_buf,
        .length = sizeof(hid_tx_buf),
    });
}

static int layer_state_changed_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    corne_send_layer_state();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(corne_daemon_layer_listener, layer_state_changed_listener);
ZMK_SUBSCRIPTION(corne_daemon_layer_listener, zmk_layer_state_changed);

static uint8_t pending_cmd;
static uint8_t pending_arg1;
static uint8_t pending_arg2;

static void query_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    switch (pending_cmd) {
    case CORNE_CMD_GET_CURRENT_LAYER:
        LOG_DBG("Corne notifier: processing query for current layer");
        corne_send_layer_state();
        break;

    case CORNE_CMD_GET_KEYMAP_SUMMARY:
        LOG_DBG("Corne notifier: processing query for keymap summary");
        corne_send_keymap_summary();
        break;

    case CORNE_CMD_GET_LAYER_INFO:
        LOG_DBG("Corne notifier: processing query for layer info: idx=%u", pending_arg1);
        corne_send_layer_info(pending_arg1);
        break;

    case CORNE_CMD_GET_LAYER_BINDING:
        LOG_DBG("Corne notifier: processing query for layer binding: layer=%u, pos=%u",
                pending_arg1, pending_arg2);
        corne_send_layer_binding(pending_arg1, pending_arg2);
        break;

    default:
        LOG_WRN("Corne notifier: unknown command 0x%02x", pending_cmd);
        break;
    }
}

static K_WORK_DEFINE(query_work, query_work_handler);

static int raw_hid_received_listener(const zmk_event_t *eh) {
    const struct raw_hid_received_event *ev = as_raw_hid_received_event(eh);
    if (ev == NULL || ev->length == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    pending_cmd = ev->data[0];
    pending_arg1 = (ev->length > 1) ? ev->data[1] : 0;
    pending_arg2 = (ev->length > 2) ? ev->data[2] : 0;

    k_work_submit(&query_work);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(corne_daemon_hid_rx_listener, raw_hid_received_listener);
ZMK_SUBSCRIPTION(corne_daemon_hid_rx_listener, raw_hid_received_event);
