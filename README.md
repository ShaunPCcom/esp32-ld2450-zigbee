# LD2450-ZB-H2

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

## Overview

A Zigbee presence sensor built on the ESP32-H2 and HLK-LD2450 24GHz mmWave radar. It tracks up to 3 people simultaneously, reports their positions, and lets you define **10 custom polygon zones** for room-level presence detection — all integrated into Home Assistant via Zigbee2MQTT.

This is a native Zigbee alternative to ESPHome-based LD2450 implementations. No WiFi needed, no cloud, just mesh networking that works with any Zigbee coordinator.

**Based on**: [TillFleisch/ESPHome-HLK-LD2450](https://github.com/TillFleisch/ESPHome-HLK-LD2450) — UART protocol implementation derived from this ESPHome component (MIT License). Reimplemented in C for ESP-IDF with Zigbee support and multi-zone architecture.

## Features

- **3-target tracking** — real-time X/Y coordinates (mm) for up to 3 people
- **10 configurable zones** — custom polygon areas with 3–10 vertices each (e.g., "couch", "desk", "bed")
- **Zigbee2MQTT integration** — 89 Home Assistant entities via external converter
- **OTA firmware updates** — remote updates via Zigbee2MQTT with automatic rollback if an update fails
- **Coordinator fallback** — keeps lights working during coordinator or HA outages using direct Zigbee bindings and a heartbeat watchdog ([setup guide](docs/coordinator-fallback.md))
- **All settings persist** — configuration survives reboots, even without a coordinator connection
- **Crash diagnostics** — boot count, reset reason, uptime, and heap sensors for remote debugging
- **Serial CLI** — configure everything over USB without needing a network connection
- **LED status** — WS2812 RGB LED shows connection state at a glance
- **Two-level factory reset** — 3 s hold resets Zigbee network (keeps config), 10 s hold wipes everything

## Zigbee vs ESPHome

This Zigbee implementation offers different trade-offs compared to ESPHome-based versions:

### Advantages of Zigbee

- **Native Zigbee** — no WiFi configuration, works with any Zigbee coordinator
- **Mesh networking** — router-capable, extends your Zigbee network range
- **OTA updates** — remote firmware updates via Zigbee2MQTT with automatic rollback
- **Settings persist independently** — config is stored on the device, not in the coordinator
- **Multi-endpoint architecture** — 11 Zigbee endpoints for cleaner HA organization
- **Two-level factory reset** — separate Zigbee vs full config reset
- **Serial CLI** — direct UART configuration without network dependency
- **10 configurable zones** — more than typical ESPHome examples (which show 1 zone)
- **Flexible polygons** — 3–10 vertices per zone (triangles, rectangles, irregular shapes)

### Advantages of ESPHome

- **Unlimited zones** — component supports unlimited zones (vs fixed 10)
- **Rich per-target data** — individual speed, distance, angle sensors per target
- **Dynamic zone updates** — runtime polygon updates via actions
- **Web interface** — ESPHome web UI for configuration
- **WiFi diagnostics** — built-in web-based tools

### Equivalent Features

Both versions support: occupancy detection (overall + per-zone), target count, max distance/angle limits, tracking mode (single/multi), coordinate publishing, and remote restart.

**Choose Zigbee if**: you want native mesh networking, simpler setup, or network-independent config.
**Choose ESPHome if**: you need unlimited zones, per-target sensors, or prefer WiFi/web management.

## Hardware

### Requirements

- **ESP32-H2 DevKit** (native 802.15.4 Zigbee radio)
- **HLK-LD2450** 24GHz mmWave radar module
- 5V power supply (USB or mains adapter)

### Wiring

| ESP32-H2 Pin | LD2450 Pin | Function |
|--------------|------------|----------|
| GPIO12       | RX         | ESP32 TX → LD2450 RX (commands) |
| GPIO22       | TX         | ESP32 RX ← LD2450 TX (data) |
| GPIO8        | —          | Status LED (built-in WS2812 on most DevKits) |
| GPIO9        | —          | BOOT button (factory reset) |
| 5V           | 5V         | Power |
| GND          | GND        | Ground |

**Notes**:
- UART baud rate: 256000
- GPIO9 is the BOOT button (active-low, internal pull-up)
- **GPIO8 Status LED**: Most ESP32-H2 boards (like the DevKitM-1) have a built-in addressable RGB LED (WS2812) on GPIO8. This is the programmable status LED — not the red power LED that stays on when the board is plugged in.

## Building

### Prerequisites

- **ESP-IDF v5.5+** ([installation guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32h2/get-started/))
- Git

### Build and Flash

```bash
git clone <repository-url>
cd ld2450_zb_h2

# Set up ESP-IDF environment (once per terminal session)
. $HOME/esp/esp-idf/export.sh

# Build and flash
idf.py set-target esp32h2
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

**Note**: `idf.py monitor` triggers a reboot on ESP32-H2 due to DTR/RTS reset. Use `idf.py monitor --no-reset` to attach without rebooting.

## Zigbee2MQTT Setup

1. **Install the external converter**:
   ```bash
   cp z2m/ld2450_zb_h2.js /path/to/zigbee2mqtt/data/external_converters/
   ```

2. **Restart Zigbee2MQTT**

3. **Pair the device**:
   - Make sure the device isn't already paired (LED shows amber blink)
   - In Z2M, click "Permit join (All)"
   - Power cycle the ESP32-H2 or press reset
   - Device auto-pairs and appears in Z2M

4. **Reconfigure** (if entities are missing):
   - Select the device in Z2M → click "Reconfigure"
   - Refresh Home Assistant to see all 89 entities

## Firmware Updates (OTA)

Updates are delivered wirelessly through the Zigbee network with automatic rollback protection. If a new firmware fails to boot, the device automatically reverts to the previous working version.

### Getting Notified

When a new version is available, Home Assistant shows an update notification via the `update.ld2450_update` entity — just like officially supported devices. Updates are never installed automatically.

### Installing

**Via Home Assistant:**
1. Go to Settings → Devices & Services → Zigbee2MQTT → [Your LD2450 device]
2. Click "Install" on the firmware update card
3. Monitor progress in the Z2M logs
4. Device reboots automatically after a successful update

**Via Zigbee2MQTT UI:**
1. Select the device → go to the "About" tab
2. Click "Update to latest firmware"
3. Watch the progress bar

### How It Works

1. **Download**: Firmware transfers over Zigbee in 64-byte blocks (~2–5 minutes)
2. **Validation**: Device verifies the image before applying
3. **Reboot**: Device boots into the new firmware
4. **Rollback**: If the new firmware fails, the bootloader reverts automatically

The device checks for updates every 24 hours. Trigger a manual check from the Z2M "About" tab.

### Hosting and Releases

Firmware releases are on the [GitHub Releases page](https://github.com/ShaunPCcom/ESP32-H2-LD2450/releases). Z2M checks the OTA index automatically.

Releases are fully automated via GitHub Actions — tagging a new version triggers the build, creates the OTA image, publishes the release, and updates the OTA index. See `.github/RELEASE.md` for details.

## Exposed Entities

All 89 entities are automatically discovered in Home Assistant via Zigbee2MQTT.

### Sensors (Read-only)

| Entity | Type | Description |
|--------|------|-------------|
| `occupancy` | Binary | Overall presence (any target detected) |
| `zone_1_occupancy` … `zone_10_occupancy` | Binary ×10 | Per-zone presence |
| `target_count` | Numeric (0–3) | Number of tracked targets |
| `target_1_x` / `target_1_y` … `target_3_x` / `target_3_y` | Numeric ×6 | Target coordinates in metres |
| `boot_count` | Numeric | Total reboots since first flash |
| `reset_reason` | Numeric (0–15) | Last reset cause (1=power on, 3=software, 8=brownout) |
| `last_uptime_sec` | Numeric | Uptime before last reset (0 after power loss) |
| `min_free_heap` | Numeric (bytes) | Lowest free memory since boot |

### Configuration (Read-Write)

| Entity | Type | Range | Description |
|--------|------|-------|-------------|
| `max_distance` | Numeric | 0–6 m | Maximum detection distance |
| `angle_left` | Numeric | 0–90° | Left angle limit |
| `angle_right` | Numeric | 0–90° | Right angle limit |
| `tracking_mode` | Switch | Multi/Single | Multi-target tracking mode |
| `coord_publishing` | Switch | ON/OFF | Enable coordinate output |
| `occupancy_cooldown` | Numeric | 0–300 s | Delay before reporting Clear (main sensor) |
| `occupancy_delay` | Numeric | 0–65535 ms | Delay before reporting Occupied (main sensor) |
| `fallback_enable` | Switch | ON/OFF | Enable coordinator fallback system |
| `fallback_mode` | Switch | ON/OFF | Hard fallback active (ON = sensor is controlling lights) |
| `fallback_cooldown` | Numeric | 0–300 s | How long to keep light on after presence clears (main, fallback only) |
| `heartbeat_enable` | Switch | ON/OFF | Enable software watchdog for HA/Z2M crash detection |
| `heartbeat_interval` | Numeric | 30–3600 s | Expected ping interval (watchdog fires at 2× this) |
| `hard_timeout_sec` | Numeric | 1–60 s | Seconds after first soft fault before escalating to hard fallback |
| `ack_timeout_ms` | Numeric | 500–10000 ms | How long to wait for coordinator ACK before soft fallback |

### Zone Configuration (5 entities per zone, 50 total)

Each of the 10 zones has:

| Entity | Type | Description |
|--------|------|-------------|
| `zone_N_vertex_count` | Select | Number of vertices: 0 (disabled) or 3–10 |
| `zone_N_coords` | Text | Polygon vertices as CSV in metres: `x1,y1,x2,y2,...` |
| `zone_N_cooldown` | Numeric (0–300 s) | Delay before reporting Clear for this zone |
| `zone_N_delay` | Numeric (0–65535 ms) | Delay before reporting Occupied for this zone |
| `fallback_cooldown_zone_N` | Numeric (0–300 s) | How long to keep light on after presence clears (fallback only) |

### Actions

| Entity | Type | Description |
|--------|------|-------------|
| `restart` | Select | Set to `restart` to reboot the device |
| `factory_reset_confirm` | Text | Type `factory-reset` exactly to wipe everything |
| `heartbeat` | Select | Set to `ping` to send a manual heartbeat |

**Total**: 89 entities (11 occupancy, 14 config, 50 zone config, 8 diagnostic sensors, 3 actions, 3 target coordinates)

## Configuration

### Via Home Assistant

All settings are exposed as entities. Changes are sent to the device over Zigbee, applied to the sensor hardware, and saved to flash — they persist across reboots automatically.

### Via Serial CLI

Connect a serial terminal at 115200 baud:

```bash
ld state                    # View sensor state
ld zones                    # List all zones

# Zone setup
ld zone 1 vertices -0.5 1.0 0.5 1.0 0.5 3.0 -0.5 3.0
ld zone 1 off

# Sensor limits
ld maxdist 5000             # Max distance in mm
ld angle 45 45              # Left and right angle limits

# Tracking
ld mode multi               # or: ld mode single
ld coords on                # Enable coordinate publishing

# Occupancy timing
ld cooldown 10              # Main sensor cooldown (seconds)
ld cooldown zone 1 15       # Per-zone cooldown
ld cooldown all 10          # Set all 11 endpoints

ld delay 250                # Main sensor delay (ms)
ld delay zone 1 500         # Per-zone delay
ld delay all 250            # Set all 11 endpoints

# Device management
ld config                   # View current config
ld diag                     # Crash diagnostics
ld nvs                      # NVS health check
ld bt off                   # Disable Bluetooth
ld reboot                   # Restart
ld factory-reset            # Full factory reset
```

All CLI changes are saved immediately and persist across reboots.

### Best Practices

**During zone setup:**
1. Turn on **coordinate publishing** — you need to see where targets are
2. Switch to **single target mode** — easier to test one zone at a time
3. Walk through each zone to verify boundaries
4. Use a Plotly graph card (see [examples](#examples)) to visualise positions and zones

**During normal operation:**
1. Turn off **coordinate publishing** — reduces Zigbee traffic (occupancy is all you need)
2. Switch to **multi-target mode** — tracks up to 3 people for better occupancy accuracy

### Occupancy Cooldown

Prevents false "unoccupied" reports when someone briefly moves out of view — for example, walking behind furniture or bending down.

**How it works:**
- When presence clears, a timer starts instead of immediately reporting Clear
- If presence returns during the timer, the Clear report is cancelled
- Occupied is always reported immediately

**Typical values:**
- `0 s` (default): No debouncing — instant reporting
- `5–10 s`: Good for most rooms
- `30–60 s`: Bathrooms, areas with intermittent visibility
- `120+ s`: Large or outdoor spaces

**Example** (10 s cooldown):
1. Person walks in → "Occupied" immediately
2. Person walks behind furniture → timer starts, still shows "Occupied"
3. Person returns within 10 s → timer resets, still "Occupied"
4. Person leaves the room → after 10 s, reports "Clear"

Each endpoint (main + 10 zones) has its own cooldown. Tune per area:
- Hallway: 5 s (quick clearing for transit)
- Bedroom: 30 s (avoid premature clearing)
- Bathroom: 120 s (people move out of view often)

### Occupancy Delay

Prevents false "occupied" reports from brief detections — sensor noise, a hand wave, or someone passing through without entering. Sometimes called anti-ghosting.

**How it works:**
- When presence is first detected, a timer starts instead of immediately reporting Occupied
- If presence clears during the timer, the Occupied report is cancelled
- Clear reports are governed by the cooldown (see above)

**Typical values:**
- `0 ms`: No delay — instant reporting
- `250 ms` (default): Filters ghost detections while staying responsive
- `500–1000 ms`: Transit areas where you want to confirm presence first

**Example** (250 ms delay):
1. Brief sensor blip (hand wave) → timer starts → blip disappears → "Occupied" never sent
2. Person walks in and stays → after 250 ms, reports "Occupied"

Each endpoint has its own delay. Tune per area:
- Entry zones: 250 ms (filter brief detections)
- Seated areas: 500 ms (require sustained presence)
- Transit zones: 0 ms (react immediately)

## Coordinator Fallback

When your coordinator or Home Assistant goes down, the sensor can keep controlling
lights on its own using direct Zigbee bindings. A heartbeat watchdog catches
cases where the Zigbee radio is fine but HA has crashed.

See [docs/coordinator-fallback.md](docs/coordinator-fallback.md) for the full
setup guide — device settings, blueprint configuration, recovery behaviour,
and tuning.

## Examples

### Home Assistant Plotly Dashboard

The `examples/home-assistant/` directory has interactive Plotly dashboard examples for visualising sensor data and zones. Useful during zone setup and for debugging.

See [`examples/home-assistant/README.md`](examples/home-assistant/README.md) for setup instructions.

## LED Status

The built-in LED on GPIO8 shows the current connection state:

| Colour/Pattern | Meaning |
|----------------|---------|
| **Amber blink** | Not joined to a Zigbee network |
| **Blue blink** | Pairing in progress |
| **Green solid** | Joined and operational |
| **Red blink** | Error or factory reset in progress |

**Note**: This requires an addressable RGB LED (WS2812) on GPIO8 — standard on most ESP32-H2 DevKits. If your board has a simple single-colour LED, you'll see blink patterns instead of colour changes.

## Factory Reset

Two levels of reset via the BOOT button (GPIO9):

### Zigbee Network Reset (3 s hold)

Leaves the network but **keeps all config** (zones, distances, angles). Use this when moving the sensor to a different coordinator.

1. Hold BOOT for 3–10 s
2. LED: fast red blink (1–3 s) → slow red blink (3–10 s)
3. Release between 3–10 s
4. Device is ready to re-pair

### Full Factory Reset (10 s hold)

Erases **everything** — Zigbee network and all saved configuration.

1. Hold BOOT for >10 s
2. LED: fast red blink → slow red blink → solid red (>10 s)
3. Release after 10 s
4. Device resets to factory defaults

You can also trigger a network reset from Z2M (Device → Settings → Remove) or a full reset via CLI (`ld factory-reset`).

## Architecture

- **Sensor driver**: `components/ld2450/` — UART RX task, protocol parser, zone logic
- **Command encoder**: `components/ld2450/ld2450_cmd.c` — UART TX, config mode, ACK reader
- **Zigbee modules**:
  - `main/zigbee_init.c` — stack setup, endpoint/cluster creation
  - `main/zigbee_attr_handler.c` — attribute write dispatch
  - `main/sensor_bridge.c` — sensor polling, state tracking, reporting
  - `main/zigbee_signal_handlers.c` — network lifecycle, factory reset
- **NVS persistence**: `main/nvs_config.c` — load/save all config on boot/change
- **Z2M converter**: `z2m/ld2450_zb_h2.js` — custom cluster definitions, fromZigbee/toZigbee

### Zigbee Endpoints

- **EP 1**: Main device — overall occupancy + all config attributes
- **EP 2–11**: Zones 1–10 — per-zone occupancy (zone config lives on EP 1)

### Custom Clusters

- **0xFC00** (EP 1): Target count, coordinates, max distance, angle, tracking mode, coordinate publishing, occupancy cooldown/delay, crash diagnostics, restart, factory reset, zone config (vertex count, coords, cooldown, delay for all 10 zones)

## Acknowledgments

UART protocol decoding is derived from [TillFleisch/ESPHome-HLK-LD2450](https://github.com/TillFleisch/ESPHome-HLK-LD2450) (MIT License). Reimplemented in C for ESP-IDF. See `NOTICE.md` and `third_party/ESPHome-HLK-LD2450/README.md` for details.

## License

MIT License — see [LICENSE](LICENSE) for details.

Copyright (c) 2026 Shaun Foulkes
