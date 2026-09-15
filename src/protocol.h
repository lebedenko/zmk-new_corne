/*
 * ZMK Corne Raw HID Protocol Specification
 * Shared between firmware and host daemon / C++ clients
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ZMK_CORNE_PROTOCOL_H
#define ZMK_CORNE_PROTOCOL_H

#include <stdint.h>

#define ZMK_RAW_REPORT_SIZE 32
#define ZMK_MAX_LAYER_NAME_LEN 16
#define ZMK_BUILD_ID_LEN 8
#define ZMK_MAX_BEHAVIOR_NAME_LEN 12

/* Message types from Keyboard -> Host */
#define CORNE_MSG_LAYER_STATE        0x01
#define CORNE_MSG_KEYMAP_SUMMARY     0x02
#define CORNE_MSG_LAYER_INFO         0x03
#define CORNE_MSG_LAYER_BINDING      0x04

/* Command types from Host -> Keyboard */
#define CORNE_CMD_GET_CURRENT_LAYER  0x01
#define CORNE_CMD_GET_KEYMAP_SUMMARY 0x02
#define CORNE_CMD_GET_LAYER_INFO      0x03
#define CORNE_CMD_GET_LAYER_BINDING   0x04

#pragma pack(push, 1)

/**
 * @brief Report sent by keyboard on layer change or in response to GET_CURRENT_LAYER
 */
struct zmk_layer_state_report {
    uint8_t  msg_type;                     /* CORNE_MSG_LAYER_STATE (0x01) */
    uint8_t  layer_index;                  /* Highest active layer index (0, 1, 2...) */
    uint32_t layer_state;                  /* 32-bit active layer bitmask */
    uint8_t  name_len;                     /* Length of ASCII layer name */
    char     name[ZMK_MAX_LAYER_NAME_LEN]; /* Null-terminated ASCII name */
    char     build_id[ZMK_BUILD_ID_LEN];   /* Firmware build ID (git short SHA or keymap hash) */
    uint8_t  reserved[1];                  /* Padding for 32-byte report size */
};

/**
 * @brief Command sent by host to query current active layer
 */
struct zmk_get_layer_cmd {
    uint8_t  cmd_type;                     /* CORNE_CMD_GET_CURRENT_LAYER (0x01) */
    uint8_t  reserved[31];
};

/**
 * @brief Command sent by host to query keymap summary
 */
struct zmk_get_keymap_summary_cmd {
    uint8_t  cmd_type;                     /* CORNE_CMD_GET_KEYMAP_SUMMARY (0x02) */
    uint8_t  reserved[31];
};

/**
 * @brief Report sent by keyboard with total layers and keys per layer
 */
struct zmk_keymap_summary_report {
    uint8_t  msg_type;                     /* CORNE_MSG_KEYMAP_SUMMARY (0x02) */
    uint8_t  layer_count;                  /* Total number of layers (e.g. 6) */
    uint8_t  keys_per_layer;               /* Number of keys per layer (e.g. 48) */
    uint8_t  default_layer;                /* Default layer ID (usually 0) */
    char     build_id[ZMK_BUILD_ID_LEN];   /* Firmware build ID */
    uint8_t  reserved[20];
};

/**
 * @brief Command sent by host to query a specific layer's info
 */
struct zmk_get_layer_info_cmd {
    uint8_t  cmd_type;                     /* CORNE_CMD_GET_LAYER_INFO (0x03) */
    uint8_t  layer_index;                  /* Layer index (0..layer_count-1) */
    uint8_t  reserved[30];
};

/**
 * @brief Report sent by keyboard with a single layer's metadata
 */
struct zmk_layer_info_report {
    uint8_t  msg_type;                     /* CORNE_MSG_LAYER_INFO (0x03) */
    uint8_t  layer_index;                  /* Layer index (0, 1, 2...) */
    uint8_t  layer_id;                     /* Internal ZMK layer ID */
    uint8_t  name_len;                     /* Length of ASCII name */
    char     name[ZMK_MAX_LAYER_NAME_LEN]; /* Null-terminated ASCII name */
    uint8_t  is_active;                    /* 1 if layer currently active, 0 otherwise */
    uint8_t  reserved[11];
};

/**
 * @brief Command sent by host to query a specific key binding on a layer
 */
struct zmk_get_layer_binding_cmd {
    uint8_t  cmd_type;                     /* CORNE_CMD_GET_LAYER_BINDING (0x04) */
    uint8_t  layer_index;                  /* Layer index */
    uint8_t  binding_index;                /* Key position (0..keys_per_layer-1) */
    uint8_t  reserved[29];
};

/**
 * @brief Report sent by keyboard with behavior binding details for a single key position
 */
struct zmk_layer_binding_report {
    uint8_t  msg_type;                     /* CORNE_MSG_LAYER_BINDING (0x04) */
    uint8_t  layer_index;                  /* Layer index */
    uint8_t  binding_index;                /* Key position */
    char     behavior_name[ZMK_MAX_BEHAVIOR_NAME_LEN]; /* Null-terminated behavior name (e.g. "kp", "hml") */
    uint32_t param1;                       /* Binding parameter 1 (keycode / layer) */
    uint32_t param2;                       /* Binding parameter 2 (second keycode / modifier) */
    uint8_t  reserved[9];
};

#pragma pack(pop)

#endif /* ZMK_CORNE_PROTOCOL_H */
