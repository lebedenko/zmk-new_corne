# Architecture & Implementation Plan: C++23 Daemon & Qt6/QML Desktop Companion

This plan outlines the architecture, component design, and delivery roadmap for replacing the Python Proof of Concept with a production-grade **C++23 systemd daemon** and a modern **Wayland-native Qt 6 / QML companion application**.

---

## 1. Project Naming & Identity

The project needs distinct, memorable names for both repositories and their respective binaries:

### Recommended Name: **Strata**
> [!TIP]
> **Rationale**: *"Strata"* (Latin plural for layers / geological beds) is a metaphor for layer-centric ergonomic split keyboards. It is short, modern, easy to type, and has zero name collisions in package repositories.

| Entity | Recommended Name | Alternative A (ErgoDeck) | Alternative B (LayerShift) |
|---|---|---|---|
| **Daemon Repository** | `strata-daemon` | `ergodeck-daemon` | `layershift-daemon` |
| **Daemon Binary** | `stratad` | `ergodeckd` | `layershiftd` |
| **CLI Tool Binary** | `strata-ctl` | `ergodeck-ctl` | `layershift-ctl` |
| **Client Repository** | `strata-app` | `ergodeck-app` | `layershift-app` |
| **Client Binary** | `strata` (or `strata-ui`) | `ergodeck` | `layershift` |
| **D-Bus Service Name** | `org.freedesktop.Strata` | `org.ergodeck.Daemon` | `org.layershift.Daemon` |
| **Systemd Service** | `stratad.service` | `ergodeckd.service` | `layershiftd.service` |
| **Config / Cache Dirs** | `~/.config/strata/`, `~/.cache/strata/` | `~/.config/ergodeck/` | `~/.config/layershift/` |

---

## 2. IPC Decision: D-Bus vs. Unix Domain Socket

### Comparative Evaluation

| Criteria | Unix Domain Socket (NDJSON) | D-Bus (Session Bus) |
|---|---|---|
| **Wayland & Linux Desktop Integration** | Poor. Requires custom client-side watchdog and connection loops. | **Native**. Desktop components (Qt, KDE, GNOME, Waybar, Hyprland) are built around D-Bus. |
| **Qt 6 / QML Integration** | Requires writing custom POSIX socket threads, JSON framing, and QML bridges. | **First-class**. `Qt6DBus` + `qdbusxml2cpp` auto-generates type-safe C++ proxies. Binding D-Bus signals to QML properties requires almost zero boilerplate. |
| **Shell & Scripting Usability** | Needs `nc -U`, `socat`, and `jq`. | Standard utilities: `busctl`, `gdbus`, `qdbus`. Users can query properties or call methods in one command. |
| **Systemd Lifecycle & Activation** | Manual service start; requires socket activation unit (`.socket`). | **Native D-Bus activation**: if daemon is stopped, launching the UI or calling the CLI starts the service on demand. |
| **Introspection & Schema** | Custom documentation required; no runtime self-discovery. | **Full introspection**: `busctl introspect` exposes every method, argument, property, and signal dynamically. |
| **High Frequency Events** | Fast for hundreds of events/sec. | Plenty fast for layer switches (1–5 per second max). |

### What is `qdbusxml2cpp` and Why is it Used?

`qdbusxml2cpp` is an official build-time code generator provided with Qt. It reads a D-Bus interface XML definition and generates type-safe C++ wrapper classes:

1. **Compile-Time Type Safety vs. Stringly-Typed APIs**:
   * *Without `qdbusxml2cpp` (Manual `QDBusInterface`)*:
     Method names and signals are raw strings: `iface.call("GetStatus")` or `connect(..., "LayerChanged", ...)`. A typo in the string or argument type compiles without warning and silently fails at runtime.
   * *With `qdbusxml2cpp` (via CMake `qt_add_dbus_interface`)*:
     Generates a concrete C++ class (`OrgFreedesktopStrataDevice1Interface`). Methods are real C++ member functions: `device->GetLayers()`, and signals are real C++ PMFs (`&Interface::LayerChanged`). Any type or naming discrepancy is caught at compile time by GCC/Clang.
