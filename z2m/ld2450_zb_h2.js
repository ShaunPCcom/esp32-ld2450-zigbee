// SPDX-License-Identifier: MIT
'use strict';

// Zigbee2MQTT external converter for LD2450-ZB-H2 mmWave presence sensor.
// Zero external requires - works with any Z2M version (tested Z2M 2.8.0).
// Place this file in your Z2M data/external_converters/ directory.

// ---- ZCL data type constants (inline to avoid require('zigbee-herdsman')) ----
const ZCL_UINT8    = 0x20;
const ZCL_UINT16   = 0x21;
const ZCL_UINT32   = 0x23;
const ZCL_CHAR_STR = 0x42;

// ---- Expose access flags ----
const ACCESS_STATE = 0b001; // 1
const ACCESS_SET   = 0b010; // 2
const ACCESS_ALL   = 0b111; // 7

// ---- Cluster ID ----
const CLUSTER_CONFIG_ID = 0xFC00;

// ---- Zone config attribute layout ----
// Base formula: 0x0040 + n*4 (n = 0..9, firmware 0-indexed, Z2M 1-indexed)
// Sub-attrs: +0 = vertex_count (U8), +1 = coords (CHAR_STR), +2 = cooldown (U16), +3 = delay (U16)
const zoneConfigAttrs = {};
for (let n = 0; n < 10; n++) {
    const base = 0x0040 + n * 4;
    zoneConfigAttrs[`zone${n + 1}VertexCount`] = {ID: base + 0, type: ZCL_UINT8,    write: true};
    zoneConfigAttrs[`zone${n + 1}Coords`]      = {ID: base + 1, type: ZCL_CHAR_STR, write: true};
    zoneConfigAttrs[`zone${n + 1}Cooldown`]    = {ID: base + 2, type: ZCL_UINT16,   write: true};
    zoneConfigAttrs[`zone${n + 1}Delay`]       = {ID: base + 3, type: ZCL_UINT16,   write: true};
}

// ---- Fallback cooldown attribute layout ----
const fallbackCooldownAttrs = {};
for (let n = 0; n < 10; n++) {
    fallbackCooldownAttrs[`fallbackZone${n + 1}Cooldown`] = {ID: 0x0070 + n, type: ZCL_UINT16, write: true};
}

// ---- Custom cluster definition ----
// Attribute ID lookup table (used for fromZigbee and configure reads)
const ATTR = {
    targetCount:       0x0000,
    targetCoords:      0x0001,
    maxDistance:       0x0010,
    angleLeft:         0x0011,
    angleRight:        0x0012,
    trackingMode:      0x0020,
    coordPublishing:   0x0021,
    occupancyCooldown: 0x0022,
    occupancyDelay:    0x0023,
    fallbackMode:      0x0024,
    fallbackCooldown:  0x0025,
    heartbeatEnable:   0x0026,
    heartbeatInterval: 0x0027,
    heartbeat:         0x0028,
    fallbackEnable:    0x0029,
    hardTimeoutSec:    0x002B,
    ackTimeoutMs:      0x002C,
    bootCount:         0x0030,
    resetReason:       0x0031,
    lastUptimeSec:     0x0032,
    minFreeHeap:       0x0033,
    restart:           0x00F0,
    factoryReset:      0x00F1,
};
// Zone attrs: base = 0x0040 + n*4 (n=0..9); sub: +0=vertexCount, +1=coords, +2=cooldown, +3=delay
// Fallback zone cooldown: 0x0070 + n (n=0..9)
const CLUSTER_NAME = 'ld2450Config';

const ld2450ConfigCluster = {
    name: CLUSTER_NAME,
    ID: CLUSTER_CONFIG_ID,
    attributes: {
        targetCount:          {ID: 0x0000, type: ZCL_UINT8,    report: true},
        targetCoords:         {ID: 0x0001, type: ZCL_CHAR_STR, report: true},
        maxDistance:          {ID: 0x0010, type: ZCL_UINT16,   write: true},
        angleLeft:            {ID: 0x0011, type: ZCL_UINT8,    write: true},
        angleRight:           {ID: 0x0012, type: ZCL_UINT8,    write: true},
        trackingMode:         {ID: 0x0020, type: ZCL_UINT8,    write: true},
        coordPublishing:      {ID: 0x0021, type: ZCL_UINT8,    write: true},
        occupancyCooldown:    {ID: 0x0022, type: ZCL_UINT16,   write: true},
        occupancyDelay:       {ID: 0x0023, type: ZCL_UINT16,   write: true},
        fallbackMode:         {ID: 0x0024, type: ZCL_UINT8,    write: true, report: true},
        fallbackCooldown:     {ID: 0x0025, type: ZCL_UINT16,   write: true},
        heartbeatEnable:      {ID: 0x0026, type: ZCL_UINT8,    write: true},
        heartbeatInterval:    {ID: 0x0027, type: ZCL_UINT16,   write: true},
        heartbeat:            {ID: 0x0028, type: ZCL_UINT8,    write: true},
        fallbackEnable:       {ID: 0x0029, type: ZCL_UINT8,    write: true},

        hardTimeoutSec:       {ID: 0x002B, type: ZCL_UINT8,    write: true},
        ackTimeoutMs:         {ID: 0x002C, type: ZCL_UINT16,   write: true},
        bootCount:            {ID: 0x0030, type: ZCL_UINT32,   report: false},
        resetReason:          {ID: 0x0031, type: ZCL_UINT8,    report: false},
        lastUptimeSec:        {ID: 0x0032, type: ZCL_UINT32,   report: false},
        minFreeHeap:          {ID: 0x0033, type: ZCL_UINT32,   report: false},
        restart:              {ID: 0x00F0, type: ZCL_UINT8,    write: true},
        factoryReset:         {ID: 0x00F1, type: ZCL_UINT8,    write: true},
        ...zoneConfigAttrs,
        ...fallbackCooldownAttrs,
    },
    commands: {},
    commandsResponse: {},
};

