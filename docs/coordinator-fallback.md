# Coordinator Fallback — Setup Guide

When your Zigbee coordinator (Z2M) or Home Assistant goes down, your lights
normally stop responding to presence. The coordinator fallback system lets
LD2450-ZB-H2 sensors keep controlling lights on their own until things recover.

It uses a two-tier approach to avoid false triggers from normal network hiccups
while still providing reliable control during real outages.

---

## What Problem Does This Solve?

Zigbee presence sensors normally report occupancy to Home Assistant, which then
controls your lights via automations. If HA or Z2M goes offline — even for a
few seconds — your lights stop responding.

The fallback system handles three scenarios:

**Brief radio hiccup** (soft fallback): The Zigbee coordinator's radio briefly
drops a response — maybe interference, maybe a congested network. The sensor
immediately turns on the bound light directly. A second later the coordinator
catches up and everything returns to normal automatically. You might not even
notice it happened.

**Real radio outage** (hard fallback): The coordinator's radio is genuinely
unreachable — powered off, out of range, or hardware failure. After a
configurable timeout (default 10 s), the sensor takes over completely, turning
lights on and off based on presence until HA explicitly tells it to stop. This
survives reboots.

**HA or Z2M software crash** (heartbeat watchdog): HA sends a periodic "ping"
to each sensor via the blueprint automation. If the pings stop arriving —
because HA crashed, Z2M stopped, or the server rebooted — the sensor enters
hard fallback. This catches the common case where the Zigbee dongle is still
powered and responding at the radio level, but the software on top of it is
dead.

### Important: Z2M going down ≠ radio going down

Stopping Z2M software does **not** trigger soft fallback. The Zigbee dongle's
radio firmware handles acknowledgements independently of Z2M — so the sensor
still gets radio-level responses even when Z2M isn't running. From the sensor's
perspective, the coordinator's radio is fine.

This is why the **heartbeat watchdog** exists. It's the only way to detect
software-level failures (Z2M stopped, HA crashed, server rebooted) when the
radio hardware is still alive. Without the heartbeat, the sensor would never
know Z2M was gone.

---

## What Are Zigbee Bindings?

A Zigbee binding is a direct link between your sensor and a light, configured
in Z2M. It lets the sensor send On/Off commands straight to the light without
going through Home Assistant.

**Without a binding**, the sensor has nowhere to send commands during fallback —
it's like having a light switch that isn't wired to anything. Bindings are what
make fallback work.

### How to set up bindings

1. Open your sensor's device page in Z2M
2. Go to the **Bind** tab
3. For each endpoint (main sensor or zone) that covers an area with a light:
   - Select the endpoint as the **source**
   - Select the light as the **target**
   - Bind the **On/Off** cluster

### Which endpoints to bind

- **Main endpoint (EP1)**: Bind to the primary light for the sensor's overall
  coverage area
- **Zone endpoints**: Bind each zone to the light that covers that zone's
  physical area — zone 1 might bind to the kitchen light, zone 6 might bind to
  the bathroom light, even if the sensor is mounted in the kitchen

You don't need to bind every endpoint. Zones without bindings simply won't
control any light during fallback (which is fine — they just have nothing to do).

---

## Setup

### Step 1: Configure bindings

Set up Zigbee bindings as described above.

### Step 2: Install the Z2M external converter

Copy `z2m/ld2450_zb_h2.js` to your Z2M `external_converters/` directory and
restart Z2M.

### Step 3: Enable fallback on each sensor (via Z2M)

Open the sensor's device page in Z2M and set:

| Setting | Recommended value | Description |
|---------|-------------------|-------------|
| `fallback_enable` | ON | Activates the fallback system |
| `heartbeat_enable` | ON | Activates the software watchdog |
| `heartbeat_interval` | `120` s | How often the sensor expects a ping (watchdog fires at 2x this) |
| `hard_timeout_sec` | `10` s | Seconds before a brief hiccup escalates to full fallback |
| `ack_timeout_ms` | `2000` ms | How long to wait for a coordinator response |
| `fallback_cooldown` | per zone | How long to keep the light on after presence clears during fallback |

All settings are in the **Exposes** tab of the Z2M device page.

### Step 4: Install the HA blueprint

Copy `ha/blueprints/ld2450_fallback_watchdog.yaml` to your HA instance at:

```
/config/blueprints/automation/ld2450/ld2450_fallback_watchdog.yaml
```

