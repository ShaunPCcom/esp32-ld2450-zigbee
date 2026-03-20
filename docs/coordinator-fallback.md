# Coordinator Fallback — Setup and Configuration Guide

The coordinator fallback system allows LD2450-ZB-H2 sensors to continue
controlling lights autonomously when the Zigbee coordinator (Z2M) or Home
Assistant is temporarily unreachable, while avoiding false triggers from
normal network jitter.

---

## How It Works

### Two-Tier Soft/Hard Model

**Soft fallback** — per-endpoint, automatic recovery:
- A single endpoint's APS ACK times out (>2 s by default)
- The sensor immediately sends On/Off directly to its bound light via Zigbee binding
- If any subsequent ACK from the coordinator arrives, all soft fallbacks clear globally
- HA reconciles light state via its automations — no Off command is sent by the firmware
- The `soft_fault` attribute increments so HA can observe and react (e.g. suppress
  occupant count updates during transient jitter)

**Hard fallback** — global, sticky:
- No ACK arrives from any source within `hard_timeout_sec` seconds (default 10 s)
  after the first soft fault
- The sensor enters hard fallback and controls all bound endpoints directly
- Sticky and NVS-backed — survives reboots
- Only cleared by HA writing `fallback_mode = false` (handled automatically by the blueprint)

**Heartbeat watchdog** — software death detection:
- HA sends a periodic "ping" write to the sensor every minute via the blueprint automation
- If no ping arrives within `heartbeat_interval × 2` seconds (default 240 s), the
  sensor enters **hard fallback directly** (no soft phase) — coordinator software is dead
- Covers scenarios where the Zigbee radio (dongle) is still ACKing but HA/Z2M has crashed

### Why Two Tiers?

A single transient APS delay (e.g. 3050 ms due to APS retry) used to cause
immediate sticky hard fallback and dispatch On/Off to bound lights.
The soft tier absorbs these transients: the soft fallback fires, the correct
light turns on, the delayed ACK arrives ~1 s later, soft fallback clears, HA
reconciles. ~1 s disruption, no user-visible effect. Hard fallback only
activates when the coordinator is genuinely unreachable.

---

## Prerequisites

1. **Zigbee bindings** configured for each endpoint/zone that should control a
   light in fallback. Without a binding, the sensor has nowhere to send On/Off.
   Configure bindings in Z2M under **Device → Bind**.

2. **Z2M external converter** (`ld2450_zb_h2.js`) installed and Z2M restarted.

3. **HA blueprint** installed at
   `/config/blueprints/automation/ld2450/ld2450_fallback_watchdog.yaml`.

---

## Device Setup (per sensor, via Z2M)

| Setting | Recommended value | Description |
|---------|-------------------|-------------|
| `fallback_enable` | `on` | Enables the soft/hard fallback system |
| `heartbeat_enable` | `on` | Enables software watchdog |
| `heartbeat_interval` | `120` s | Expected ping interval; watchdog fires at 2× this |
| `hard_timeout_sec` | `10` s | Time from first soft fault to hard fallback escalation |
| `ack_timeout_ms` | `2000` ms | APS ACK timeout before soft fallback fires |
| `fallback_cooldown` | tune per zone | How long to keep light on after Clear in fallback |

### Tuning `ack_timeout_ms`

The default 2000 ms is intentionally aggressive to give fast light response.
If you see frequent soft faults (`soft_fault` incrementing often) on a busy
network, increase to 3000–5000 ms. Data point: APS retries typically resolve
within ~1 s, so 3000 ms is a safe conservative value.

### Tuning `hard_timeout_sec`

10 s default gives a 2–12 s window from coordinator loss to hard fallback
activation. Increase to 20–30 s on networks with occasional multi-second
APS delays to reduce hard fallback entries.

### Fallback cooldown

`fallback_cooldown` (main EP) and `fallback_cooldown_zone_N` (per zone)
control how long the sensor keeps the light on after an Unoccupied report
arrives while in hard fallback. Default 300 s (5 min) keeps lights on
conservatively — tune to your use case.

---

## Blueprint Setup

### Installation