2. **Single Source of Truth**:
   The `.xml` contract file defines the D-Bus interface once. Both client proxies (`qdbusxml2cpp -p`) and daemon server adaptors (`qdbusxml2cpp -a`) can be generated from the exact same file, guaranteeing the daemon and client never desynchronize.
3. **Eliminates Serialization Boilerplate**:
   Complex signatures (e.g. `a(ussb)` array of structs for layers and `a(ussuu)` for key bindings) are automatically marshaled into Qt types (`QList<LayerInfo>`) without writing repetitive `QDBusArgument` streaming operators.
4. **Zero Runtime Overhead**:
   `qdbusxml2cpp` runs strictly at build time inside CMake. It produces standard C++ files compiled into your binary.
5. **Optional**:
   If preferred, a developer can write a thin manual wrapper using `QDBusInterface` instead without code generation. Using `qdbusxml2cpp` is simply idiomatic Qt best practice.

> [!IMPORTANT]
> **Recommendation: D-Bus Session Bus (`org.freedesktop.Strata`)**
> D-Bus is the superior architecture for this system. It eliminates custom networking code in Qt/QML, provides automatic systemd service activation, and allows tools like Waybar, Hyprland scripts, and the CLI to observe layer changes with standard tools like `busctl --user monitor org.freedesktop.Strata`.

---

## 3. System Architecture & Component Diagram

```mermaid
flowchart TD
    subgraph Hardware ["Hardware Layer (USB & BLE)"]
        CORNE["Eyelash Corne (ZMK BLE/USB)"]
        VOYAGER["ZSA Voyager (QMK USB)"]
    end

    subgraph Kernel ["Linux Kernel Subsystem"]
        HIDRAW["/dev/hidraw* Nodes"]
        UDEV["udev Subsystem"]
        CORNE --> HIDRAW
        VOYAGER --> HIDRAW
    end

    subgraph DaemonRepo ["Daemon Repo: stratad & strata-ctl"]
        subgraph DeviceLayer ["Device Abstraction Layer"]
            DEV_MGR["DeviceManager (libudev monitor)"]
            IDEV["<< interface >>\nIDevice"]
            ZMK_DEV["ZmkRawHidDevice\n(Corne / ZMK BLE & USB)"]
            QMK_DEV["QmkVoyagerDevice\n(ZSA Voyager QMK Raw HID)"]
            
            DEV_MGR -->|Instantiates| IDEV
            IDEV <|-- ZMK_DEV
            IDEV <|-- QMK_DEV
            HIDRAW <-->|POSIX I/O| IDEV
            UDEV -->|Device Events| DEV_MGR
        end

        subgraph CoreDaemon ["Core Daemon Engine"]
            CACHE["KeymapCache (nlohmann/json)"]
            LOG["LogEngine (journald / console)"]
            DBUS_SRV["DBusService (sd-bus / sdbus-c++)"]
            
            IDEV --> CACHE
            IDEV --> LOG
            IDEV --> DBUS_SRV
        end

        CLI["strata-ctl (CLI Client)"]
        CLI <-->|D-Bus Calls / Signals| DBUS_SRV
    end

    subgraph ClientRepo ["Client Repo: strata (Qt6 / QML)"]
        DBUS_CLIENT["DBusConnector (QDBus)"]
        CFG["ConfigManager (tomlplusplus)"]
        SNI["TrayManager (StatusNotifierItem)"]
        NOTIF["NotificationManager (Desktop Notif)"]
        KEYMAP_MODEL["KeymapVisualizerModel"]
        QML_VIEW["QML Wayland View\n(Fusion / Native Theme)"]

        DBUS_SRV <==>|Session Bus: org.freedesktop.Strata| DBUS_CLIENT
        DBUS_CLIENT --> KEYMAP_MODEL
        DBUS_CLIENT --> SNI
        DBUS_CLIENT --> NOTIF
        CFG --> QML_VIEW
        KEYMAP_MODEL --> QML_VIEW
    end
```

---

## 4. Daemon Repository Design (`strata-daemon`)

