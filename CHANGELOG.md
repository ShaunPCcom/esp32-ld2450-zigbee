# Changelog

## v2.6.1 - 2026-04-27

### Bug Fixes
- **Z2M zone updates now reflect immediately in web UI**: Config changes written via
  Z2M (zone coordinates, cooldowns, sensor settings) now trigger an SSE push to the
  web UI just like web-originated saves do. Previously, the SSE path only fired on
  HTTP POST — so Z2M attribute writes were invisible to the UI until a manual refresh.

---

## v2.6.0 - 2026-04-26

### Features
- **Real-time config and OTA push via SSE**: The web UI now receives config and OTA
  status updates over Server-Sent Events instead of polling. Config changes appear
  immediately rather than on the next 3-second poll. OTA status updates push as they
  happen — no more 60-second cycle or 5-second fast-poll during updates.
- **OTA in-progress indicator**: While a firmware update is running, the web UI shows
  an updating indicator and disables the install button to prevent duplicate triggers.

---

## v2.5.0 - 2026-04-25

### Features
- **ACK-tracked occupancy reports with retry**: Occupancy state changes are now
  sent as explicit ZCL reports with APS acknowledgement tracking. If the coordinator
  doesn't ACK, the firmware retries up to three times at 250 ms / 500 ms / 1 s
  before escalating to the existing soft fallback chain. This replaces the
  send-and-forget ZBoss auto-reporting path.
- **Firmware-side 5-minute keep-alive**: Every 5 minutes the firmware enqueues a
  full-state occupancy burst for all 11 endpoints, so Z2M always has a current
  snapshot even with no motion. Reports are coalesced — a recent state-change
  report for a given endpoint suppresses the keep-alive report for that endpoint.
- **Occupancy read on pair**: Z2M now reads current occupancy from all 11 endpoints
  during the configure phase, so entities are populated immediately after pairing
  rather than showing null until the first motion event.

### Fixes
- **NVS reporting info crash on existing devices**: Calling `esp_zb_zcl_stop_attr_reporting`
  or `esp_zb_zcl_find_reporting_info` on a device whose NVS reporting table was written
  by a previous firmware version could leave the linked list in a corrupt state,
  causing a load-access fault on the next boot. The disable-autoreport path has been
  removed entirely; the firmware now owns all occupancy reports explicitly and never
  touches the ZBoss reporting info table for occupancy.

---

## v2.4.1 - 2026-04-20

### Fixes
- **Reset boot count is now a select**: `diag_reset_boot_count` changed from a binary
  to a Select with a single `Reset` option. This matches how Z2M handles one-shot
  actions. Any automation that wrote `true`/`false` to this entity will need updating.

---

## v2.4.0 - 2026-04-14

### Features
- **WiFi scan-and-connect**: The web UI WiFi tab now scans for nearby networks and
  lets you select one from a list rather than typing the SSID manually.

### Fixes
- **C6 rejoins Z2M after power cycle**: After switching to end-device mode in v2.3.0,
  C6 units were silent on reboot — Z2M had no way to know the device was back and
  the device would only appear after a force-remove and re-pair. C6 now broadcasts
  a ZDO Device_annce on startup to restore the visibility that router mode
  provided automatically.
- **Reduced occupancy report interval**: Occupancy attribute reporting is now more
  responsive under quiet conditions.

---

## v2.3.2 - 2026-04-12

### Fixes
- **Web UI broken after v2.3.1 OTA**: Serving files directly from embedded binary data caused a JS parse failure (trailing null byte appended by the build system). Reverted to SPIFFS-based serving. Web assets are still compiled into the firmware binary — on first boot after an OTA update the device detects the version mismatch and copies the embedded files to SPIFFS before starting the web server.

---

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