function registerCustomClusters(device) {
    device.addCustomCluster('ld2450Config', ld2450ConfigCluster);
}

// ---- CSV unit conversion helpers ----
// Firmware stores coordinates in mm; Z2M exposes them in metres.

// mm CSV string → metres CSV string  (e.g. "100,-200,300,400" → "0.1,-0.2,0.3,0.4")
function mmCsvToMetres(mmCsv) {
    if (!mmCsv || mmCsv.trim() === '') return '';
    return mmCsv.split(',').map(v => String(Number(v) / 1000)).join(',');
}

// metres CSV string → mm CSV string  (e.g. "0.1,-0.2,0.3,0.4" → "100,-200,300,400")
function metresCsvToMm(mCsv) {
    if (!mCsv || mCsv.trim() === '') return '';
    return mCsv.split(',').map(v => String(Math.round(Number(v) * 1000))).join(',');
}

// ---- Expose helpers (inline, no require) ----

function binaryExpose(property, label, access, valueOn, valueOff, description, name) {
    return {type: 'binary', name: name || property, label, property, access,
        value_on: valueOn, value_off: valueOff, description};
}

function numericExpose(name, label, access, description, opts) {
    const e = {type: 'numeric', name, label, property: name, access, description};
    if (opts) Object.assign(e, opts);
    return e;
}

function enumExpose(name, label, access, values, description) {
    return {type: 'enum', name, label, property: name, access, values, description};
}

function textExpose(name, label, access, description) {
    return {type: 'text', name, label, property: name, access, description};
}

// ---- fromZigbee converters ----

