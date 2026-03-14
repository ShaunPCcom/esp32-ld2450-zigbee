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

// ---- Custom cluster definition ----
const ld2450ConfigCluster = {
    ID: CLUSTER_CONFIG_ID,
    attributes: {
        targetCount:       {ID: 0x0000, type: ZCL_UINT8,    report: true},
        targetCoords:      {ID: 0x0001, type: ZCL_CHAR_STR, report: true},
        maxDistance:       {ID: 0x0010, type: ZCL_UINT16,   write: true},
        angleLeft:         {ID: 0x0011, type: ZCL_UINT8,    write: true},
        angleRight:        {ID: 0x0012, type: ZCL_UINT8,    write: true},
        trackingMode:      {ID: 0x0020, type: ZCL_UINT8,    write: true},
        coordPublishing:   {ID: 0x0021, type: ZCL_UINT8,    write: true},
        occupancyCooldown: {ID: 0x0022, type: ZCL_UINT16,   write: true},
        occupancyDelay:    {ID: 0x0023, type: ZCL_UINT16,   write: true},
        bootCount:         {ID: 0x0030, type: ZCL_UINT32,   report: false},
        resetReason:       {ID: 0x0031, type: ZCL_UINT8,    report: false},
        lastUptimeSec:     {ID: 0x0032, type: ZCL_UINT32,   report: false},
        minFreeHeap:       {ID: 0x0033, type: ZCL_UINT32,   report: false},
        restart:           {ID: 0x00F0, type: ZCL_UINT8,    write: true},
        factoryReset:      {ID: 0x00F1, type: ZCL_UINT8,    write: true},
        ...zoneConfigAttrs,
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
        cluster: 'ld2450Config',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            const result = {};
            const d = msg.data;

            if (d.targetCount !== undefined)     result.target_count       = d.targetCount;
            if (d.maxDistance !== undefined)     result.max_distance       = d.maxDistance / 1000;
            if (d.angleLeft !== undefined)       result.angle_left         = d.angleLeft;
            if (d.angleRight !== undefined)      result.angle_right        = d.angleRight;
            if (d.trackingMode !== undefined)    result.tracking_mode      = d.trackingMode === 0;
            if (d.coordPublishing !== undefined) result.coord_publishing   = d.coordPublishing === 1;
            if (d.occupancyCooldown !== undefined) result.occupancy_cooldown = d.occupancyCooldown;
            if (d.occupancyDelay !== undefined)  result.occupancy_delay    = d.occupancyDelay;

            if (d.bootCount !== undefined)       result.boot_count         = d.bootCount;
            if (d.resetReason !== undefined)     result.reset_reason       = d.resetReason;
            if (d.lastUptimeSec !== undefined)   result.last_uptime_sec    = d.lastUptimeSec;
            if (d.minFreeHeap !== undefined)     result.min_free_heap      = d.minFreeHeap;

            if (d.targetCoords !== undefined) {
                const str = d.targetCoords || '';
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

            /* Zone config attrs (n=0..9 firmware, z=1..10 Z2M) */
            for (let n = 0; n < 10; n++) {
                const z = n + 1;
                const vc = d[`zone${z}VertexCount`];
                const cs = d[`zone${z}Coords`];
                const cl = d[`zone${z}Cooldown`];
                const dl = d[`zone${z}Delay`];

                if (vc !== undefined) result[`zone_${z}_vertex_count`] = String(vc);
                if (cs !== undefined) result[`zone_${z}_coords`]       = mmCsvToMetres(cs || '');
                if (cl !== undefined) result[`zone_${z}_cooldown`]     = cl;
                if (dl !== undefined) result[`zone_${z}_delay`]        = dl;
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
                const validVc = Math.max(0, parseInt(value) || 0);
                const coordsKey = `zone_${z}_coords`;
                const stateUpdate = {[key]: String(validVc)};

                await ep1.write('ld2450Config', {[`zone${z}VertexCount`]: validVc});

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
                        await ep1.write('ld2450Config', {[`zone${z}Coords`]: metresCsvToMm(metresCoords)});
                        stateUpdate[coordsKey] = metresCoords;
                    } catch (e) {
                        meta.logger.warn(`[ZB_LD2450] Coords resize failed: ${e.message}`);
                    }
                }

                return {state: stateUpdate};
            }

            /* Zone coords (metres CSV → mm CSV for firmware) */
            const zoneCoordsMatch = key.match(/^zone_(\d+)_coords$/);
            if (zoneCoordsMatch) {
                const n = parseInt(zoneCoordsMatch[1]) - 1;
                const mmCsv = metresCsvToMm(value);
                await ep1.write('ld2450Config', {[`zone${n + 1}Coords`]: mmCsv});
                return {state: {[key]: value}};
            }

            /* Zone cooldown */
            const zoneCoolMatch = key.match(/^zone_(\d+)_cooldown$/);
            if (zoneCoolMatch) {
                const n = parseInt(zoneCoolMatch[1]) - 1;
                await ep1.write('ld2450Config', {[`zone${n + 1}Cooldown`]: value});
                return {state: {[key]: value}};
            }

            /* Zone delay */
            const zoneDelayMatch = key.match(/^zone_(\d+)_delay$/);
            if (zoneDelayMatch) {
                const n = parseInt(zoneDelayMatch[1]) - 1;
                await ep1.write('ld2450Config', {[`zone${n + 1}Delay`]: value});
                return {state: {[key]: value}};
            }

            /* Main endpoint config */
            const map = {
                max_distance:       {attr: 'maxDistance',       val: (v) => Math.round(v * 1000)},
                angle_left:         {attr: 'angleLeft',         val: (v) => v},
                angle_right:        {attr: 'angleRight',        val: (v) => v},
                tracking_mode:      {attr: 'trackingMode',      val: (v) => v ? 0 : 1},
                coord_publishing:   {attr: 'coordPublishing',   val: (v) => v ? 1 : 0},
                occupancy_cooldown: {attr: 'occupancyCooldown', val: (v) => v},
                occupancy_delay:    {attr: 'occupancyDelay',    val: (v) => v},
            };
            const m = map[key];
            if (m) {
                await ep1.write('ld2450Config', {[m.attr]: m.val(value)});
                return {state: {[key]: value}};
            }
        },

        convertGet: async (entity, key, meta) => {
            registerCustomClusters(meta.device);
            const ep1 = meta.device.getEndpoint(1);

            /* Zone attrs */
            const zoneMatch = key.match(/^zone_(\d+)_(vertex_count|coords|cooldown|delay)$/);
            if (zoneMatch) {
                const n = parseInt(zoneMatch[1]) - 1;
                const subMap = {
                    vertex_count: `zone${n + 1}VertexCount`,
                    coords:       `zone${n + 1}Coords`,
                    cooldown:     `zone${n + 1}Cooldown`,
                    delay:        `zone${n + 1}Delay`,
                };
                await ep1.read('ld2450Config', [subMap[zoneMatch[2]]]);
                return;
            }

            /* Main endpoint config */
            const attrs = {
                max_distance: 'maxDistance', angle_left: 'angleLeft',
                angle_right: 'angleRight', tracking_mode: 'trackingMode',
                coord_publishing: 'coordPublishing', occupancy_cooldown: 'occupancyCooldown',
                occupancy_delay: 'occupancyDelay',
            };
            if (attrs[key]) await ep1.read('ld2450Config', [attrs[key]]);
        },
    },

    restart: {
        key: ['restart'],
        convertSet: async (entity, key, value, meta) => {
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);
            await ep.write('ld2450Config', {restart: 1});
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
            await ep.write('ld2450Config', {factoryReset: 0xFE}, {disableDefaultResponse: true});
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

// ---- Device definition ----

const definition = {
    zigbeeModel: ['LD2450-H2'],
    model: 'LD2450-ZB-H2',
    vendor: 'LD2450Z',
    description: 'HLK-LD2450 mmWave presence sensor (Zigbee, ESP32-H2)',
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
    configure: async (device, coordinatorEndpoint) => {
        registerCustomClusters(device);

        const ep1 = device.getEndpoint(1);

        /* EP1: occupancy + config reporting */
        await ep1.bind('msOccupancySensing', coordinatorEndpoint);
        await ep1.configureReporting('msOccupancySensing', [
            {attribute: 'occupancy', minimumReportInterval: 0,
             maximumReportInterval: 300, reportableChange: 0},
        ]);
        await ep1.bind('ld2450Config', coordinatorEndpoint);
        await ep1.configureReporting('ld2450Config', [
            {attribute: 'targetCount', minimumReportInterval: 0,
             maximumReportInterval: 300, reportableChange: 1},
            {attribute: 'targetCoords', minimumReportInterval: 0,
             maximumReportInterval: 300},
        ]);

        /* Read all EP1 config + zone config attrs */
        await ep1.read('ld2450Config', [
            'targetCount', 'targetCoords', 'maxDistance', 'angleLeft', 'angleRight',
            'trackingMode', 'coordPublishing', 'occupancyCooldown', 'occupancyDelay',
            'bootCount', 'resetReason', 'lastUptimeSec', 'minFreeHeap',
        ]);
        /* Read zone config attrs one zone at a time — 40 attrs in one frame exceeds ZCL frame limits */
        for (let n = 0; n < 10; n++) {
            await ep1.read('ld2450Config', [
                `zone${n + 1}VertexCount`,
                `zone${n + 1}Coords`,
                `zone${n + 1}Cooldown`,
                `zone${n + 1}Delay`,
            ]);
        }

        /* EPs 2-11: occupancy sensing only (zone config lives on EP1) */
        for (let z = 0; z < 10; z++) {
            const ep = device.getEndpoint(z + 2);
            await ep.bind('msOccupancySensing', coordinatorEndpoint);
            await ep.configureReporting('msOccupancySensing', [
                {attribute: 'occupancy', minimumReportInterval: 0,
                 maximumReportInterval: 300, reportableChange: 0},
            ]);
        }
    },
};

module.exports = [definition];