### 4.1 Toolchain & Build Standards
* **Standard**: C++23 (`-std=c++23`)
* **Build System**: CMake $\ge 3.28$ + Ninja generator
* **Compilers**: GCC $\ge 14$ and Clang $\ge 18$
* **Formatting & Linting**: `.clang-format` (LLVM style, 100 col), `.clang-tidy` (modernize, readability, bugprone, performance)
* **Automation**: `Taskfile.yml`
* **CI/CD**: GitHub Actions matrix workflow (GCC + Clang, clang-tidy, unit tests)

### 4.2 Key Abstractions (Designed for ZMK + QMK Voyager)

#### Generic Device Interface (`src/device/i_device.hpp`)
```cpp
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct LayerInfo {
    uint8_t index;
    uint8_t id;
    std::string name;
    bool isActive;
};

struct KeyBinding {
    uint8_t pos;
    std::string behavior;
    uint32_t param1;
    uint32_t param2;
};

struct KeymapSummary {
    uint8_t layerCount;
    uint8_t keysPerLayer;
    uint8_t defaultLayer;
    std::string buildId;
};

enum class DeviceType { ZmkRawHid, QmkVoyager };

class IDevice {
public:
    virtual ~IDevice() = default;

    virtual DeviceType type() const = 0;
    virtual std::string name() const = 0;
    virtual std::string deviceNode() const = 0;
    virtual std::string buildId() const = 0;
    virtual bool isOpen() const = 0;

    virtual bool open() = 0;
    virtual void close() = 0;
    virtual void poll(std::chrono::milliseconds timeout) = 0;

    // Async query operations
    virtual void queryCurrentLayer() = 0;
    virtual void queryKeymapSummary() = 0;
    virtual void queryLayerInfo(uint8_t layerIndex) = 0;
    virtual void queryLayerBinding(uint8_t layerIndex, uint8_t keyPosition) = 0;

    // Callbacks for incoming hardware events
    using LayerStateCb = std::function<void(uint8_t index, uint32_t mask, std::string name, std::string buildId)>;
    using SummaryCb = std::function<void(const KeymapSummary&)>;
    using LayerInfoCb = std::function<void(const LayerInfo&)>;
    using BindingCb = std::function<void(uint8_t layer, const KeyBinding&)>;
    using DisconnectCb = std::function<void()>;

    virtual void setOnLayerState(LayerStateCb cb) = 0;
    virtual void setOnSummary(SummaryCb cb) = 0;
    virtual void setOnLayerInfo(LayerInfoCb cb) = 0;
    virtual void setOnBinding(BindingCb cb) = 0;
    virtual void setOnDisconnect(DisconnectCb cb) = 0;
};
```

#### Device Implementations:
1. **`ZmkRawHidDevice`**:
   - Matches Eyelash Corne (`VID: 0x1D50`, `PID: 0x615E`, Usage Page `0xFF60`, Usage `0x61`).
   - Handles the Linux BLE HoG unnumbered report quirk: writes 33 bytes with leading `0x00` dummy report ID over `/dev/hidraw`.
   - Parses 32-byte packed binary structs defined in `protocol.h`.
2. **`QmkVoyagerDevice` (Future-proofed)**:
   - Matches ZSA Voyager (`VID: 0x3297`, `PID: 0x1977`, Raw HID usage page `0xFF60`, usage `0x61`).
   - Implements QMK raw HID query/response protocol (32-byte packets with command IDs for layer state and keymap matrix).

#### Device Manager (`src/device/device_manager.hpp`)
- Uses `libudev` (`udev_monitor_new_from_netlink`, `udev_monitor_filter_add_match_subsystem_devtype(..., "hidraw", nullptr)`) to listen for kernel device insertion and removal.
- Auto-detects device type based on VID/PID and report descriptor usage page.
- Emits device attachment/detachment events to the daemon core.

### 4.3 Logging Architecture (Systemd / Journald vs. Terminal)

#### Strategy:
- **Terminal Execution** (`isatty(STDERR_FILENO)` is true):
  - Colored, human-readable terminal output (timestamps, colored log levels).