const fzLocal = {
    occupancy: {
        cluster: 'msOccupancySensing',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.data.occupancy === undefined) return {};
            const val = msg.data.occupancy === 1;
            const ep = msg.endpoint.ID;
            if (ep === 1) return {occupancy: val};
            const zone = ep - 1;
            if (zone >= 1 && zone <= 10) return {[`zone_${zone}_occupancy`]: val};
            return {};
        },
    },

    config: {
        // Z2M dispatches unregistered custom clusters as the string representation of the
        // numeric ID (e.g. '64512'), not the number. Use String() to ensure strict === match.
        cluster: String(CLUSTER_CONFIG_ID),
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            meta.logger.debug(`[ZB_LD2450] cluster 0xFC00 frame: ep=${msg.endpoint.ID} type=${msg.type} keys=${Object.keys(msg.data).join(',')}`);
            const result = {};
            const d = msg.data;

            if (d[ATTR.targetCount] !== undefined)     result.target_count       = d[ATTR.targetCount];
            if (d[ATTR.maxDistance] !== undefined)     result.max_distance       = d[ATTR.maxDistance] / 1000;
            if (d[ATTR.angleLeft] !== undefined)       result.angle_left         = d[ATTR.angleLeft];
            if (d[ATTR.angleRight] !== undefined)      result.angle_right        = d[ATTR.angleRight];
            if (d[ATTR.trackingMode] !== undefined)    result.tracking_mode      = d[ATTR.trackingMode] === 0;
            if (d[ATTR.coordPublishing] !== undefined) result.coord_publishing   = d[ATTR.coordPublishing] === 1;
            if (d[ATTR.occupancyCooldown] !== undefined) result.occupancy_cooldown = d[ATTR.occupancyCooldown];
            if (d[ATTR.occupancyDelay] !== undefined)    result.occupancy_delay    = d[ATTR.occupancyDelay];
            if (d[ATTR.fallbackMode] !== undefined)      result.fallback_mode      = d[ATTR.fallbackMode] === 1;
            if (d[ATTR.fallbackCooldown] !== undefined)  result.fallback_cooldown  = d[ATTR.fallbackCooldown];
            if (d[ATTR.fallbackEnable] !== undefined)    result.fallback_enable    = d[ATTR.fallbackEnable] === 1;

            if (d[ATTR.hardTimeoutSec] !== undefined)    result.hard_timeout_sec   = d[ATTR.hardTimeoutSec];
            if (d[ATTR.ackTimeoutMs] !== undefined)      result.ack_timeout_ms     = d[ATTR.ackTimeoutMs];
            if (d[ATTR.heartbeatEnable] !== undefined)   result.heartbeat_enable   = d[ATTR.heartbeatEnable] === 1;
            if (d[ATTR.heartbeatInterval] !== undefined) result.heartbeat_interval = d[ATTR.heartbeatInterval];

            if (d[ATTR.bootCount] !== undefined)     result.boot_count     = d[ATTR.bootCount];
            if (d[ATTR.resetReason] !== undefined)   result.reset_reason   = d[ATTR.resetReason];
            if (d[ATTR.lastUptimeSec] !== undefined) result.last_uptime_sec = d[ATTR.lastUptimeSec];
            if (d[ATTR.minFreeHeap] !== undefined)   result.min_free_heap  = d[ATTR.minFreeHeap];

            if (d[ATTR.targetCoords] !== undefined) {
                const str = d[ATTR.targetCoords] || '';
                const parts = str.split(';').filter(Boolean);
                for (let i = 0; i < 3; i++) {
                    if (i < parts.length) {
                        const [x, y] = parts[i].split(',').map(Number);
                        result[`target_${i + 1}_x`] = isNaN(x) ? 0 : x / 1000;
                        result[`target_${i + 1}_y`] = isNaN(y) ? 0 : y / 1000;
                    } else {
                        result[`target_${i + 1}_x`] = 0;
                        result[`target_${i + 1}_y`] = 0;
                    }
                }
            }

            /* Zone config attrs: base = 0x0040 + n*4 (n=0..9, z=n+1) */
            for (let n = 0; n < 10; n++) {
                const z = n + 1;
                const base = 0x0040 + n * 4;
                const vc = d[base + 0];
                const cs = d[base + 1];
                const cl = d[base + 2];
                const dl = d[base + 3];
                const fc = d[0x0070 + n];

                if (vc !== undefined) result[`zone_${z}_vertex_count`]      = String(vc);
                if (cs !== undefined) result[`zone_${z}_coords`]            = mmCsvToMetres(cs || '');
                if (cl !== undefined) result[`zone_${z}_cooldown`]          = cl;
                if (dl !== undefined) result[`zone_${z}_delay`]             = dl;
                if (fc !== undefined) result[`fallback_cooldown_zone_${z}`] = fc;
            }

            return result;
        },
    },
};

// ---- toZigbee converters ----

