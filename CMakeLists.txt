# Copyright (c) 2024 The ZMK Contributors
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_CORNE_DAEMON_NOTIFIER)
  if(NOT CONFIG_RAW_HID)
    message(FATAL_ERROR "ZMK Corne Daemon Notifier requires CONFIG_RAW_HID=y. Add zmk-raw-hid to your west projects list.")
  endif()

  # Determine build ID (8-character hex string)
  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      OUTPUT_VARIABLE CORNE_BUILD_ID_VAL
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
  endif()

  # Fallback: hash keymap file if git short sha is not available
  if(NOT CORNE_BUILD_ID_VAL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/config/eyelash_corne.keymap")
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/config/eyelash_corne.keymap" KEYMAP_HASH)
    string(SUBSTRING "${KEYMAP_HASH}" 0 8 CORNE_BUILD_ID_VAL)
  endif()

  # Fallback: timestamp
  if(NOT CORNE_BUILD_ID_VAL)
    string(TIMESTAMP CORNE_BUILD_ID_VAL "%s")
    string(SUBSTRING "${CORNE_BUILD_ID_VAL}" 0 8 CORNE_BUILD_ID_VAL)
  endif()

  message(STATUS "Corne Daemon Notifier Build ID: ${CORNE_BUILD_ID_VAL}")

  target_sources(app PRIVATE src/corne_daemon_notifier.c)
  target_include_directories(app PRIVATE src)
  target_compile_definitions(app PRIVATE CORNE_BUILD_ID="${CORNE_BUILD_ID_VAL}")
endif()