- **Systemd Service Execution** (`INVOCATION_ID` or `JOURNAL_STREAM` environment variable set):
  - Direct output to `sd_journal_print` or stdout formatted with systemd priority prefixes:
    - `<3>` Error
    - `<4>` Warning
    - `<6>` Info
    - `<7>` Debug
- **Log Level Filtering (`STRATAD_LOG_LEVEL` environment variable)**:
  - **Normal Mode (`INFO` / default)**:
    - Logs: daemon startup, device opened/connected, keymap cache hit/miss, summary info, graceful shutdown.
    - Suppresses: high-frequency live updates (e.g. `Layer update: ...` on every key press).
  - **Debug Mode (`DEBUG`)**:
    - Logs: every incoming/outgoing raw report byte hex dump, work queue events, individual binding responses, client connections.

### 4.4 D-Bus Specification (`org.freedesktop.Strata`)

Object Path: `/org/freedesktop/Strata/Device0`  
Interface: `org.freedesktop.Strata.Device1`

#### Methods:
* `GetStatus() -> (b: connected, s: device_name, s: node, s: build_id, u: active_layer, s: layer_name, u: state_mask)`
* `GetLayers() -> a(ussb)` — array of `(index, id, name, is_active)`
* `GetKeymap(u: layer) -> a(ussuu)` — array of `(pos, behavior, param1, param2)`
* `RefreshKeymap(u: layer) -> b: success`
* `ClearCache() -> u: evicted_count`

#### Properties:
* `Connected (b)`: Read-only
* `DeviceName (s)`: Read-only
* `ActiveLayerIndex (u)`: Read-only
* `ActiveLayerName (s)`: Read-only
* `BuildId (s)`: Read-only

#### Signals:
* `LayerChanged(u: layer_index, s: layer_name, u: state_mask, s: build_id)`
* `DeviceConnected(s: device_name, s: node, s: build_id)`
* `DeviceDisconnected(s: node)`
* `KeymapLoaded(s: build_id, s: source, u: layer_count)`
* `LayerBindingsLoaded(u: layer, u: key_count)`

### 4.5 Daemon Repo File Hierarchy
```text
strata-daemon/
├── CMakeLists.txt
├── Taskfile.yml
├── .clang-format
├── .clang-tidy
├── .github/
│   └── workflows/
│       └── ci.yml
├── cmake/
│   ├── CompilerWarnings.cmake
│   └── Sanitizers.cmake
├── systemd/
│   ├── stratad.service            # systemd --user service
│   └── org.freedesktop.Strata.service # D-Bus activation definition
├── udev/
│   └── 99-strata-keyboards.rules
├── src/
│   ├── common/
│   │   ├── types.hpp
│   │   ├── protocol_zmk.hpp       # Binary packed struct definitions
│   │   └── logger.hpp             # Journald & TTY logging abstraction
│   ├── device/
│   │   ├── i_device.hpp           # Device abstraction interface
│   │   ├── device_manager.hpp     # libudev device hotplug monitor
│   │   ├── device_manager.cpp
│   │   ├── zmk_raw_hid_device.hpp # ZMK Raw HID implementation
│   │   ├── zmk_raw_hid_device.cpp
│   │   └── qmk_voyager_device.hpp # QMK / Voyager stub
│   ├── cache/
│   │   ├── keymap_cache.hpp       # Persistent disk cache (JSON)
│   │   └── keymap_cache.cpp
│   ├── ipc/
│   │   ├── dbus_server.hpp        # sd-bus / sdbus-c++ service
│   │   └── dbus_server.cpp
│   ├── daemon_main.cpp            # Main entrypoint for stratad
│   └── cli_main.cpp               # Main entrypoint for strata-ctl
└── tests/
    ├── test_protocol.cpp
    ├── test_cache.cpp
    └── test_device_mock.cpp
```

---

## 5. Client Repository Design (`strata-app`)