You can copy the file using the **File Editor** addon, **Studio Code Server**
addon, **Samba** share, or **SSH**.

Then go to **Settings → Automations → Blueprints** and click the reload button.

### Step 5: Create the automation

Click **LD2450 Coordinator Fallback Watchdog → Create Automation** and configure:

**LD2450 Sensors** — Select the **main occupancy entity** for each sensor. This
is the one named `binary_sensor.<device_name>_occupancy` (without a zone number).
Do not select zone occupancy entities — the fallback attributes (`soft_fault`,
`fallback_mode`) only exist on the main entity.

| Input | Default | Description |
|-------|---------|-------------|
| **LD2450 Sensors** | — | Main occupancy entity per sensor (see above) |
| **Z2M bridge entity** | `binary_sensor.zigbee2mqtt_bridge_connection_state` | Tracks Z2M connectivity for restart recovery |
| **Recovery delay** | 60 s | Wait after HA startup before clearing fallback |
| **Multi-sensor re-check delay** | 30 s | Wait before clearing if only one sensor entered fallback |
| **Notify on soft fault** | off | Persistent notification on network hiccups (useful during initial deployment) |

---

## Recovery Behaviour

The blueprint automatically clears hard fallback in all four recovery scenarios:

| Scenario | How recovery works | Delay |
|----------|-------------------|-------|
| Coordinator radio goes down while HA is running | Blueprint sees `fallback_mode` turn on, waits, then clears it | 30 s |
| Heartbeat watchdog fires, then HA/Z2M recovers | Next heartbeat ping goes through, blueprint sees hard fallback is active and clears it | ~1 min (next heartbeat cycle) |
| Z2M restarts (HA stays up) | Blueprint detects Z2M bridge reconnection, checks and clears | 15 s |
| HA server reboots | Blueprint runs on HA startup, checks and clears | 60 s |

The delays give Z2M and HA time to fully start up before the blueprint takes action.

---

## Monitoring

Both attributes are visible in the Z2M device page under the **Exposes** tab.

### `soft_fault` — network hiccup counter

Increments each time a soft fallback fires. Resets to 0 automatically when
the coordinator responds.

- **A few per day**: Normal on a busy Zigbee network
- **Multiple per hour**: Your network is congested — increase `ack_timeout_ms`
  to 3000–5000 ms
- **Immediately followed by `fallback_mode` turning on**: The coordinator
  genuinely went down

### `fallback_mode` — hard fallback status

`false` = normal operation. `true` = hard fallback active.

The blueprint clears this automatically on recovery. If it stays `true` after
Z2M and HA are both back and running, clear it manually in Z2M or via the
CLI (`ld fallback clear`).

---

## Tuning

### `ack_timeout_ms` (default: 2000 ms)

How long the sensor waits for a coordinator response before triggering soft
fallback. The default 2000 ms is aggressive — it prioritises fast light
response over avoiding soft faults. Increase to 3000–5000 ms if you see
frequent soft faults. Zigbee retries typically resolve within ~1 s, so 3000 ms
is a good conservative starting point.

### `hard_timeout_sec` (default: 10 s)

How long after the first soft fault before escalating to hard fallback. The
default gives a 2–12 s window from coordinator loss to full fallback activation.
Increase to 20–30 s on networks with occasional multi-second delays.

### `fallback_cooldown` (default: 300 s)

How long the sensor keeps the light on after presence clears during hard
fallback. Per-zone cooldowns (`fallback_cooldown_zone_N`) are also available.
Default 5 minutes keeps lights on conservatively — adjust based on how quickly
you want lights to turn off when a room empties during an outage.

---

## Limitations

**Fallback can only send On/Off commands.** Brightness levels, colour
temperature, time-of-day conditions, and similar logic are HA's responsibility
and resume automatically once the coordinator recovers.

**Soft fallback only detects radio-level failures.** If Z2M stops but the
Zigbee dongle is still powered, the sensor won't notice — it still gets
radio-level acknowledgements. Enable the heartbeat watchdog to catch
software-level failures.

---

## CLI Reference

These commands are for direct hardware access via a USB serial terminal.
They are not needed for normal setup — all settings are configurable via Z2M.

```
ld fallback              Show fallback status
ld fallback clear        Clear hard fallback
ld fallback enable 1     Enable the fallback system
ld fallback enable 0     Disable (pure HA mode)
ld fallback timeout 10   Set hard_timeout_sec
ld fallback ack-timeout 2000  Set ack_timeout_ms
```