const tzLocal = {
    config: {
        key: [
            'max_distance', 'angle_left', 'angle_right', 'tracking_mode', 'coord_publishing',
            'occupancy_cooldown', 'occupancy_delay',
            'fallback_mode', 'fallback_cooldown',
            'fallback_enable', 'hard_timeout_sec', 'ack_timeout_ms',
            'heartbeat_enable', 'heartbeat_interval', 'heartbeat',
            ...Array.from({length: 10}, (_, i) => `fallback_cooldown_zone_${i + 1}`),
            ...Array.from({length: 10}, (_, i) => [
                `zone_${i + 1}_vertex_count`,
                `zone_${i + 1}_coords`,
                `zone_${i + 1}_cooldown`,
                `zone_${i + 1}_delay`,
            ]).flat(),
        ],
        convertSet: async (entity, key, value, meta) => {
            registerCustomClusters(meta.device);
            const ep1 = meta.device.getEndpoint(1);

            /* Zone vertex_count — append/truncate existing coords to match new count */
            const zoneVcMatch = key.match(/^zone_(\d+)_vertex_count$/);
            if (zoneVcMatch) {
                const z = parseInt(zoneVcMatch[1]);
                const n = z - 1;
                const zoneEp = meta.device.getEndpoint(n + 2);
                const validVc = Math.max(0, parseInt(value) || 0);
                const coordsKey = `zone_${z}_coords`;
                const stateUpdate = {[key]: String(validVc)};

                const zoneBase = 0x0040 + n * 4;
                await zoneEp.write(CLUSTER_CONFIG_ID, {[zoneBase]: {value: validVc, type: ZCL_UINT8}});

                if (validVc > 0) {
                    /* Parse current pairs from state (metres CSV) */
                    const curCoords = (meta.state || {})[coordsKey] || '';
                    const tokens = curCoords ? curCoords.split(',').map(s => s.trim()).filter(Boolean) : [];
                    const pairs = [];
                    for (let i = 0; i + 1 < tokens.length; i += 2) pairs.push([tokens[i], tokens[i + 1]]);

                    /* Append zero pairs if growing; truncate if shrinking */
                    while (pairs.length < validVc) pairs.push(['0', '0']);
                    pairs.length = validVc;

                    const metresCoords = pairs.map(p => p.join(',')).join(',');
                    try {
                        await zoneEp.write(CLUSTER_CONFIG_ID, {[zoneBase + 1]: {value: metresCsvToMm(metresCoords), type: ZCL_CHAR_STR}});
                        stateUpdate[coordsKey] = metresCoords;
                    } catch (e) {
                        meta.logger.warn(`[ZB_LD2450] Coords resize failed: ${e.message}`);
                    }
                } else {
                    /* Zone disabled — clear cached coords in Z2M state */
                    stateUpdate[coordsKey] = '';
                }

                return {state: stateUpdate};
            }

            /* Zone coords (metres CSV → mm CSV for firmware) */
            const zoneCoordsMatch = key.match(/^zone_(\d+)_coords$/);
            if (zoneCoordsMatch) {
                const n = parseInt(zoneCoordsMatch[1]) - 1;
                const zoneEp = meta.device.getEndpoint(n + 2);
                const mmCsv = metresCsvToMm(value);
                await zoneEp.write(CLUSTER_CONFIG_ID, {[0x0040 + n * 4 + 1]: {value: mmCsv, type: ZCL_CHAR_STR}});
                return {state: {[key]: value}};
            }

            /* Zone cooldown */
            const zoneCoolMatch = key.match(/^zone_(\d+)_cooldown$/);
            if (zoneCoolMatch) {
                const n = parseInt(zoneCoolMatch[1]) - 1;
                const zoneEp = meta.device.getEndpoint(n + 2);
                await zoneEp.write(CLUSTER_CONFIG_ID, {[0x0040 + n * 4 + 2]: {value, type: ZCL_UINT16}});
                return {state: {[key]: value}};
            }

            /* Zone delay */
            const zoneDelayMatch = key.match(/^zone_(\d+)_delay$/);
            if (zoneDelayMatch) {
                const n = parseInt(zoneDelayMatch[1]) - 1;
                const zoneEp = meta.device.getEndpoint(n + 2);
                await zoneEp.write(CLUSTER_CONFIG_ID, {[0x0040 + n * 4 + 3]: {value, type: ZCL_UINT16}});
                return {state: {[key]: value}};
            }

            /* Fallback zone cooldown (fallback_cooldown_zone_N → 0x0070+N, stays on EP1) */
            const fbZoneCoolMatch = key.match(/^fallback_cooldown_zone_(\d+)$/);
            if (fbZoneCoolMatch) {
                const n = parseInt(fbZoneCoolMatch[1]) - 1;  /* 0-indexed */
                await ep1.write(CLUSTER_CONFIG_ID, {[0x0070 + n]: {value, type: ZCL_UINT16}});
                return {state: {[key]: value}};
            }

            /* Main endpoint config */
            const map = {
                max_distance:       {id: ATTR.maxDistance,       type: ZCL_UINT16, val: (v) => Math.round(v * 1000)},
                angle_left:         {id: ATTR.angleLeft,         type: ZCL_UINT8,  val: (v) => v},
                angle_right:        {id: ATTR.angleRight,        type: ZCL_UINT8,  val: (v) => v},
                tracking_mode:      {id: ATTR.trackingMode,      type: ZCL_UINT8,  val: (v) => v ? 0 : 1},
                coord_publishing:   {id: ATTR.coordPublishing,   type: ZCL_UINT8,  val: (v) => v ? 1 : 0},
                occupancy_cooldown: {id: ATTR.occupancyCooldown, type: ZCL_UINT16, val: (v) => v},
                occupancy_delay:    {id: ATTR.occupancyDelay,    type: ZCL_UINT16, val: (v) => v},
                fallback_mode:      {id: ATTR.fallbackMode,      type: ZCL_UINT8,  val: (v) => v ? 1 : 0},
                fallback_cooldown:  {id: ATTR.fallbackCooldown,  type: ZCL_UINT16, val: (v) => v},
                heartbeat_enable:   {id: ATTR.heartbeatEnable,   type: ZCL_UINT8,  val: (v) => v ? 1 : 0},
                heartbeat_interval: {id: ATTR.heartbeatInterval, type: ZCL_UINT16, val: (v) => v},
                heartbeat:          {id: ATTR.heartbeat,         type: ZCL_UINT8,  val: (_) => 1},
                fallback_enable:    {id: ATTR.fallbackEnable,    type: ZCL_UINT8,  val: (v) => v ? 1 : 0},
                hard_timeout_sec:   {id: ATTR.hardTimeoutSec,    type: ZCL_UINT8,  val: (v) => v},
                ack_timeout_ms:     {id: ATTR.ackTimeoutMs,      type: ZCL_UINT16, val: (v) => v},
            };
            const m = map[key];
            if (m) {
                await ep1.write(CLUSTER_CONFIG_ID, {[m.id]: {value: m.val(value), type: m.type}});
                return {state: {[key]: value}};
            }
        },

        convertGet: async (entity, key, meta) => {
            registerCustomClusters(meta.device);
            const ep1 = meta.device.getEndpoint(1);

            /* Zone attrs — read from the zone's own EP */
            const zoneMatch = key.match(/^zone_(\d+)_(vertex_count|coords|cooldown|delay)$/);
            if (zoneMatch) {
                const n = parseInt(zoneMatch[1]) - 1;
                const zoneEp = meta.device.getEndpoint(n + 2);
                const subOffset = {vertex_count: 0, coords: 1, cooldown: 2, delay: 3};
                await zoneEp.read(CLUSTER_CONFIG_ID, [0x0040 + n * 4 + subOffset[zoneMatch[2]]]);
                return;
            }

            /* Fallback zone cooldown get */
            const fbZoneCoolGetMatch = key.match(/^fallback_cooldown_zone_(\d+)$/);
            if (fbZoneCoolGetMatch) {
                const n = parseInt(fbZoneCoolGetMatch[1]) - 1;
                await ep1.read(CLUSTER_CONFIG_ID, [0x0070 + n]);
                return;
            }

            /* Main endpoint config */
            const attrIds = {
                max_distance: ATTR.maxDistance, angle_left: ATTR.angleLeft,
                angle_right: ATTR.angleRight, tracking_mode: ATTR.trackingMode,
                coord_publishing: ATTR.coordPublishing, occupancy_cooldown: ATTR.occupancyCooldown,
                occupancy_delay: ATTR.occupancyDelay,
                fallback_mode: ATTR.fallbackMode, fallback_cooldown: ATTR.fallbackCooldown,
                fallback_enable: ATTR.fallbackEnable,
                hard_timeout_sec: ATTR.hardTimeoutSec, ack_timeout_ms: ATTR.ackTimeoutMs,
                heartbeat_enable: ATTR.heartbeatEnable, heartbeat_interval: ATTR.heartbeatInterval,
            };
            if (attrIds[key] !== undefined) await ep1.read(CLUSTER_CONFIG_ID, [attrIds[key]]);
        },
    },

    restart: {
        key: ['restart'],
        convertSet: async (entity, key, value, meta) => {
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);
            await ep.write(CLUSTER_CONFIG_ID, {[ATTR.restart]: {value: 1, type: ZCL_UINT8}});
            return {state: {restart: ''}};
        },
    },

    factory_reset: {
        key: ['factory_reset_confirm'],
        convertSet: async (entity, key, value, meta) => {
            if (value !== 'factory-reset') {
                meta.logger.warn('[ZB_LD2450] Factory reset not triggered: type "factory-reset" to confirm');
                return;
            }
            meta.logger.warn('[ZB_LD2450] Factory reset triggered via Z2M');
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);
            await ep.write(CLUSTER_CONFIG_ID, {[ATTR.factoryReset]: {value: 0xFE, type: ZCL_UINT8}}, {disableDefaultResponse: true});
        },
    },
};