### 5.1 Technology Stack
* **Language**: C++23
* **Framework**: Qt 6.6+ (Core, Gui, Qml, Quick, QuickControls2, DBus)
* **API Policy**: **No Qt private APIs** (`#include <private/...>` strictly forbidden)
* **Theme & Styling**:
  - `QQuickStyle::setStyle("Fusion");` as baseline, automatically reading system `QPalette`.
  - Supports freedesktop appearance portal (`org.freedesktop.appearance.color-scheme`) for instant Light/Dark switching without application restart.
  - Zero hardcoded colors or app-specific skins: all key caps, backgrounds, borders, and text bind to `palette.window`, `palette.base`, `palette.text`, `palette.highlight`, etc.
* **Configuration**: `tomlplusplus` (`toml++`) header-only library for `~/.config/strata/config.toml`.
* **Platform Target**: **Wayland only**. No X11 libraries or X11-specific atoms.

### 5.2 StatusNotifierItem (System Tray)
* In Wayland compositors (Plasma, GNOME with AppIndicator, Sway, Hyprland via Waybar), standard tray icons use the **FreeDesktop StatusNotifierItem (SNI)** D-Bus specification.
* Qt 6 `QSystemTrayIcon` natively uses SNI over D-Bus on Wayland when no XEmbed tray is present.
* CLI arguments:
  - `--minimized` / `--tray`: Launches directly to tray with the main visualizer window hidden.
* Tray context menu:
  - Header: Connected device model & current active layer badge
  - Item: *Open Visualizer*
  - Item: *Settings*
  - Separator
  - Item: *Quit*

### 5.3 Desktop Notifications
* Communicates with `org.freedesktop.Notifications` via D-Bus.
* Configurable via TOML:
  ```toml
  [notifications]
  on_device_connect = true
  on_device_disconnect = true
  on_layer_change = false       # Disabled by default so fast typing doesn't spam toasts
  battery_warning_threshold = 15
  ```

### 5.4 Visualizer & QML Architecture
* **Split Ergonomic Keymap Model**:
  - The QML layer renders key matrix slots based on the device profile:
    - Corne layout: 3x6 staggered alphas + 3 thumb keys per half + central joystick/encoder.
  - Exposes `activeLayerIndex` as a reactive QML property.
* **Smart Key Legend Decoder**:
  - Resolves ZMK packed usages (`param1`, `param2`):
    - `0x00070035` $\rightarrow$ `` ` ``
    - `0x00070014` $\rightarrow$ `Q`
    - `home_row_mod` with `param1: 0x000700e3`, `param2: 0x00070004` $\rightarrow$ Tap: `A`, Hold: `GUI`
    - `layer_tap` with `param1: 1`, `param2: 0x0007002c` $\rightarrow$ Tap: `SPACE`, Hold: `[NUM]`
* **Low Overhead**:
  - Updates only on `LayerChanged` D-Bus signal. Does not capture or process individual typing keystrokes.

### 5.5 Client Repo File Hierarchy
```text
strata-app/
├── CMakeLists.txt
├── Taskfile.yml
├── .clang-format
├── .clang-tidy
├── .github/
│   └── workflows/
│       └── ci.yml
├── assets/
│   ├── icons/
│   │   ├── strata.svg
│   │   └── strata-tray-active.svg
│   └── desktop/
│       └── org.freedesktop.Strata.desktop
├── src/
│   ├── main.cpp
│   ├── config/
│   │   ├── config_manager.hpp     # tomlplusplus wrapper
│   │   └── config_manager.cpp
│   ├── dbus/
│   │   ├── strata_dbus_client.hpp # QDBusInterface connecting to stratad
│   │   └── strata_dbus_client.cpp
│   ├── tray/
│   │   ├── tray_manager.hpp       # StatusNotifierItem tray handler
│   │   └── tray_manager.cpp
│   ├── notification/
│   │   ├── notifier.hpp           # freedesktop notifications
│   │   └── notifier.cpp
│   └── model/
│       ├── keymap_model.hpp       # QAbstractListModel for QML keymap grid
│       ├── keymap_model.cpp
│       ├── keycode_decoder.hpp    # Translates HID usages to human labels
│       └── keycode_decoder.cpp
└── qml/
    ├── main.qml                   # Main visualizer window
    ├── components/
    │   ├── SplitKeyboard.qml      # Left/Right half renderer
    │   ├── KeyCap.qml             # Adaptive palette key renderer
    │   ├── LayerTabBar.qml        # Layer indicator bar
    │   └── StatusBar.qml          # Connection & battery footer
    └── qml.qrc
