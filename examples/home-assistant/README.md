# Home Assistant Dashboard Examples

This directory contains Home Assistant dashboard examples for visualizing and configuring the LD2450 Zigbee sensor.

## Prerequisites

1. **Plotly Graph Card** installed via HACS:
   - In Home Assistant, go to **HACS** → **Frontend**
   - Search for "Plotly Graph Card"
   - Install and restart Home Assistant

2. **Sensor paired with Zigbee2MQTT**:
   - Device should be paired and showing all entities in Home Assistant
   - Note your device's Zigbee IEEE address (e.g., `0x1234567890abcdef`)

## Files

### `plotly-card.yaml`

Standalone Plotly graph card showing:
- **3 target positions** (real-time X/Y coordinates)
- **10 zone boundaries** (active zones only — disabled zones are invisible)
- **Coverage area** (sensor detection range overlay)

**Usage:**
1. Open the file and replace `YOUR_DEVICE_ID` with your device's Zigbee IEEE address
2. In Home Assistant, create a new manual Lovelace card
3. Paste the modified YAML content

### `zone-dashboard.yaml`

Complete dashboard tab with:
- **Zone coordinate inputs** (vertex count selector + CSV text input per zone, shown for zones 1–5)
- **Plotly graph visualization** (all 10 zones embedded in the dashboard)
- **Occupancy badges** (quick status for zones 1–5)
- **Control buttons** (restart device, toggle tracking mode/coordinates)

**Usage:**
1. Open the file and replace all instances of `YOUR_DEVICE_ID` with your Zigbee IEEE address
2. In Home Assistant, go to **Settings** → **Dashboards** → **Add Dashboard**
3. Choose "New dashboard from scratch"
4. Edit dashboard → Three dots menu → "Raw configuration editor"
5. Paste the modified YAML content
6. Customize zone names and headings as needed
7. Copy and rename zone sections 1–5 for zones 6–10 if needed

## Configuration Workflow

### Initial Zone Setup

1. **Enable coordinate publishing**:
   - Toggle `switch.YOUR_DEVICE_ID_coord_publishing` to ON

2. **Enable single target mode**:
   - Toggle `switch.YOUR_DEVICE_ID_tracking_mode` to Single Target
   - This makes it easier to test one zone at a time

3. **Configure zones**:
   - Use the Plotly graph to visualize the sensor coverage area
   - For each zone, select the number of vertices (3–10) using the vertex count selector
   - Enter the polygon coordinates as a flat CSV in metres: `x1,y1,x2,y2,x3,y3,...`
   - Walk through each zone and verify the occupancy binary sensor triggers
   - Adjust coordinates as needed

4. **Return to normal operation**:
   - Toggle `switch.YOUR_DEVICE_ID_coord_publishing` to OFF (reduces Zigbee traffic)
   - Toggle `switch.YOUR_DEVICE_ID_tracking_mode` to Multi Target (track up to 3 people)

### Coordinate System

- **Origin (0,0)**: Sensor location
- **X-axis**: Left/right (−6 to +6 metres)
  - Negative X = Left of sensor
  - Positive X = Right of sensor
- **Y-axis**: Forward distance (0 to 6 metres)
  - 0 = Sensor location
  - 6 = 6 metres forward

Coordinates are in **metres**. Example triangle zone covering a couch 2–4 metres forward, 1 metre wide:
- Vertex 1: `-0.5,2.0` (left-front corner)
- Vertex 2: `0.5,2.0` (right-front corner)
- Vertex 3: `0.0,4.0` (back-centre)
- Coords field: `-0.5,2.0,0.5,2.0,0.0,4.0`

A rectangle (4 vertices) covering the same area:
- Coords field: `-0.5,2.0,0.5,2.0,0.5,4.0,-0.5,4.0`

### Zone Disable

Set the vertex count to `0` to disable a zone. This clears the coordinates and stops occupancy detection for that zone. The zone trace disappears from the Plotly graph.

## Attribution

Plotly graph visualization adapted from **Anthony Hua's LD2450-map-chart.yaml**:
https://github.com/athua/ha-utils/blob/main/LD2450-map-chart.yaml

Modified for:
- Zigbee entity naming (vs ESPHome)
- 10 zones with variable vertex count (3–10 vertices, vs fixed 4-vertex rectangles)
- CSV coordinate format via `$fn` expressions
- 6 metre coverage (vs 7.5 metre)

## Tips

- **Zone overlap**: Zones can overlap. A target in multiple zones triggers all overlapping zone occupancy sensors.
- **Variable vertices**: Use triangles (3) for simple areas, rectangles (4) for rooms, and higher counts for irregular spaces.
- **Zone disable**: Set vertex count to 0 to disable a zone without losing its position in the zone list.
- **Testing**: Stand still in a zone for 2–3 seconds to ensure stable detection before moving to test the next zone.
- **Automation delays**: Add a small delay (1–2 seconds) in automations to avoid triggering on brief passes through zones.

## Troubleshooting

**Graph not showing targets:**
- Ensure coordinate publishing is enabled (`switch.YOUR_DEVICE_ID_coord_publishing`)
- Check that targets are detected (`sensor.YOUR_DEVICE_ID_target_count` > 0)
- Verify Plotly Graph Card is installed via HACS

**Zone not showing on graph:**
- Confirm the zone is enabled (vertex count ≥ 3)
- Check the coords field contains a valid CSV with the correct number of values (2 × vertex count)
- Verify the `text.YOUR_DEVICE_ID_zone_N_coords` entity state in Developer Tools

**Zone not triggering occupancy:**
- Verify zone coordinates form a valid polygon (no self-intersecting edges)
- Check that you're within the sensor's detection range
- Ensure `coord_publishing` is ON and you can see yourself as a target on the graph
- Try increasing the zone size slightly

**Entities not found:**
- Verify device is paired with Zigbee2MQTT
- Hit "Reconfigure" on the device in Z2M settings
- Check that all entity IDs use your actual Zigbee IEEE address