// ---- Expose definitions ----

const exposesDefinition = [
    binaryExpose('occupancy', 'Occupancy', ACCESS_STATE, true, false,
        'Overall presence detected in sensing area'),

    ...Array.from({length: 10}, (_, i) =>
        binaryExpose(`zone_${i + 1}_occupancy`, `Zone ${i + 1} occupancy`, ACCESS_STATE, true, false,
            `Presence detected in zone ${i + 1}`)
    ),

    numericExpose('target_count', 'Target count', ACCESS_STATE,
        'Number of detected targets', {value_min: 0, value_max: 3}),

    ...Array.from({length: 3}, (_, i) => [
        numericExpose(`target_${i + 1}_x`, `Target ${i + 1} X`, ACCESS_STATE,
            `Target ${i + 1} X coordinate`, {unit: 'm'}),
        numericExpose(`target_${i + 1}_y`, `Target ${i + 1} Y`, ACCESS_STATE,
            `Target ${i + 1} Y coordinate`, {unit: 'm'}),
    ]).flat(),

    numericExpose('max_distance', 'Max distance', ACCESS_ALL,
        'Maximum detection distance', {unit: 'm', value_min: 0, value_max: 6, value_step: 0.01}),

    numericExpose('angle_left', 'Angle left', ACCESS_ALL,
        'Left angle limit of detection zone', {unit: '°', value_min: 0, value_max: 90, value_step: 1}),

    numericExpose('angle_right', 'Angle right', ACCESS_ALL,
        'Right angle limit of detection zone', {unit: '°', value_min: 0, value_max: 90, value_step: 1}),

    binaryExpose('tracking_mode', 'Multi target', ACCESS_ALL, true, false,
        'Multi-target tracking (off = single target)'),

    binaryExpose('coord_publishing', 'Coordinate publishing', ACCESS_ALL, true, false,
        'Enable publishing of target coordinates'),

    numericExpose('occupancy_cooldown', 'Occupancy cooldown', ACCESS_ALL,
        'Minimum time before reporting Clear (main sensor)', {unit: 's', value_min: 0, value_max: 300, value_step: 1}),

    numericExpose('occupancy_delay', 'Occupancy delay', ACCESS_ALL,
        'Delay before reporting Occupied (main sensor)', {unit: 'ms', value_min: 0, value_max: 65535, value_step: 1}),

    /* Zone config (1-10): vertex_count, coords (metres CSV), cooldown, delay */
    ...Array.from({length: 10}, (_, i) => [
        enumExpose(`zone_${i + 1}_vertex_count`, `Zone ${i + 1} vertex count`, ACCESS_ALL,
            ['0', '3', '4', '5', '6', '7', '8', '9', '10'],
            `Number of zone ${i + 1} vertices (0 = disabled, 3–10 = active polygon)`),
        textExpose(`zone_${i + 1}_coords`, `Zone ${i + 1} coords`, ACCESS_ALL,
            `Zone ${i + 1} polygon vertices in metres as CSV: x1,y1,x2,y2,...`),
        numericExpose(`zone_${i + 1}_cooldown`, `Zone ${i + 1} cooldown`, ACCESS_ALL,
            `Minimum time before reporting Clear for zone ${i + 1}`,
            {unit: 's', value_min: 0, value_max: 300, value_step: 1}),
        numericExpose(`zone_${i + 1}_delay`, `Zone ${i + 1} delay`, ACCESS_ALL,
            `Delay before reporting Occupied for zone ${i + 1}`,
            {unit: 'ms', value_min: 0, value_max: 65535, value_step: 1}),
    ]).flat(),

    /* Coordinator fallback */
    binaryExpose('fallback_mode', 'Fallback mode', ACCESS_ALL, true, false,
        'Active when coordinator is offline and device is controlling lights directly via binding. ' +
        'Firmware sets this on ACK timeout; HA clears it (write false) to resume normal operation.'),

    numericExpose('fallback_cooldown', 'Fallback cooldown (main)', ACCESS_ALL,
        'How long to keep main EP light on after Clear in fallback mode',
        {unit: 's', value_min: 0, value_max: 600, value_step: 1}),

    ...Array.from({length: 10}, (_, i) =>
        numericExpose(`fallback_cooldown_zone_${i + 1}`, `Fallback cooldown zone ${i + 1}`, ACCESS_ALL,
            `How long to keep zone ${i + 1} light on after Clear in fallback mode`,
            {unit: 's', value_min: 0, value_max: 600, value_step: 1})
    ),

    /* Software watchdog (heartbeat) */
    binaryExpose('heartbeat_enable', 'Heartbeat watchdog', ACCESS_ALL, true, false,
        'Enable software watchdog. When enabled, the device expects periodic heartbeat writes ' +
        'from the coordinator. If none arrive within 2× the interval, fallback mode activates. ' +
        'Use with the provided HA automation blueprint.'),

    numericExpose('heartbeat_interval', 'Heartbeat interval', ACCESS_ALL,
        'Expected interval between heartbeat writes from the coordinator. ' +
        'Watchdog timeout = interval × 2.',
        {unit: 's', value_min: 30, value_max: 3600, value_step: 1}),

    enumExpose('heartbeat', 'Heartbeat ping', ACCESS_SET, ['ping'],
        'Write "ping" to reset the software watchdog timer. Called by the HA automation blueprint.'),

    /* Soft/hard two-tier fallback control */
    binaryExpose('fallback_enable', 'Fallback enable', ACCESS_ALL, true, false,
        'Enable the soft/hard two-tier fallback system. When off, the device relies entirely on Home Assistant ' +
        'for light control. When on, the device monitors coordinator responsiveness: if no response arrives ' +
        'within the soft fallback timeout, it temporarily controls bound lights directly. If the coordinator ' +
        'remains unresponsive beyond the hard fallback timeout, the device enters persistent autonomous mode ' +
        'until Home Assistant explicitly clears it.'),

    numericExpose('hard_timeout_sec', 'Hard fallback timeout', ACCESS_ALL,
        'Time in seconds after soft fallback activates before the device escalates to hard (sticky) fallback. ' +
        'In hard fallback, the device permanently controls bound lights based on occupancy until Home Assistant ' +
        'explicitly clears the fallback state. Hard fallback persists across reboots. ' +
        'Default: 10s. Set higher to tolerate brief coordinator outages without escalating.',
        {unit: 's', value_min: 5, value_max: 120, value_step: 1}),

    numericExpose('ack_timeout_ms', 'Soft fallback timeout', ACCESS_ALL,
        'Time in milliseconds the device waits for a coordinator response after an occupancy change. ' +
        'If no response arrives within this window, the device enters soft fallback: it sends On/Off ' +
        'commands directly to bound lights based on occupancy, bypassing the coordinator. Soft fallback ' +
        'is temporary — the first successful coordinator response clears it globally. ' +
        'Default: 2000ms. Increase if your Zigbee network has high latency.',
        {unit: 'ms', value_min: 500, value_max: 10000, value_step: 100}),

    /* Crash diagnostics (read-only) */
    numericExpose('boot_count', 'Boot count', ACCESS_STATE,
        'Total number of device reboots (monotonic counter)', {}),

    numericExpose('reset_reason', 'Reset reason', ACCESS_STATE,
        'Last reset cause code (0=unknown, 1=poweron, 8=brownout, 3=software, 4=panic, 5=int_wdt, 6=task_wdt)',
        {value_min: 0, value_max: 15}),

    numericExpose('last_uptime_sec', 'Last uptime', ACCESS_STATE,
        'Uptime in seconds before last reset. Non-zero only after a clean software reboot (e.g. via Restart button). Hardware resets and power loss always show 0.',
        {unit: 's'}),

    numericExpose('min_free_heap', 'Min free heap', ACCESS_STATE,
        'Minimum free heap memory since boot', {unit: 'bytes'}),

    enumExpose('restart', 'Restart', ACCESS_SET, ['restart'],
        'Restart the device'),

    {type: 'text', name: 'factory_reset_confirm', label: 'Factory reset confirm',
        property: 'factory_reset_confirm', access: ACCESS_SET,
        description: 'Type "factory-reset" exactly and press Set to perform a full factory reset. Erases all settings and Zigbee network data.'},
];