```

---

## 6. Implementation Roadmap & Milestones

### Phase 1: Daemon Foundation & ZMK Driver (`strata-daemon`)
- [ ] Initialize CMake C++23 project with `Taskfile.yml`, clang-tidy, and clang-format.
- [ ] Implement `Logger` supporting both ANSI TTY and `sd_journal_print`.
- [ ] Implement `IDevice` interface and `ZmkRawHidDevice` driver (migrating raw HID packet parsing and the BLE HoG 0x00 dummy byte logic).
- [ ] Implement `DeviceManager` with `libudev` hotplug monitoring.
- [ ] Implement `KeymapCache` with JSON disk persistence.
- [ ] Write unit tests for protocol serialization, device state transitions, and caching.

### Phase 2: D-Bus Engine & CLI (`strata-daemon`)
- [ ] Implement `DBusServer` exposing `org.freedesktop.Strata.Device1` methods, properties, and signals.
- [ ] Integrate systemd `sd_notify` (`READY=1`, `STATUS=...`).
- [ ] Write `stratad.service` systemd unit and D-Bus auto-activation service file.
- [ ] Implement `strata-ctl` CLI binary querying the daemon over D-Bus (`status`, `layers`, `keymap`, `cache`, `listen`).
- [ ] Test systemd user service startup, journald log output, and hot-unplug/hot-plug over BLE and USB.

### Phase 3: Desktop Companion Application (`strata-app`)
- [ ] Initialize CMake Qt 6 / QML C++23 project.
- [ ] Implement `ConfigManager` with `tomlplusplus` for `config.toml`.
- [ ] Implement `StrataDBusClient` binding to `org.freedesktop.Strata`.
- [ ] Implement StatusNotifierItem tray icon with `--minimized` launch support.
- [ ] Implement desktop notifications for device connection/disconnection.
- [ ] Implement QML keymap visualizer for Eyelash Corne layout with dynamic layer highlighting.
- [ ] Verify dark/light theme switching using standard Qt palette.

### Phase 4: QMK / ZSA Voyager Extension
- [ ] Implement `QmkVoyagerDevice` subclass in `strata-daemon`.
- [ ] Add Voyager layout geometry in `strata-app` QML visualizer.
- [ ] Test seamless runtime switching when both Corne and Voyager are connected.

---

## 7. Verification & Quality Gates

### Automated Verification
```bash
# Daemon
task format:check    # clang-format verification
task lint            # clang-tidy check
task test            # CTest unit test suite

# Client
task build           # Full Ninja Qt6 compilation
task test            # QTest suite for keycode decoder & config parser
```

### Manual Verification Checklist
1. **Systemd & Journald**:
   - `systemctl --user start stratad`
   - In normal mode, verify journald logs startup and connection, but does NOT spam when switching layers.
   - Run `STRATAD_LOG_LEVEL=debug systemctl --user restart stratad`, verify full debugging output appears in `journalctl --user -u stratad -f`.
2. **D-Bus Inspection**:
   - Run `busctl --user introspect org.freedesktop.Strata /org/freedesktop/Strata/Device0`.
   - Run `busctl --user monitor org.freedesktop.Strata` and switch layers on the physical keyboard; verify `LayerChanged` signal fires instantly.
3. **Tray & Wayland**:
   - Run `strata --minimized` in Hyprland / Sway / KDE Wayland session; verify icon appears in Waybar/panel tray.
   - Click tray item *Open Visualizer*; verify main window displays Eyelash Corne geometry with current layer keys illuminated.
4. **Theme Adaptation**:
   - Toggle system dark/light mode in desktop settings; verify QML window adapts colors immediately without hardcoded artifacts.
