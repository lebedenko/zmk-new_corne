# Strata Daemon & CLI (Phase 1) - Walkthrough & Verification Report

Phase 1 of the **Strata** ecosystem has been implemented, tested, and verified on live hardware. The Python POC daemon has been replaced with a modern, high-performance C++23 daemon (`stratad`) and control CLI (`strata-cli`).

Repository: `/home/andrii/Projects/pet/strata-daemon`

---

## 1. Summary of Deliverables

### Core Binaries & Components
1. **`stratad` (Daemon Binary)**:
   - Systemd user service (`Type=notify`) integrating `sd_notify(0, "READY=1")` and `sd_notify(0, "STOPPING=1")`.
   - Dual-mode logging: colored ISO timestamps for interactive TTY terminals; native systemd stream prefixes (`<6>`, `<7>`, etc.) when managed by journald.
   - Quiet by default in normal mode (suppresses high-frequency layer transition spam), detailed diagnostic dumps in debug mode (`-v` or `STRATAD_LOG_LEVEL=debug`).
   - Dynamic device discovery and hotplug monitoring via `libudev` filtering for Raw HID usage page `0xFF60` and usage `0x61`.
   - Native D-Bus session bus service (`org.freedesktop.Strata`) via `sd-bus` from `libsystemd`.
   - Transparent handling of the Linux Bluetooth HoG `0x00` dummy report ID byte.
2. **`strata-cli` (CLI Client)**:
   - Subcommands: `status`, `layers`, `keymap`, `cache`, `listen`.
   - Rich tabular output using C++23 `std::format` or structured JSON via `--json`.
   - Real-time D-Bus signal listener (`strata-cli listen`).
3. **Persistent Keymap Cache**:
   - Stores device keymaps in `~/.cache/strata/keymaps/<Device>_<BuildID>.json`.
   - Fast cache hits eliminate redundant hardware discovery queries over Bluetooth Low Energy.
   - On-demand binding prefetching and cache updates.
4. **Device Abstraction Architecture**:
   - `IDevice`: generic keyboard interface.
   - `ZmkRawHidDevice`: ZMK raw HID driver (active on Eyelash Corne).
   - `QmkVoyagerDevice`: Stub driver ready for ZSA Voyager QMK Raw HID.

---

## 2. Verification on Live Hardware

The implementation was compiled and verified directly against the user's connected **Eyelash Corne** keyboard:

### Daemon Startup & Hardware Discovery
```
2026-09-16 16:13:48.656 [INFO ] Starting stratad (Strata keyboard daemon)...
2026-09-16 16:13:48.656 [DEBUG] Keymap cache directory: /home/andrii/.cache/strata/keymaps
2026-09-16 16:13:48.656 [INFO ] Registered D-Bus service 'org.freedesktop.Strata' at object path '/org/freedesktop/Strata/Device0'
2026-09-16 16:13:48.656 [INFO ] Udev monitor initialized for hidraw devices
2026-09-16 16:13:48.657 [INFO ] Detected Eyelash Corne Raw HID node at /dev/hidraw7
2026-09-16 16:13:48.657 [INFO ] Opened HID device: /dev/hidraw7 (Eyelash Corne)
2026-09-16 16:13:48.657 [INFO ] Device connected: Eyelash Corne (/dev/hidraw7)
2026-09-16 16:13:48.657 [INFO ] stratad is ready and running.
2026-09-16 16:13:49.124 [DEBUG] Layer update: idx=0, name='QWERTY', mask=0x00000000, build='cd1b94f9'
2026-09-16 16:13:49.155 [INFO ] Received keymap summary: 6 layers, 48 keys/layer, build=cd1b94f9
2026-09-16 16:13:49.155 [INFO ] Cache miss for build cd1b94f9. Discovering keymap from hardware...
2026-09-16 16:13:49.185 [DEBUG] Received layer info: index=0, name='QWERTY'
2026-09-16 16:13:49.216 [DEBUG] Received layer info: index=1, name='NUMBER'
2026-09-16 16:13:49.246 [DEBUG] Received layer info: index=2, name='NAV'
2026-09-16 16:13:49.288 [DEBUG] Received layer info: index=3, name='SYS'
2026-09-16 16:13:49.320 [DEBUG] Received layer info: index=4, name='FN'
2026-09-16 16:13:49.350 [DEBUG] Received layer info: index=5, name='GAME'
2026-09-16 16:13:49.350 [INFO ] Saved keymap cache to /home/andrii/.cache/strata/keymaps/Eyelash Corne_cd1b94f9.json
2026-09-16 16:13:49.350 [INFO ] Keymap discovery complete (6 layers). Saved to cache.
```

