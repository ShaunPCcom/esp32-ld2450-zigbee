# Changelog

## v2.3.1 - 2026-04-11

### Fixes
- **Web UI not updating via OTA**: `app.js`, `style.css`, and `index.html` were stored in a separate SPIFFS partition that OTA never touched — devices would run new firmware with the old UI indefinitely. All three files are now compiled directly into the firmware binary, so every OTA update carries the current web UI automatically.

---

## v2.3.0 - 2026-04-11

### Features
- **C6 end-device mode**: ESP32-C6 builds now run as a Zigbee end device rather than a router. WiFi and Zigbee router mode share the C6 radio in a way Espressif doesn't support reliably — devices trying to join through a C6 router would fail to pair. End-device mode eliminates the problem entirely. H2 is unaffected and remains a full router.
- **Manual OTA upload**: New "Manual Upload" section in the System tab lets you flash a `.ota` release file directly from your browser without going through Z2M or a network update.
- **Zone coordinate inputs**: The Zones tab now shows X/Y text boxes for each vertex (in metres, matching Z2M). Editing a value applies the change immediately and stays in sync with drag-and-drop edits and Z2M attribute writes. Intended for fine-tuning after placing zones visually.
- **Radar cartesian grid**: Metre-spaced X and Y grid lines on the radar canvas give you a spatial reference for placing and aligning zone vertices.
- **Zigbee-only reset**: `zb-reset` CLI command and System tab button clear the Zigbee network state so you can re-pair without losing zone configs or device settings.

### Fixes
- **Web UI live view freeze**: Added a 3-second watchdog on the WebSocket client. A half-open TCP connection would leave the radar frozen indefinitely — the only recovery was a device restart. The watchdog detects silence and reconnects automatically.
- **Z2M EP1 config read**: Attribute reads batched to avoid INSUFFICIENT_SPACE errors on reconfigure.

---

## v2.2.5 - 2026-04-04

### Fixes
- **OTA "already in progress" on every trigger**: Slot acquisition moved from query-response time to the OTA START callback. A Z2M "check for updates" fires a query response but never starts block transfer — acquiring the slot there left it permanently held, rejecting all subsequent triggers.
- **ABORT retry double-release**: Slot is now released before scheduling the retry alarm (not held through the wait). The START callback re-acquires it when the retry fires.
- **Z2M update check triggering spurious Wi-Fi download**: Query responses with version=0 / size=0 (Z2M check-only notifications) are now ignored instead of launching a download.
- **Wi-Fi OTA "Out of buffer" from GitHub CDN**: GitHub release URLs redirect to a signed CDN URL with a JWT-bearing path (~930 chars). Fixed by pre-resolving the redirect chain manually before streaming, and setting `buffer_size_tx=4096` for the CDN request.

---

## v2.2.4 - 2026-03-31

### Features (C6)
- **Web UI OTA trigger**: System tab now has a "Check for Updates" button and an "Update Firmware" button (enabled only when an update is available). An amber banner appears at the top of the page when a newer version is found — clicking it jumps to the System tab.
- **Background update check**: Device polls the OTA index every 12 hours. Interval is configurable (1–72 h) via a slider in the System tab and persists across reboots.
- **Wi-Fi OTA transport**: When Wi-Fi is connected, firmware downloads directly over HTTPS instead of transferring block-by-block over Zigbee — cuts update time from minutes to seconds.

### Internal
- Modular OTA component refactor: `zigbee_ota` split into `ota_state`, `ota_writer`, `ota_header`, `ota_zigbee_transport`, `ota_wifi_transport`, `ota_trigger_z2m`, `ota_trigger_web` — public API unchanged.

---

## v2.2.3 - 2026-03-21

### Fixes
- Enable Boya flash driver for nanoESP32-C6 boards
- Enable WiFi modem sleep and larger OTA blocks to reduce radio coexistence interference on C6

---

## v2.2.2 - 2026-02-28

### Features
- Dual-target support: firmware now builds for both ESP32-H2 and ESP32-C6
- C6 adds Wi-Fi provisioning, web UI (radar visualisation + settings), and mDNS
- Separate OTA images per target (imageType 0x0001 H2 / 0x0003 C6)

---

## v2.2.1 - 2026-01-15

### Fixes
- Coordinator fallback: soft fallback correctly distinguishes per-zone ack timeouts from global hard timeout
- Zigbee signal handler extraction into dedicated source file

---

## v2.2.0 - 2025-12-10

### Features
- Two-tier coordinator fallback: soft (per-zone latency assist) and hard (full device autonomy)
- Heartbeat watchdog with configurable interval
- Fallback cooldown to suppress re-entry noise after recovery
