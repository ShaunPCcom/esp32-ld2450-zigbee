# LD2450-ZB-H2

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

## Overview

ESP32-H2 + HLK-LD2450 mmWave presence sensor with native Zigbee support. This firmware is a Zigbee alternative to ESPHome-based implementations, bringing native Zigbee mesh networking to the LD2450 sensor.

The LD2450 is a 24GHz mmWave radar sensor that tracks up to 3 targets simultaneously, reporting their X/Y coordinates. This firmware adds **10 configurable polygon zones** (3–10 vertices each — triangles, rectangles, or any irregular shape) for room-level presence detection and integrates everything into Home Assistant via Zigbee2MQTT.

**Based on**: [TillFleisch/ESPHome-HLK-LD2450](https://github.com/TillFleisch/ESPHome-HLK-LD2450) - UART protocol implementation derived from this ESPHome component (MIT License). Reimplemented in C for ESP-IDF with Zigbee support and multi-zone architecture.

## Features

- **3-target tracking**: Real-time X/Y coordinates (mm) for up to 3 moving targets
- **10 configurable zones**: Define custom polygon presence detection areas with 3–10 vertices each (e.g., "couch", "desk", "bed")
- **Zigbee2MQTT integration**: 71 Home Assistant entities via external converter
- **OTA firmware updates**: Remote updates via Zigbee2MQTT (dual partition rollback protection)
- **Crash diagnostic telemetry**: Remote debugging via boot_count, reset_reason, last_uptime, and min_free_heap sensors
- **NVS persistence**: All configuration survives reboots (independent of coordinator)
- **Serial CLI**: Configure zones, tracking mode, distance/angle limits over UART
- **LED status indicator**: WS2812 RGB shows connection state
- **Two-level factory reset**: 3s hold = Zigbee network reset (keeps config), 10s hold = full factory reset

## Comparison: Zigbee vs ESPHome

This Zigbee implementation offers different trade-offs compared to ESPHome-based versions:

### **Advantages of Zigbee Version**
- ✅ **Native Zigbee** - No WiFi configuration, works with any Zigbee coordinator
- ✅ **Mesh networking** - Router-capable, extends Zigbee network range
- ✅ **OTA updates** - Remote firmware updates via Zigbee2MQTT with automatic rollback
- ✅ **NVS persistence** - Config survives without coordinator connection
- ✅ **Multi-endpoint architecture** - 11 Zigbee endpoints (cleaner HA organization)
- ✅ **Two-level factory reset** - Separate Zigbee vs full config reset
- ✅ **Serial CLI** - Direct UART configuration without network dependency
- ✅ **10 configurable zones** - More than typical ESPHome examples (which show 1 zone)
- ✅ **Flexible polygons** - 3–10 vertices per zone (triangles, rectangles, irregular shapes)

### **Advantages of ESPHome Version**
- ✅ **Unlimited zones** - Component supports unlimited zones (vs fixed 10)
- ✅ **Rich per-target data** - Individual speed, distance, angle sensors per target
- ✅ **Dynamic zone updates** - Runtime polygon updates via actions
- ✅ **Web interface** - ESPHome web UI for configuration
- ✅ **WiFi diagnostics** - Built-in web-based tools

### **Equivalent Features**
- Occupancy detection (overall + per-zone)
- Target count reporting
- Max distance / angle limits
- Tracking mode (single/multi)
- Coordinate publishing
- Restart button

**Choose Zigbee if**: You want native Zigbee mesh, simpler setup, or network-independent config.
**Choose ESPHome if**: You need unlimited zones, flexible polygons, or prefer WiFi/web management.

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
| GPIO8        | -          | Status LED (built-in on most DevKits) |
| GPIO9        | -          | BOOT button (factory reset) |
| 5V           | 5V         | Power |
| GND          | GND        | Ground |

**Notes**:
- UART baud rate: 256000
- GPIO9 is the ESP32-H2 DevKit BOOT button (active-low, internal pull-up)
- **GPIO8 Status LED**: Many ESP32-H2 development boards (such as the ESP32-H2-DevKitM-1) include a built-in addressable RGB LED (WS2812) connected to GPIO8. This is the programmable status LED that shows Zigbee connection state. The firmware uses the ESP-IDF `led_strip` driver to display color-coded status. Note: Your board may also have a separate red power LED that stays on when powered - this is not the status LED.

## Building

### Prerequisites

- **ESP-IDF v5.5+** ([installation guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32h2/get-started/))
- Git

### Build and Flash

```bash
# Clone the repository
git clone <repository-url>
cd ld2450_zb_h2

# Set up ESP-IDF environment (do this in every new terminal session)
. $HOME/esp/esp-idf/export.sh

# Configure, build, and flash
idf.py set-target esp32h2
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

**Note**: `idf.py monitor` will trigger a device reboot on ESP32-H2 due to DTR/RTS reset behavior. This is expected. Use `idf.py monitor --no-reset` to avoid triggering a reboot when attaching.

## Zigbee2MQTT Setup

1. **Install the external converter**:
   ```bash
   cp z2m/ld2450_zb_h2.js /path/to/zigbee2mqtt/data/external_converters/
   ```

2. **Restart Zigbee2MQTT**

3. **Pair the device**:
   - Ensure the device is not already paired (LED shows amber "not joined")
   - In Z2M UI, click "Permit join (All)"
   - Power cycle the ESP32-H2 or press the reset button
   - Device will auto-pair and appear in Z2M

4. **Reconfigure** (if entities are missing):
   - In Z2M, select the device
   - Click "Reconfigure" in device settings
   - Refresh Home Assistant to see all 71 entities

## Firmware Updates (OTA)

This device supports Over-The-Air (OTA) firmware updates via Zigbee2MQTT. Updates are delivered wirelessly through the Zigbee network with automatic rollback protection.

### Update Notifications

When a new firmware version is available, Home Assistant will show an update notification via the `update.ld2450_update` entity (similar to officially supported Zigbee devices). Updates are **not** installed automatically - you must approve them manually.

### Installing Updates

**Via Home Assistant:**
1. Navigate to Settings → Devices & Services → Zigbee2MQTT → [Your LD2450 device]
2. Click "Install" on the firmware update card
3. Monitor progress in the Zigbee2MQTT logs
4. Device will reboot automatically after successful update

**Via Zigbee2MQTT UI:**
1. Select the device in Z2M
2. Go to the "About" tab
3. Click "Update to latest firmware"
4. Monitor the OTA progress bar

### Update Process

1. **Download**: Firmware is transferred over Zigbee in 64-byte blocks (~2-5 minutes)
2. **Validation**: Device verifies the downloaded image
3. **Reboot**: Device reboots into the new firmware partition
4. **Rollback protection**: If the new firmware fails to boot, the bootloader automatically reverts to the previous working version

### Hosting Firmware Updates

Firmware releases are hosted on GitHub. To check for updates or manually download firmware:

1. Visit the [GitHub Releases page](https://github.com/ShaunPCcom/ESP32-H2-LD2450/releases)
2. Download the latest `.ota` file
3. Zigbee2MQTT automatically checks the configured OTA index for new versions

**Note**: The device checks for updates every 24 hours by default. You can trigger a manual check from the Z2M UI "About" tab.

### Release Automation

Firmware releases are fully automated using GitHub Actions. When a new version is tagged in the repository:

1. **Automated build**: ESP-IDF builds the firmware on Ubuntu runners
2. **OTA image creation**: Zigbee OTA format packaging with proper headers
3. **GitHub release**: Published with release notes from git commits
4. **OTA index update**: `z2m/ota_index.json` automatically updated with download URL

This ensures consistent, reproducible releases and enables rapid bug fixes. Users receive update notifications automatically through Zigbee2MQTT/Home Assistant within 24 hours of release publication (or on manual update check).

**For developers**: See `.github/RELEASE.md` for the release process and version management details.

## Exposed Entities

All entities are automatically discovered in Home Assistant via Zigbee2MQTT:

### Sensors (Read-only)

| Entity | Type | Description |
|--------|------|-------------|
| `binary_sensor.ld2450_occupancy` | Binary | Overall occupancy (any target present) |
| `binary_sensor.ld2450_zone_1_occupancy` … `zone_10_occupancy` | Binary ×10 | Per-zone occupancy |
| `sensor.ld2450_target_count` | Numeric (0–3) | Number of tracked targets |
| `sensor.ld2450_target_1_x` / `target_1_y` … `target_3_x` / `target_3_y` | Numeric ×6 | Target X/Y coordinates in metres |
| `sensor.ld2450_boot_count` | Numeric | Total device reboots (monotonic counter) |
| `sensor.ld2450_reset_reason` | Numeric (0–15) | Last reset cause (1=POWERON, 3=SOFTWARE, 8=BROWNOUT, etc.) |
| `sensor.ld2450_last_uptime_sec` | Numeric | Uptime in seconds before last reset (0 after power loss) |
| `sensor.ld2450_min_free_heap` | Numeric (bytes) | Minimum free heap memory since boot |

### Configuration (Read-Write)

| Entity | Type | Range | Description |
|--------|------|-------|-------------|
| `number.ld2450_max_distance` | Numeric | 0–6 m | Max detection distance |
| `number.ld2450_angle_left` | Numeric | 0–90° | Left angle limit |
| `number.ld2450_angle_right` | Numeric | 0–90° | Right angle limit |
| `switch.ld2450_tracking_mode` | Switch | ON=Multi/OFF=Single | Multi-target tracking mode |
| `switch.ld2450_coord_publishing` | Switch | ON/OFF | Enable coordinate publishing |
| `number.ld2450_occupancy_cooldown` | Numeric | 0–300 s | Cooldown before reporting Clear (main sensor) |
| `number.ld2450_occupancy_delay` | Numeric | 0–65535 ms | Delay before reporting Occupied (main sensor) |

### Zone Configuration (4 entities per zone, 40 total)

Each of the 10 zones has:

| Entity | Type | Description |
|--------|------|-------------|
| `select.ld2450_zone_N_vertex_count` | Select | Number of vertices: 0 (disabled) or 3–10 |
| `text.ld2450_zone_N_coords` | Text | Polygon vertices as flat metres CSV: `x1,y1,x2,y2,...` |
| `number.ld2450_zone_N_cooldown` | Numeric (0–300 s) | Cooldown before reporting Clear for this zone |
| `number.ld2450_zone_N_delay` | Numeric (0–65535 ms) | Delay before reporting Occupied for this zone |

### Actions

| Entity | Type | Description |
|--------|------|-------------|
| `select.ld2450_restart` | Select | Set to `restart` to reboot the device |
| `text.ld2450_factory_reset_confirm` | Text | Type `factory-reset` to trigger a full factory reset |

**Total**: 71 entities (11 occupancy sensors, 7 config controls, 40 zone config, 8 read-only sensors, 2 diagnostics actions, 3 target coords)

## Configuration

### Via Home Assistant

All writable configuration is exposed as entities in Home Assistant. Changes are automatically:
1. Sent to the device via Zigbee
2. Applied to the LD2450 sensor hardware
3. Saved to NVS (persist across reboots)

### Via Serial CLI

Connect a serial terminal (115200 baud) to see the CLI prompt:

```bash
# View sensor state
ld state

# List all zones
ld zones

# Configure a zone (coordinates in metres, 3-10 vertex pairs)
ld zone 1 vertices -0.5 1.0 0.5 1.0 0.5 3.0 -0.5 3.0
ld zone 1 off

# Set max distance and angle limits (applied via zone filter)
ld maxdist 5000
ld angle 45 45

# Set tracking mode
ld mode multi  # or: ld mode single

# Enable coordinate publishing
ld coords on

# Set occupancy cooldown (seconds before reporting Clear)
ld cooldown 10            # Set main sensor cooldown
ld cooldown zone 1 15    # Set zone 1 cooldown (zones 1-10)
ld cooldown all 10       # Set all 11 endpoints to same value

# Set occupancy delay (ms before reporting Occupied)
ld delay 250             # Set main sensor delay
ld delay zone 1 500      # Set zone 1 delay (zones 1-10)
ld delay all 250         # Set all 11 endpoints to same value

# Bluetooth control
ld bt off

# View current config
ld config

# View crash diagnostics (boot count, reset reason, uptime, heap)
ld diag

# Test NVS health (diagnostics)
ld nvs

# Restart device
ld reboot

# Full factory reset (erase Zigbee network + NVS config)
ld factory-reset
```

**Note**: CLI changes are immediately saved to NVS and persist across reboots.

### Configuration Best Practices

**During Zone Setup:**
1. Enable **coordinate publishing** (`switch.ld2450_coord_publishing` → ON)
2. Enable **single target tracking** (`switch.ld2450_tracking_mode` → Single Target)
3. Walk through each zone one at a time to verify boundaries
4. Use a Plotly graph card (see examples) to visualize target position and zones

**During Normal Operation:**
1. Disable **coordinate publishing** (`switch.ld2450_coord_publishing` → OFF)
   - Reduces Zigbee traffic
   - Only needed for setup/debugging
2. Enable **multi-target tracking** (`switch.ld2450_tracking_mode` → Multi Target)
   - Tracks up to 3 people simultaneously
   - Better for real-world occupancy detection

**Why This Matters:**
- **Single target mode** during setup makes it easier to test one zone at a time (only one person tracked)
- **Coordinate publishing off** during normal operation reduces network overhead (occupancy is all you need for automations)
- **Multi-target mode** during operation allows tracking multiple people moving through different zones

### Occupancy Cooldown

The **occupancy cooldown** feature prevents false "unoccupied" reports when someone briefly moves out of sensor view. This reduces chattiness and prevents lights from turning off prematurely.

**How it works:**
- When occupancy **clears** (no targets detected), a cooldown timer starts
- The sensor does NOT report "Clear" immediately
- After the cooldown period expires, if still clear, "Clear" is reported
- If occupancy returns during the cooldown, the "Clear" report is cancelled (never sent)
- When occupancy is **detected**, an optional delay (default 250 ms) can be configured before reporting — see [Occupancy Delay](#occupancy-delay) below

**Typical values:**
- `0 seconds` (default): Immediate reporting, no debouncing
- `5-10 seconds`: Good for most rooms, prevents brief absences from clearing occupancy
- `30-60 seconds`: Bathrooms, areas where people may briefly leave sensor view
- `120+ seconds`: Larger spaces, outdoor areas with intermittent detection

**Example:** With a 10-second cooldown:
1. Person walks into sensor view → Reports "Occupied" immediately
2. Person walks behind furniture (out of view) → Cooldown starts, stays "Occupied"
3. Person returns within 10 seconds → Still "Occupied", cooldown resets
4. Person leaves room → After 10 seconds, reports "Clear"

**Configuration per endpoint:**
Each of the 11 endpoints (main + 10 zones) has its own independent cooldown value. This allows fine-tuning behavior per area:
- **Bedroom main**: 30s (avoid premature clearing)
- **Bedroom zone (bed)**: 60s (longer for sleeping area)
- **Hallway**: 5s (want quick clearing for transit areas)
- **Bathroom**: 120s (people often move out of view briefly)
- **Office zone (desk)**: 10s (balance between responsiveness and stability)

## Occupancy Delay

The **occupancy delay** feature prevents false "occupied" reports from brief detections (e.g., a hand wave, sensor noise, or someone passing through without entering). This is sometimes called anti-ghosting.

**How it works:**
- When occupancy **transitions from Clear to Occupied**, a delay timer starts
- The sensor does NOT report "Occupied" immediately
- After the delay expires, if still occupied, "Occupied" is reported
- If occupancy clears during the delay window, the "Occupied" report is cancelled (never sent)
- When occupancy **clears**, the Clear report is governed by the cooldown (see above)

**Typical values:**
- `0 ms`: Immediate reporting, no debouncing
- `250 ms` (default): Filters out brief ghost detections while remaining responsive
- `500–1000 ms`: Transit areas where you want to confirm presence before triggering

**Example:** With a 250 ms delay:
1. Sensor briefly detects a target (hand wave) → Delay starts
2. Target disappears within 250 ms → "Occupied" is never reported
3. Person enters room and stays → After 250 ms, reports "Occupied"
4. Person leaves → Cooldown governs when "Clear" is reported

**Configuration per endpoint:**
Each of the 11 endpoints (main + 10 zones) has its own independent delay value. This allows tuning per area:
- **Entry zones**: 250 ms (filter brief pass-through detections)
- **Seated areas**: 500 ms (require sustained presence before triggering)
- **Transit zones**: 0 ms (react immediately to any detection)

## Examples

### Home Assistant Plotly Dashboard

The `examples/home-assistant/` directory includes interactive Plotly dashboard examples for visualizing sensor data and zones. These dashboards are useful during zone setup and for debugging target tracking behavior.

See [`examples/home-assistant/README.md`](examples/home-assistant/README.md) for setup instructions.

## LED Status

The built-in LED on GPIO8 indicates the current Zigbee connection state:

| Color/Pattern | Meaning |
|---------------|---------|
| **Amber blink** | Not joined to a Zigbee network |
| **Blue blink** | Pairing in progress (steering mode) |
| **Green solid** | Joined to network and operational |
| **Red blink** | Error or factory reset in progress (check serial logs) |

**Note**: RGB color indication requires an addressable RGB LED (WS2812-compatible) on GPIO8, which is standard on many ESP32-H2 development boards like the ESP32-H2-DevKitM-1. This is the programmable LED that changes colors, not the power LED. If your board only has a simple single-color LED on GPIO8, you'll see blink patterns (on/off) instead of color changes, which still provides basic status feedback.

## Factory Reset

Two levels of reset are available via the BOOT button (GPIO9):

### Zigbee Network Reset (3 seconds)
Leaves the Zigbee network but **keeps zones and configuration**. Useful for moving the sensor to a different coordinator.

1. Hold BOOT button for 3-10 seconds
2. **LED feedback**:
   - 1-3s: Fast red blink (building to reset)
   - 3-10s: Slow red blink (Zigbee reset armed)
3. Release between 3-10 seconds
4. Device resets Zigbee network, keeps config, ready to re-pair

### Full Factory Reset (10 seconds)
Erases **both** Zigbee network and NVS configuration (zones, max distance, angles, etc.). Complete reset to defaults.

1. Hold BOOT button for >10 seconds
2. **LED feedback**:
   - 1-3s: Fast red blink
   - 3-10s: Slow red blink
   - >10s: Solid red (full reset armed)
3. Release after 10 seconds
4. Device resets everything to factory defaults

**Note**: You can also trigger a Zigbee network reset from Zigbee2MQTT UI (Device → Settings → Remove). For a full reset via CLI, use `ld factory-reset`.

## Architecture

- **Sensor driver**: `components/ld2450/` - UART RX task, protocol parser, zone logic
- **Command encoder**: `components/ld2450/ld2450_cmd.c` - UART TX, config mode, ACK reader
- **Zigbee modules**:
  - `main/zigbee_init.c` - Stack setup, endpoint/cluster creation
  - `main/zigbee_attr_handler.c` - Attribute write dispatch
  - `main/sensor_bridge.c` - Sensor polling, state tracking, reporting
  - `main/zigbee_signal_handlers.c` - Network lifecycle, factory reset
- **NVS persistence**: `main/nvs_config.c` - Load/save all config on boot/change
- **Z2M converter**: `z2m/ld2450_zb_h2.js` - Custom cluster defs, fromZigbee/toZigbee

### Zigbee Endpoints

- **EP 1**: Main device (overall occupancy + all config attributes)
- **EP 2–11**: Zones 1–10 (per-zone occupancy only — zone config lives on EP 1)

### Custom Clusters

- **0xFC00** (EP 1): Target count, coordinates, max distance, angle, tracking mode, coord publishing, occupancy cooldown/delay, crash diagnostics, restart, factory reset, zone config (vertex count, coords CSV, cooldown, delay for all 10 zones via attribute range 0x0040–0x006B)

## Acknowledgments

This project's UART binary protocol decoding logic is derived from [TillFleisch/ESPHome-HLK-LD2450](https://github.com/TillFleisch/ESPHome-HLK-LD2450) (MIT License). The code was reimplemented from scratch in C for ESP-IDF, but the protocol interpretation approach follows the upstream ESPHome component. See `NOTICE.md` and `third_party/ESPHome-HLK-LD2450/README.md` for details.

## License

MIT License - see [LICENSE](LICENSE) file for details.

Copyright (c) 2026 Shaun Foulkes