// ---- Shared configure helpers ----

async function configureBindingsAndReads(device, coordinatorEndpoint) {
    const ep1 = device.getEndpoint(1);

    await ep1.bind('msOccupancySensing', coordinatorEndpoint);
    await ep1.configureReporting('msOccupancySensing', [
        {attribute: 'occupancy', minimumReportInterval: 0,
         maximumReportInterval: 300, reportableChange: 0},
    ]);
    await ep1.bind(CLUSTER_CONFIG_ID, coordinatorEndpoint);

    /* Read EP1 config attrs in two batches — 20 attrs in one frame exceeds ZCL frame limits.
     * Batch 1: live data + writable config (~69-byte ZCL payload, safe)
     * Batch 2: fallback timing + diagnostics (~51-byte ZCL payload, safe) */
    try {
        await ep1.read(CLUSTER_CONFIG_ID, [
            ATTR.targetCount, ATTR.targetCoords, ATTR.maxDistance, ATTR.angleLeft, ATTR.angleRight,
            ATTR.trackingMode, ATTR.coordPublishing, ATTR.occupancyCooldown, ATTR.occupancyDelay,
            ATTR.fallbackMode, ATTR.fallbackCooldown, ATTR.fallbackEnable,
        ]);
    } catch (e) {
        console.error(`[ZB_LD2450] EP1 batch1 read FAILED: ${e.message}`);
    }
    try {
        await ep1.read(CLUSTER_CONFIG_ID, [
            ATTR.hardTimeoutSec, ATTR.ackTimeoutMs, ATTR.heartbeatEnable, ATTR.heartbeatInterval,
            ATTR.bootCount, ATTR.resetReason, ATTR.lastUptimeSec, ATTR.minFreeHeap,
        ]);
    } catch (e) {
        console.error(`[ZB_LD2450] EP1 batch2 read FAILED: ${e.message}`);
    }

    /* EPs 2-11: occupancy + per-zone config cluster */
    for (let n = 0; n < 10; n++) {
        const zoneEp = device.getEndpoint(n + 2);
        const base = 0x0040 + n * 4;
        try {
            await zoneEp.bind('msOccupancySensing', coordinatorEndpoint);
            await zoneEp.configureReporting('msOccupancySensing', [
                {attribute: 'occupancy', minimumReportInterval: 0,
                 maximumReportInterval: 300, reportableChange: 0},
            ]);
            await zoneEp.bind(CLUSTER_CONFIG_ID, coordinatorEndpoint);
            await zoneEp.read(CLUSTER_CONFIG_ID, [base, base + 1, base + 2, base + 3]);
        } catch (e) {
            console.error(`[ZB_LD2450] Zone EP${n + 2} configure FAILED: ${e.message}`);
        }
    }
}

