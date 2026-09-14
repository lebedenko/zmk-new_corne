# Copyright (c) 2024 The ZMK Contributors
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_CORNE_DAEMON_NOTIFIER)
  if(NOT CONFIG_RAW_HID)
    message(FATAL_ERROR "ZMK Corne Daemon Notifier requires CONFIG_RAW_HID=y. Add zmk-raw-hid to your west projects list.")
  endif()

  target_sources(app PRIVATE src/corne_daemon_notifier.c)
endif()