1. Copy `ha/blueprints/ld2450_fallback_watchdog.yaml` to
   `/config/blueprints/automation/ld2450/ld2450_fallback_watchdog.yaml` on
   your HA instance.
2. Go to **Settings → Automations → Blueprints** and reload blueprints.
3. Click **LD2450 Coordinator Fallback Watchdog → Create Automation**.

### Inputs

| Input | Default | Description |
|-------|---------|-------------|
| **LD2450 Sensors** | — | Select the **main occupancy entity** for each sensor (e.g. `binary_sensor.kitchen_sensor_occupancy`). Use the main EP entity, not zone entities. |
| **Z2M bridge entity** | `binary_sensor.zigbee2mqtt_bridge_connection_state` | Z2M bridge connectivity sensor. Used to detect Z2M restarts. |
| **Recovery delay** | 60 s | Wait after HA startup before clearing hard fallback (gives Z2M time to reconnect). |
| **Multi-sensor re-check delay** | 30 s | If only one sensor enters hard fallback while others are normal, wait this long before clearing. Guards against a single sensor failure vs a real outage. |
| **Notify on soft fault** | off | Send a persistent HA notification on each soft fault. Useful for monitoring network jitter during initial deployment. |

### Which entity to select

Select the **main occupancy sensor** for each physical device — the one that
corresponds to EP1 (not zone 1–10 occupancy entities). The `soft_fault` and
`fallback_mode` attributes live on EP1 and are only present on the main entity.

In HA, this is typically named `binary_sensor.<device_name>_occupancy` (without
a zone number suffix).

---

## Recovery Behaviour

The blueprint handles three recovery scenarios automatically:

| Scenario | Trigger | Delay | Action |
|----------|---------|-------|--------|
| Coordinator goes down while HA is running | `hard_fallback_detected` (live state change) | 30 s re-check | Writes `fallback_mode=false` |
| Z2M restarts (HA stays up) | `z2m_reconnect` (bridge entity → on) | 15 s | Checks and clears if in hard fallback |
| HA server reboots | `ha_start` (HA startup event) | 60 s | Checks and clears if in hard fallback |

The delays ensure Z2M and HA automations have time to fully initialise before
the blueprint takes action.

---

## Monitoring

### `soft_fault` attribute

Increments each time a soft fallback fires. Firmware resets it to 0 when the
coordinator ACKs. Observable in Z2M and HA.

- Occasional soft faults (a few per day): normal on a busy network
- Frequent soft faults (multiple per hour): consider increasing `ack_timeout_ms`
- Soft fault immediately followed by `fallback_mode=true`: coordinator genuinely went down

### `fallback_mode` attribute

`false` = normal operation. `true` = hard fallback active. The blueprint clears
this automatically on recovery. If it stays `true` after Z2M and HA are back,
write `false` manually via Z2M or the CLI (`ld fallback clear`).

---

## Limitations

**Bindings are direct On/Off only.** In hard fallback, the sensor dispatches
On/Off to its bound lights. It cannot replicate complex HA automation logic
(e.g. "turn on bathroom light when kitchen sensor detects presence"). Bindings
should target the light physically closest to the sensor's coverage area.

**Cross-room automations are HA's responsibility.** The fallback system is
designed for same-zone direct control. Complex multi-room logic should be
handled by HA automations using the `soft_fault` attribute as a trigger hint.

**Per-EP binding mask not implemented.** If a zone has no binding target
(e.g. a doorway zone with no associated light), it will still dispatch On/Off
in fallback — it simply won't reach any device. This is harmless but wastes
a Zigbee transmission.

---

## CLI Reference

```
ld fallback              — Show fallback status (hard mode, enable, soft faults, timeouts)
ld fallback clear        — Clear hard fallback (equivalent to writing fallback_mode=false)
ld fallback enable 1     — Enable soft/hard fallback system
ld fallback enable 0     — Disable (pure HA mode, no APS probing)
ld fallback timeout 10   — Set hard_timeout_sec (seconds soft→hard escalation)
ld fallback ack-timeout 2000  — Set ack_timeout_ms (APS ACK timeout)
```