// ---- Device definition ----

const sharedBase = {
    vendor: 'LD2450Z',
    fromZigbee: [fzLocal.occupancy, fzLocal.config],
    toZigbee: [tzLocal.config, tzLocal.restart, tzLocal.factory_reset],
    exposes: exposesDefinition,
    ota: true,
    meta: {
        overrideHaDiscoveryPayload: (payload) => {
            if (payload.object_id && payload.object_id.endsWith('_occupancy')) {
                payload.device_class = 'occupancy';
            }
        },
    },
    onEvent: async (type, data, device) => {
        if (type === 'start' || type === 'deviceInterview') {
            registerCustomClusters(device);
        }
    },
};

/* H2: reporting only for live sensor data + fallback mode.
 * Writable config attrs do not yet have ACCESS_REPORTING in firmware —
 * full pipeline 2 support pending H2 firmware update. */
const definition = {
    ...sharedBase,
    zigbeeModel: ['LD2450-H2'],
    model: 'LD2450-ZB-H2',
    description: 'HLK-LD2450 mmWave presence sensor (Zigbee, ESP32-H2)',
    configure: async (device, coordinatorEndpoint) => {
        registerCustomClusters(device);
        const ep1 = device.getEndpoint(1);
        await configureBindingsAndReads(device, coordinatorEndpoint);
        await ep1.configureReporting(CLUSTER_CONFIG_ID, [
            {attribute: {ID: ATTR.targetCount,  type: ZCL_UINT8},    minimumReportInterval: 0, maximumReportInterval: 300,  reportableChange: 1},
            {attribute: {ID: ATTR.targetCoords, type: ZCL_CHAR_STR}, minimumReportInterval: 0, maximumReportInterval: 300},
            {attribute: {ID: ATTR.fallbackMode, type: ZCL_UINT8},    minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
        ]);
    },
};

/* C6: full reporting on all writable config + zone attrs.
 * Requires firmware with ACCESS_REPORTING on writable attrs (v2.2.0+).
 * Enables pipeline 2: device-side changes (CLI/web) propagate automatically to Z2M/HA. */
const definitionC6 = {
    ...sharedBase,
    zigbeeModel: ['LD2450-C6'],
    model: 'LD2450-ZB-C6',
    description: 'HLK-LD2450 mmWave presence sensor (Zigbee+WiFi, ESP32-C6)',
    configure: async (device, coordinatorEndpoint) => {
        registerCustomClusters(device);
        const ep1 = device.getEndpoint(1);
        await configureBindingsAndReads(device, coordinatorEndpoint);
        /* Split into small batches to stay within ZCL frame size limits */
        await ep1.configureReporting(CLUSTER_CONFIG_ID, [
            {attribute: {ID: ATTR.targetCount,       type: ZCL_UINT8},    minimumReportInterval: 0, maximumReportInterval: 300,  reportableChange: 1},
            {attribute: {ID: ATTR.targetCoords,      type: ZCL_CHAR_STR}, minimumReportInterval: 0, maximumReportInterval: 300},
            {attribute: {ID: ATTR.fallbackMode,      type: ZCL_UINT8},    minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
            {attribute: {ID: ATTR.maxDistance,       type: ZCL_UINT16},   minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
            {attribute: {ID: ATTR.angleLeft,         type: ZCL_UINT8},    minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
        ]);
        await ep1.configureReporting(CLUSTER_CONFIG_ID, [
            {attribute: {ID: ATTR.angleRight,        type: ZCL_UINT8},    minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
            {attribute: {ID: ATTR.trackingMode,      type: ZCL_UINT8},    minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
            {attribute: {ID: ATTR.coordPublishing,   type: ZCL_UINT8},    minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
            {attribute: {ID: ATTR.occupancyCooldown, type: ZCL_UINT16},   minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
            {attribute: {ID: ATTR.occupancyDelay,    type: ZCL_UINT16},   minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
        ]);
        await ep1.configureReporting(CLUSTER_CONFIG_ID, [
            {attribute: {ID: ATTR.fallbackEnable,    type: ZCL_UINT8},    minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
            {attribute: {ID: ATTR.fallbackCooldown,  type: ZCL_UINT16},   minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
            {attribute: {ID: ATTR.hardTimeoutSec,    type: ZCL_UINT8},    minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
            {attribute: {ID: ATTR.ackTimeoutMs,      type: ZCL_UINT16},   minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
            {attribute: {ID: ATTR.heartbeatEnable,   type: ZCL_UINT8},    minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
            {attribute: {ID: ATTR.heartbeatInterval, type: ZCL_UINT16},   minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
        ]);
        /* Zone config attrs — each zone on its own EP, one call per EP */
        for (let n = 0; n < 10; n++) {
            const zoneEp = device.getEndpoint(n + 2);
            const base = 0x0040 + n * 4;
            await zoneEp.configureReporting(CLUSTER_CONFIG_ID, [
                {attribute: {ID: base + 0, type: ZCL_UINT8},    minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
                {attribute: {ID: base + 1, type: ZCL_CHAR_STR}, minimumReportInterval: 0, maximumReportInterval: 3600},
                {attribute: {ID: base + 2, type: ZCL_UINT16},   minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
                {attribute: {ID: base + 3, type: ZCL_UINT16},   minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
            ]);
        }
    },
};

module.exports = [definition, definitionC6];
