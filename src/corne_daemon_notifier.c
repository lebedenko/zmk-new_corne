/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <raw_hid/events.h>

#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdint.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define CORNE_MSG_LAYER_STATE 0x01
#define CORNE_CMD_GET_CURRENT_LAYER 0x01
#define MAX_LAYER_NAME_LEN 16

static uint8_t hid_buf[CONFIG_RAW_HID_REPORT_SIZE];

static void corne_send_layer_state(void) {
    zmk_keymap_layer_index_t highest_idx = zmk_keymap_highest_layer_active();
    zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(highest_idx);
    const char *name = zmk_keymap_layer_name(layer_id);
    if (name == NULL) {
        name = "";
    }

    uint32_t state = zmk_keymap_layer_state();
    uint8_t name_len = (uint8_t)MIN(strlen(name), MAX_LAYER_NAME_LEN);

    memset(hid_buf, 0, sizeof(hid_buf));
    hid_buf[0] = CORNE_MSG_LAYER_STATE;
    hid_buf[1] = highest_idx;
    memcpy(&hid_buf[2], &state, sizeof(uint32_t));
    hid_buf[6] = name_len;
    memcpy(&hid_buf[7], name, name_len);

    LOG_INF("Corne notifier sending layer state: idx=%u, name=%s, state=0x%08x",
            highest_idx, name, state);

    raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = hid_buf,
        .length = sizeof(hid_buf),
    });
}

static int layer_state_changed_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    corne_send_layer_state();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(corne_daemon_layer_listener, layer_state_changed_listener);
ZMK_SUBSCRIPTION(corne_daemon_layer_listener, zmk_layer_state_changed);

static int raw_hid_received_listener(const zmk_event_t *eh) {
    const struct raw_hid_received_event *ev = as_raw_hid_received_event(eh);
    if (ev == NULL || ev->length == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->data[0] == CORNE_CMD_GET_CURRENT_LAYER) {
        LOG_DBG("Corne notifier: received query for current layer");
        corne_send_layer_state();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(corne_daemon_hid_rx_listener, raw_hid_received_listener);
ZMK_SUBSCRIPTION(corne_daemon_hid_rx_listener, raw_hid_received_event);