### CLI Status Command
```bash
❯ ./build/strata-cli status
Keyboard:     Connected (/dev/hidraw7) [Eyelash Corne]
Build ID:     cd1b94f9
Active Layer: QWERTY (Index: 0, Mask: 0x00000000)
Layers:       6 configured
Cache:        Cached
```

### CLI Layers Command
```bash
❯ ./build/strata-cli layers
Idx   ID    Name            Active
----------------------------------
0     0     QWERTY          *
1     1     NUMBER          
2     2     NAV             
3     3     SYS             
4     4     FN              
5     5     GAME            
```

### CLI Keymap Bindings Command
```bash
❯ ./build/strata-cli keymap --layer 0
Device: Eyelash Corne (Build ID: cd1b94f9)
Layers: 6, Keys per layer: 48

Layer 0 Bindings:
  Pos   Behavior          Param 1       Param 2
  ------------------------------------------------
  0     key_press         0x00070035    0
  1     key_press         0x00070014    0
  2     key_press         0x0007001a    0
  3     key_press         0x00070008    0
  4     key_press         0x00070015    0
  5     key_press         0x00070017    0
...
```

### Automated Unit Test Suite
```bash
❯ task test
task: Task "configure" is up to date
task: [build] ninja -C build
ninja: no work to do.
task: [test] ctest --test-dir build --output-on-failure
Test project /home/andrii/Projects/pet/strata-daemon/build
    Start 1: test_protocol
1/2 Test #1: test_protocol ....................   Passed    0.00 sec
    Start 2: test_cache
2/2 Test #2: test_cache .......................   Passed    0.00 sec

100% tests passed out of 2
Total Test time (real) =   0.01 sec
```

---

## 3. D-Bus Specification Reference

The daemon registers on the user session bus:
- **Service**: `org.freedesktop.Strata`
- **Object Path**: `/org/freedesktop/Strata/Device0`
- **Interface**: `org.freedesktop.Strata.Device1`

### Methods
| Method | Signature | Description |
|---|---|---|
| `GetStatus` | `() -> (s)` | Returns JSON string containing current device connection and layer state |
| `GetLayers` | `() -> (s)` | Returns JSON array of configured layers |
| `GetKeymap` | `(uint32 layer, bool refresh) -> (s)` | Returns JSON keymap metadata and bindings (`layer=255` for all layers) |
| `RefreshKeymap` | `() -> (b)` | Forces full hardware keymap rediscovery |
| `ClearCache` | `() -> (b)` | Wipes cached keymap JSON files |

### Signals
| Signal | Signature | Description |
|---|---|---|
| `LayerChanged` | `(uint32 idx, string name, uint32 mask, string build_id)` | Fired immediately on keyboard layer transition |
| `DeviceConnected` | `(string name, string node, string build_id)` | Fired when supported keyboard attaches |
| `DeviceDisconnected` | `(string node)` | Fired when keyboard detaches |
| `KeymapLoaded` | `(string build_id, string source, uint32 layer_count)` | Fired when keymap metadata is loaded from cache or hardware |
| `LayerBindingsLoaded` | `(uint32 layer, uint32 count)` | Fired when all bindings for a layer have arrived |

---

## 4. How to Install and Run as a Systemd Service

To install the daemon as an auto-started user service:

```bash
cd /home/andrii/Projects/pet/strata-daemon

# 1. Install user unit files
task install-service

# 2. Start the daemon service
systemctl --user enable --now stratad

# 3. Check status & logs
systemctl --user status stratad
journalctl --user -u stratad -f
```
