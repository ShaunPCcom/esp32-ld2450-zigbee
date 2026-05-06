'use strict';

/* ─────────────────────────────────────────────────────────────
   State
───────────────────────────────────────────────────────────── */
let cfg = {};
let ws  = null;
let activeZone = 0;
let editMode   = false;
let live = { t: [], occ: false, z: Array(10).fill(false) };
let drag = null;   // { zi, vi } while dragging a vertex
let cvW = 0, cvH = 0;

/* ─────────────────────────────────────────────────────────────
   Boot
───────────────────────────────────────────────────────────── */
window.addEventListener('DOMContentLoaded', async () => {
  initCanvas();
  initTabs();
  initSliders();
  initToggles();
  initSelects();
  initOtaInterval();
  await loadConfig();
  await loadStatus();
  await loadOtaStatus();
  await loadOtaInterval();
  await loadOtaIndexUrl();
  connectWS();
  buildZoneGrid();
  renderZoneDetail();
  requestAnimationFrame(frame);
  document.getElementById('hdr-host').textContent = location.hostname;

  setInterval(pollStatus,   10000);
});

/* ─────────────────────────────────────────────────────────────
   Tabs
───────────────────────────────────────────────────────────── */
function initTabs() {
  document.querySelectorAll('.tab').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.tab').forEach(b => b.classList.remove('on'));
      document.querySelectorAll('.pane').forEach(p => p.classList.remove('on'));
      btn.classList.add('on');
      document.getElementById('pane-' + btn.dataset.tab).classList.add('on');
      editMode = btn.dataset.tab === 'zones';
      document.getElementById('edit-banner').classList.toggle('show', editMode);
    });
  });
}

/* ─────────────────────────────────────────────────────────────
   Config API
───────────────────────────────────────────────────────────── */
async function loadConfig() {
  try {
    const r = await fetch('/api/config');
    cfg = await r.json();
    applyToUI();
  } catch (e) {
    toast('CONFIG LOAD FAILED', 'err');
  }
}

async function loadStatus() {
  try {
    const r = await fetch('/api/status');
    const s = await r.json();
    document.getElementById('sy-fw').textContent    = s.firmware   || '—';
    document.getElementById('sy-up').textContent    = fmtUptime(s.uptime_sec);
    document.getElementById('sy-heap').textContent  = fmtBytes(s.free_heap);
    document.getElementById('wf-state').textContent = s.wifi || '—';
    document.getElementById('wf-host').textContent  = location.hostname;
  } catch (e) {}
}

async function pollStatus() {
  try {
    const r = await fetch('/api/status');
    const s = await r.json();
    document.getElementById('sy-up').textContent    = fmtUptime(s.uptime_sec);
    document.getElementById('sy-heap').textContent  = fmtBytes(s.free_heap);
    document.getElementById('wf-state').textContent = s.wifi || '—';
  } catch (e) {}
}

async function saveConfig(patch) {
  try {
    const r = await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(patch)
    });
    const d = await r.json();
    if (d.status === 'ok') toast('SAVED', 'ok');
    else                   toast('ERROR', 'err');
  } catch (e) {
    toast('SAVE FAILED', 'err');
  }
}

async function saveAllZones() {
  const patch = {
    zones: cfg.zones.map(z => ({
      vertex_count: z.vertex_count,
      coords:       z.coords || '',
      cooldown_sec: z.cooldown_sec,
      delay_ms:     z.delay_ms,
      fallback_cooldown_sec: z.fallback_cooldown_sec
    }))
  };
  await saveConfig(patch);
}

/* ─────────────────────────────────────────────────────────────
   UI ↔ Config binding
───────────────────────────────────────────────────────────── */
function applyToUI() {
  const focused = document.activeElement;
  /* Sliders */
  document.querySelectorAll('input[type=range][data-key]').forEach(sl => {
    if (sl === focused) return;
    const v = cfg[sl.dataset.key];
    if (v !== undefined) { sl.value = v; updateVal(sl); }
  });
  /* Toggles */
  document.querySelectorAll('input[type=checkbox][data-key]').forEach(cb => {
    if (cb === focused) return;
    const v = cfg[cb.dataset.key];
    if (v !== undefined)
      cb.checked = cb.hasAttribute('data-invert') ? (v === 0) : (v !== 0);
  });
  /* Selects */
  document.querySelectorAll('select[data-key]').forEach(sel => {
    if (sel === focused) return;
    const v = cfg[sel.dataset.key];
    if (v !== undefined) sel.value = String(v);
  });
  /* Overlay */
  refreshOverlay();
  buildZoneGrid();
}

function updateVal(sl) {
  const el = document.getElementById('v-' + sl.dataset.key);
  if (el) el.textContent = sl.value + (sl.dataset.unit || '');
}

function initSliders() {
  document.querySelectorAll('input[type=range][data-key]').forEach(sl => {
    sl.addEventListener('input', () => updateVal(sl));
    sl.addEventListener('change', () => {
      const key = sl.dataset.key;
      cfg[key] = Number(sl.value);
      saveConfig({ [key]: cfg[key] });
      if (key === 'max_distance_mm') refreshOverlay();
    });
  });
}

function initToggles() {
  document.querySelectorAll('input[type=checkbox][data-key]').forEach(cb => {
    cb.addEventListener('change', () => {
      const key = cb.dataset.key;
      let v = cb.checked ? 1 : 0;
      if (cb.hasAttribute('data-invert')) v = cb.checked ? 0 : 1;
      cfg[key] = v;
      saveConfig({ [key]: v });
    });
  });
}

function initSelects() {
  document.querySelectorAll('select[data-key]').forEach(sel => {
    sel.addEventListener('change', () => {
      const key = sel.dataset.key;
      cfg[key] = Number(sel.value);
      saveConfig({ [key]: cfg[key] });
    });
  });
}

function refreshOverlay() {
  const dist = cfg.max_distance_mm || 5000;
  const zones = cfg.zones || [];
  const active = zones.filter(z => z.vertex_count >= 3).length;
  document.getElementById('ov-range').textContent = (dist / 1000).toFixed(1) + ' m';
  document.getElementById('ov-zones').textContent = active + '/10';
}

/* ─────────────────────────────────────────────────────────────
   Zone Grid
───────────────────────────────────────────────────────────── */
function buildZoneGrid() {
  const grid = document.getElementById('zone-grid');
  grid.innerHTML = '';
  (cfg.zones || []).forEach((z, i) => {
    const enabled = z.vertex_count >= 3;
    const btn = document.createElement('button');
    btn.className = ['zbtn', enabled ? 'enabled' : '', i === activeZone ? 'active' : ''].join(' ');
    btn.textContent = 'Z' + (i + 1);
    btn.addEventListener('click', () => {
      activeZone = i;
      grid.querySelectorAll('.zbtn').forEach((b, j) => {
        b.className = ['zbtn', (cfg.zones[j] && cfg.zones[j].vertex_count >= 3) ? 'enabled' : '', j === i ? 'active' : ''].join(' ');
      });
      renderZoneDetail();
    });
    grid.appendChild(btn);
  });
}

/* ─────────────────────────────────────────────────────────────
   Zone Detail
───────────────────────────────────────────────────────────── */
function renderZoneDetail() {
  const el  = document.getElementById('zone-detail');
  const z   = cfg.zones && cfg.zones[activeZone];
  if (!z) { el.innerHTML = ''; return; }
  const enabled = z.vertex_count >= 3;

  el.innerHTML = `
    <div class="sec">Zone ${activeZone + 1}</div>
    <div class="tog-row">
      <span class="tog-lbl">Zone Enabled</span>
      <label class="tog">
        <input type="checkbox" id="z-en" ${enabled ? 'checked' : ''}>
        <div class="tog-track"></div><div class="tog-thumb"></div>
      </label>
    </div>
    ${enabled ? `
    <div class="field">
      <div class="flabel">Vertices
        <span class="fval" id="z-vc-v">${z.vertex_count}</span>
      </div>
      <input type="range" id="z-vc" min="3" max="10" step="1" value="${z.vertex_count}">
    </div>
    <div class="field">
      <div class="flabel">Clear Cooldown
        <span class="fval" id="z-cd-v">${z.cooldown_sec}s</span>
      </div>
      <input type="range" id="z-cd" min="0" max="30" step="1" value="${z.cooldown_sec}">
    </div>
    <div class="field">
      <div class="flabel">Entry Delay
        <span class="fval" id="z-dl-v">${z.delay_ms}ms</span>
      </div>
      <input type="range" id="z-dl" min="0" max="2000" step="50" value="${z.delay_ms}">
    </div>
    <div class="field">
      <div class="flabel">Fallback Cooldown
        <span class="fval" id="z-fc-v">${z.fallback_cooldown_sec}s</span>
      </div>
      <input type="range" id="z-fc" min="0" max="120" step="5" value="${z.fallback_cooldown_sec}">
    </div>
    <div class="sec">Coordinates</div>
    <div class="coord-grid" id="z-coord-grid">
      ${parseCoords(z.coords).map((p, vi) => `
        <div class="coord-row">
          <span class="coord-lbl">V${vi + 1}</span>
          <label class="coord-field">
            <span class="coord-axis">X</span>
            <input type="number" class="coord-inp" data-vi="${vi}" data-axis="x"
              value="${(p.x / 1000).toFixed(3)}" step="0.001">
            <span class="coord-unit">m</span>
          </label>
          <label class="coord-field">
            <span class="coord-axis">Y</span>
            <input type="number" class="coord-inp" data-vi="${vi}" data-axis="y"
              value="${(p.y / 1000).toFixed(3)}" step="0.001" min="0">
            <span class="coord-unit">m</span>
          </label>
        </div>`).join('')}
    </div>
    <div class="hint">Fine-tune vertex positions after drag-and-drop. Use the radar grid for alignment — changes apply immediately.</div>
    ` : `
    <div class="hint">Enable to activate this zone.<br>A default triangle will appear — drag its vertices on the radar to position it.</div>
    `}
  `;

  /* Enable toggle */
  document.getElementById('z-en').addEventListener('change', async e => {
    const z2 = cfg.zones[activeZone];
    if (e.target.checked) {
      z2.vertex_count = 3;
      if (!z2.coords || countPairs(z2.coords) < 3) {
        const d = cfg.max_distance_mm || 3000;
        const h = Math.round(d * 0.25), t = Math.round(d * 0.55);
        const w = Math.round(d * 0.15);
        z2.coords = `${-w},${h},${w},${h},0,${t}`;
      }
    } else {
      z2.vertex_count = 0;
    }
    await saveAllZones();
    buildZoneGrid();
    refreshOverlay();
    renderZoneDetail();
  });

  if (!enabled) return;

  /* Vertex count */
  const vcSl = document.getElementById('z-vc');
  vcSl.addEventListener('input', () => {
    document.getElementById('z-vc-v').textContent = vcSl.value;
  });
  vcSl.addEventListener('change', async () => {
    const nv = Number(vcSl.value);
    const oz = cfg.zones[activeZone];
    const pts = parseCoords(oz.coords);
    if (nv < pts.length) {
      pts.splice(nv);
    } else {
      while (pts.length < nv) {
        const last = pts[pts.length - 1] || { x: 0, y: 500 };
        pts.push({ x: last.x + 150, y: last.y });
      }
    }
    oz.vertex_count = nv;
    oz.coords = toCoords(pts);
    await saveAllZones();
    renderZoneDetail();
  });

  /* Coordinate inputs */
  document.querySelectorAll('.coord-inp').forEach(inp => {
    inp.addEventListener('change', async () => {
      const vi   = Number(inp.dataset.vi);
      const axis = inp.dataset.axis;
      const mm   = Math.round(parseFloat(inp.value) * 1000);
      if (isNaN(mm)) return;
      const oz  = cfg.zones[activeZone];
      const pts = parseCoords(oz.coords);
      if (!pts[vi]) return;
      pts[vi][axis] = axis === 'y' ? Math.max(0, mm) : mm;
      oz.coords = toCoords(pts);
      await saveAllZones();
    });
  });

  /* Zone timing sliders */
  bindZoneSlider('z-cd', 'z-cd-v', 's',  v => { cfg.zones[activeZone].cooldown_sec = v; });
  bindZoneSlider('z-dl', 'z-dl-v', 'ms', v => { cfg.zones[activeZone].delay_ms = v; });
  bindZoneSlider('z-fc', 'z-fc-v', 's',  v => { cfg.zones[activeZone].fallback_cooldown_sec = v; });
}

function bindZoneSlider(id, valId, unit, setter) {
  const sl = document.getElementById(id);
  if (!sl) return;
  sl.addEventListener('input', () => {
    document.getElementById(valId).textContent = sl.value + unit;
  });
  sl.addEventListener('change', async () => {
    setter(Number(sl.value));
    await saveAllZones();
  });
}

/* ─────────────────────────────────────────────────────────────
   Coordinate helpers
   Firmware format: flat comma-separated "x1,y1,x2,y2,x3,y3"
   csv_count_pairs counts commas → (commas+1)/2
───────────────────────────────────────────────────────────── */
function parseCoords(csv) {
  if (!csv || csv.trim() === '') return [];
  const vals = csv.split(',').map(Number);
  const pts = [];
  for (let i = 0; i + 1 < vals.length; i += 2) {
    if (!isNaN(vals[i]) && !isNaN(vals[i + 1])) {
      pts.push({ x: vals[i], y: vals[i + 1] });
    }
  }
  return pts;
}

function toCoords(pts) {
  return pts.map(p => `${Math.round(p.x)},${Math.round(p.y)}`).join(',');
}

function countPairs(csv) {
  if (!csv || csv.trim() === '') return 0;
  const n = (csv.match(/,/g) || []).length;
  return Math.floor((n + 1) / 2);
}

/* ─────────────────────────────────────────────────────────────
   Canvas
───────────────────────────────────────────────────────────── */
let cv, ctx;

function initCanvas() {
  cv  = document.getElementById('cv');
  ctx = cv.getContext('2d');
  resize();
  new ResizeObserver(resize).observe(cv.parentElement);

  cv.addEventListener('mousedown',  e => onDown(evPt(e)));
  cv.addEventListener('mousemove',  e => onMove(evPt(e)));
  cv.addEventListener('mouseup',    () => onUp());
  cv.addEventListener('mouseleave', () => onUp());
  cv.addEventListener('touchstart', e => { e.preventDefault(); onDown(evPt(e.touches[0])); }, { passive: false });
  cv.addEventListener('touchmove',  e => { e.preventDefault(); onMove(evPt(e.touches[0])); }, { passive: false });
  cv.addEventListener('touchend',   e => { e.preventDefault(); onUp(); }, { passive: false });
}

function resize() {
  const wrap = cv.parentElement;
  cv.width  = cvW = wrap.clientWidth;
  cv.height = cvH = wrap.clientHeight;
}

/* mm → canvas pixel  (sensor at top, positive y goes down) */
function mm2cv(x, y) {
  const ox = cvW / 2, oy = 28;
  const maxD = cfg.max_distance_mm || 5000;
  const sc   = (cvH - 56) / maxD;
  return [ox + x * sc, oy + y * sc];
}

/* canvas pixel → mm */
function cv2mm(cx, cy) {
  const ox = cvW / 2, oy = 28;
  const maxD = cfg.max_distance_mm || 5000;
  const sc   = (cvH - 56) / maxD;
  return [(cx - ox) / sc, (cy - oy) / sc];
}

/* ─────────────────────────────────────────────────────────────
   Render loop
───────────────────────────────────────────────────────────── */
function frame() {
  draw();
  requestAnimationFrame(frame);
}

function draw() {
  if (!ctx || cvW === 0) return;
  ctx.clearRect(0, 0, cvW, cvH);

  // Dark background with subtle noise feel via gradient
  const bg = ctx.createRadialGradient(cvW / 2, 0, 0, cvW / 2, 0, cvH);
  bg.addColorStop(0, '#0a1510');
  bg.addColorStop(1, '#060c09');
  ctx.fillStyle = bg;
  ctx.fillRect(0, 0, cvW, cvH);

  drawGrid();
  drawFOV();
  drawZones();
  drawTargets();
  drawSensorNode();
}

const RINGS = 4;

function drawGrid() {
  const [ox, oy] = mm2cv(0, 0);
  const maxD = cfg.max_distance_mm || 5000;
  const sc   = (cvH - 56) / maxD;

  ctx.font = '10px "Share Tech Mono",monospace';
  ctx.textAlign = 'left';

  for (let i = 1; i <= RINGS; i++) {
    const r = (maxD / RINGS) * i * sc;
    ctx.beginPath();
    ctx.arc(ox, oy, r, 0, Math.PI);
    ctx.strokeStyle = 'rgba(30,80,48,.5)';
    ctx.lineWidth = 1;
    ctx.stroke();

    const dist_m = ((maxD / RINGS) * i / 1000).toFixed(1);
    ctx.fillStyle = 'rgba(62,102,80,.7)';
    ctx.fillText(dist_m + 'm', ox + 4, oy + r - 5);
  }

  // Cartesian meter grid — X and Y lines at 1m intervals
  const step = maxD <= 2000 ? 500 : 1000;
  const [, bot] = mm2cv(0, maxD);
  ctx.lineWidth = 1;
  ctx.setLineDash([2, 5]);

  // Horizontal Y lines (depth reference, straight)
  for (let y = step; y < maxD; y += step) {
    const [, cy] = mm2cv(0, y);
    ctx.beginPath();
    ctx.moveTo(0, cy);
    ctx.lineTo(cvW, cy);
    ctx.strokeStyle = 'rgba(30,80,48,.55)';
    ctx.stroke();
  }

  // Vertical X lines (left/right reference) + labels
  const maxX = Math.ceil((cvW / 2) / (step * (cvH - 56) / maxD)) * step;
  ctx.font = '9px "Share Tech Mono",monospace';
  ctx.textAlign = 'center';
  for (let x = -maxX; x <= maxX; x += step) {
    if (x === 0) continue;
    const [cx] = mm2cv(x, 0);
    if (cx < 0 || cx > cvW) continue;
    ctx.beginPath();
    ctx.moveTo(cx, oy);
    ctx.lineTo(cx, bot);
    ctx.strokeStyle = 'rgba(30,80,48,.55)';
    ctx.stroke();
    const label = (x > 0 ? '+' : '') + (x / 1000).toFixed(step < 1000 ? 1 : 0) + 'm';
    ctx.fillStyle = 'rgba(62,102,80,.95)';
    ctx.fillText(label, cx, oy + 12);
  }
  ctx.setLineDash([]);

  // Center axis
  const [, ty] = mm2cv(0, maxD);
  ctx.beginPath();
  ctx.moveTo(ox, oy);
  ctx.lineTo(ox, ty);
  ctx.strokeStyle = 'rgba(26,51,34,.6)';
  ctx.lineWidth = 1;
  ctx.stroke();
}

function drawFOV() {
  if (!cfg.max_distance_mm) return;
  const [ox, oy] = mm2cv(0, 0);
  const maxD = cfg.max_distance_mm;
  const sc   = (cvH - 56) / maxD;
  const r    = maxD * sc;
  const la   = ((cfg.angle_left_deg  || 0) * Math.PI) / 180;
  const ra   = ((cfg.angle_right_deg || 0) * Math.PI) / 180;
  const aL   = Math.PI / 2 + la;   /* left FOV edge  (down-left)  */
  const aR   = Math.PI / 2 - ra;   /* right FOV edge (down-right) */

  // Fill (clockwise sweep from right edge to left edge through π/2)
  ctx.beginPath();
  ctx.moveTo(ox, oy);
  ctx.arc(ox, oy, r, aR, aL);
  ctx.closePath();
  ctx.fillStyle = 'rgba(0,232,122,.04)';
  ctx.fill();

  // Left edge
  ctx.setLineDash([5, 5]);
  ctx.strokeStyle = 'rgba(0,232,122,.22)';
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(ox, oy);
  ctx.lineTo(ox + Math.cos(aL) * r, oy + Math.sin(aL) * r);
  ctx.stroke();

  // Right edge
  ctx.beginPath();
  ctx.moveTo(ox, oy);
  ctx.lineTo(ox + Math.cos(aR) * r, oy + Math.sin(aR) * r);
  ctx.stroke();
  ctx.setLineDash([]);

  // Arc
  ctx.beginPath();
  ctx.arc(ox, oy, r, aR, aL);
  ctx.strokeStyle = 'rgba(0,232,122,.28)';
  ctx.lineWidth = 1;
  ctx.stroke();
}

function drawZones() {
  if (!cfg.zones) return;
  cfg.zones.forEach((z, i) => {
    if (z.vertex_count < 3 || !z.coords) return;
    const pts = parseCoords(z.coords);
    if (pts.length < 3) return;
    const cvPts = pts.map(p => mm2cv(p.x, p.y));
    const occ   = live.z[i];
    const sel   = editMode && i === activeZone;

    // Fill
    ctx.beginPath();
    ctx.moveTo(cvPts[0][0], cvPts[0][1]);
    cvPts.slice(1).forEach(p => ctx.lineTo(p[0], p[1]));
    ctx.closePath();
    ctx.fillStyle = occ  ? 'rgba(0,232,122,.1)'
                 : sel  ? 'rgba(0,153,238,.08)'
                        : 'rgba(0,232,122,.03)';
    ctx.fill();

    // Outline
    ctx.beginPath();
    ctx.moveTo(cvPts[0][0], cvPts[0][1]);
    cvPts.slice(1).forEach(p => ctx.lineTo(p[0], p[1]));
    ctx.closePath();
    if (sel) ctx.setLineDash([3, 3]);
    ctx.strokeStyle = occ  ? 'rgba(0,232,122,.85)'
                   : sel  ? 'rgba(0,153,238,.7)'
                          : 'rgba(0,232,122,.28)';
    ctx.lineWidth = occ ? 1.5 : 1;
    ctx.stroke();
    ctx.setLineDash([]);

    // Label
    const cx = cvPts.reduce((s, p) => s + p[0], 0) / cvPts.length;
    const cy = cvPts.reduce((s, p) => s + p[1], 0) / cvPts.length;
    ctx.font = '11px "Share Tech Mono",monospace';
    ctx.textAlign = 'center';
    ctx.fillStyle = occ ? 'rgba(0,232,122,.9)' : 'rgba(0,232,122,.38)';
    ctx.fillText('Z' + (i + 1), cx, cy + 4);
    ctx.textAlign = 'left';

    // Vertex handles (edit mode, selected zone)
    if (sel) {
      cvPts.forEach((p, vi) => {
        const dragging = drag && drag.zi === i && drag.vi === vi;
        ctx.beginPath();
        ctx.arc(p[0], p[1], dragging ? 9 : 5, 0, Math.PI * 2);
        ctx.fillStyle   = 'rgba(0,153,238,' + (dragging ? '.9' : '.7') + ')';
        ctx.fill();
        ctx.strokeStyle = 'rgba(0,153,238,.9)';
        ctx.lineWidth = 1;
        ctx.stroke();
      });
    }
  });
}

function drawTargets() {
  live.t.forEach(t => {
    if (!t.p) return;
    const [cx, cy] = mm2cv(t.x, t.y);

    // Glow
    const g = ctx.createRadialGradient(cx, cy, 0, cx, cy, 18);
    g.addColorStop(0,   'rgba(0,232,122,.55)');
    g.addColorStop(.4,  'rgba(0,232,122,.18)');
    g.addColorStop(1,   'rgba(0,232,122,0)');
    ctx.beginPath();
    ctx.arc(cx, cy, 18, 0, Math.PI * 2);
    ctx.fillStyle = g;
    ctx.fill();

    // Core
    ctx.beginPath();
    ctx.arc(cx, cy, 4, 0, Math.PI * 2);
    ctx.fillStyle = '#00e87a';
    ctx.fill();

    // Crosshair
    ctx.strokeStyle = 'rgba(0,232,122,.45)';
    ctx.lineWidth = .7;
    ctx.beginPath();
    ctx.moveTo(cx - 12, cy); ctx.lineTo(cx + 12, cy);
    ctx.moveTo(cx, cy - 12); ctx.lineTo(cx, cy + 12);
    ctx.stroke();

    // Coords
    ctx.font = '10px "Share Tech Mono",monospace';
    ctx.fillStyle = 'rgba(0,232,122,.75)';
    ctx.fillText(Math.round(t.x) + ',' + Math.round(t.y), cx + 10, cy - 8);
  });
}

function drawSensorNode() {
  const [ox, oy] = mm2cv(0, 0);
  ctx.beginPath();
  ctx.arc(ox, oy, 14, 0, Math.PI * 2);
  ctx.strokeStyle = 'rgba(0,232,122,.15)';
  ctx.lineWidth = 1;
  ctx.stroke();

  ctx.beginPath();
  ctx.arc(ox, oy, 5, 0, Math.PI * 2);
  ctx.fillStyle = '#00e87a';
  ctx.fill();
}

/* ─────────────────────────────────────────────────────────────
   Vertex drag
───────────────────────────────────────────────────────────── */
const HIT_R = 14;

function evPt(e) {
  const r  = cv.getBoundingClientRect();
  const sx = cv.width  / r.width;
  const sy = cv.height / r.height;
  return [(e.clientX - r.left) * sx, (e.clientY - r.top) * sy];
}

function hitTest(cx, cy) {
  if (!editMode || !cfg.zones) return null;
  const z = cfg.zones[activeZone];
  if (!z || z.vertex_count < 3) return null;
  const pts = parseCoords(z.coords).map(p => mm2cv(p.x, p.y));
  for (let vi = 0; vi < pts.length; vi++) {
    const dx = cx - pts[vi][0], dy = cy - pts[vi][1];
    if (Math.hypot(dx, dy) < HIT_R) return { zi: activeZone, vi };
  }
  return null;
}

function onDown([cx, cy]) {
  const h = hitTest(cx, cy);
  if (h) drag = h;
}

function onMove([cx, cy]) {
  if (!drag) return;
  const z = cfg.zones[drag.zi];
  if (!z) return;
  const pts = parseCoords(z.coords);
  const [mx, my] = cv2mm(cx, cy);
  pts[drag.vi] = { x: Math.round(mx), y: Math.round(Math.max(0, my)) };
  z.coords = toCoords(pts);
}

function onUp() {
  if (!drag) return;
  drag = null;
  saveAllZones();
  if (editMode) renderZoneDetail();
}

/* ─────────────────────────────────────────────────────────────
   WebSocket
───────────────────────────────────────────────────────────── */
let wsLastMsg = 0;

function connectWS() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws/targets`);

  ws.onopen = () => {
    wsLastMsg = Date.now();
    document.getElementById('ws-dot').classList.add('live');
    document.getElementById('ws-label').textContent = 'live';
  };

  ws.onmessage = e => {
    wsLastMsg = Date.now();
    try {
      const d = JSON.parse(e.data);
      live.t   = d.t  || [];
      live.occ = d.occ || false;
      live.z   = d.z  || Array(10).fill(false);

      const badge = document.getElementById('r-badge');
      badge.textContent = live.occ ? 'OCCUPIED' : 'VACANT';
      badge.className   = 'r-badge' + (live.occ ? ' occ' : '');

      const active = live.t.filter(t => t.p).length;
      document.getElementById('ov-tgts').textContent = active;
    } catch (_) {}
  };

  ws.onclose = () => {
    document.getElementById('ws-dot').classList.remove('live');
    document.getElementById('ws-label').textContent = 'offline';
    setTimeout(connectWS, 3000);
  };

  ws.onerror = () => ws.close();
}

/* Watchdog: if the socket appears open but no message for >3 s,
 * the TCP connection has gone half-open. Force-close to trigger
 * the onclose → reconnect path. */
setInterval(() => {
  if (ws && ws.readyState === WebSocket.OPEN && wsLastMsg > 0
      && (Date.now() - wsLastMsg) > 3000) {
    ws.close();
  }
}, 2000);

/* ─────────────────────────────────────────────────────────────
   SSE — live config and OTA sync
───────────────────────────────────────────────────────────── */
let _sseReconnectTimer = null;
let _sseBadgeEl = null;

function _sseShowReconnectBadge(show) {
  if (show && !_sseBadgeEl) {
    _sseBadgeEl = document.createElement('div');
    _sseBadgeEl.id = 'sse-badge';
    _sseBadgeEl.style.cssText =
      'position:fixed;bottom:8px;left:50%;transform:translateX(-50%);' +
      'background:#1a1a1a;color:#f59e0b;border:1px solid #f59e0b44;' +
      'padding:6px 14px;border-radius:6px;font-size:.8em;z-index:999';
    _sseBadgeEl.textContent = 'Live sync reconnecting\u2026';
    document.body.appendChild(_sseBadgeEl);
  } else if (!show && _sseBadgeEl) {
    _sseBadgeEl.remove();
    _sseBadgeEl = null;
  }
}

(function initSSE() {
  const es = new EventSource('/api/events');

  es.addEventListener('config', e => {
    try {
      const fresh = JSON.parse(e.data);
      cfg = fresh;
      applyToUI();
      if (editMode) renderZoneDetail();
    } catch (_) {}
  });

  es.addEventListener('ota', e => {
    try { applyOtaStatus(JSON.parse(e.data)); } catch (_) {}
  });

  es.onerror = () => {
    clearTimeout(_sseReconnectTimer);
    _sseReconnectTimer = setTimeout(() => _sseShowReconnectBadge(true), 5000);
  };

  es.onopen = () => {
    clearTimeout(_sseReconnectTimer);
    _sseReconnectTimer = null;
    _sseShowReconnectBadge(false);
  };
})();

/* ─────────────────────────────────────────────────────────────
   Actions
───────────────────────────────────────────────────────────── */
/* ─────────────────────────────────────────────────────────────
   OTA Update
───────────────────────────────────────────────────────────── */
function applyOtaStatus(d) {
  const statusEl  = document.getElementById('sy-ota-status');
  const updateBtn = document.getElementById('btn-ota-update');
  if (!statusEl || !updateBtn) return;

  const inProgress = d && d.in_progress;
  const avail      = d && d.available;
  const barText    = document.getElementById('update-bar-text');

  if (inProgress) {
    statusEl.textContent = 'Updating firmware\u2026';
    updateBtn.disabled   = true;
    document.body.classList.remove('has-update');
    document.body.classList.add('updating-firmware');
    if (barText) barText.textContent = '\u21bb UPDATING FIRMWARE\u2026';
  } else {
    statusEl.textContent = avail ? ('v' + d.latest + ' available') : 'Up to date';
    updateBtn.disabled   = !avail;
    document.body.classList.toggle('has-update', !!avail);
    document.body.classList.remove('updating-firmware');
    if (barText && avail) barText.textContent = '\u2191 FIRMWARE UPDATE AVAILABLE \u2014 CLICK TO UPDATE';
  }
}

async function loadOtaStatus() {
  try {
    const r = await fetch('/api/ota/status');
    if (!r.ok) return;
    const d = await r.json();
    applyOtaStatus(d);
  } catch (e) {}
}

async function doOtaCheck() {
  const btn = document.getElementById('btn-ota-check');
  btn.disabled   = true;
  btn.textContent = '↻ Checking…';
  try {
    const r = await fetch('/api/ota/check', { method: 'POST' });
    const d = await r.json();
    applyOtaStatus(d);
    toast(d.available ? 'UPDATE AVAILABLE' : 'UP TO DATE', 'ok');
  } catch (e) {
    toast('CHECK FAILED', 'err');
  } finally {
    btn.disabled    = false;
    btn.textContent = '↻ Check for Updates';
  }
}

async function doOtaUpdate() {
  if (!confirm('Download and install firmware update? Device will restart.')) return;
  try {
    const r = await fetch('/api/ota', { method: 'POST' });
    if (r.status === 202) {
      toast('UPDATE STARTED\u2026', 'ok');
      applyOtaStatus({ in_progress: true });
    } else if (r.status === 409) {
      toast('UPDATE ALREADY IN PROGRESS', 'err');
    } else {
      toast('UPDATE FAILED', 'err');
    }
  } catch (e) {
    toast('UPDATE FAILED', 'err');
  }
}

function goToSystem() {
  document.querySelectorAll('.tab').forEach(b => b.classList.remove('on'));
  document.querySelectorAll('.pane').forEach(p => p.classList.remove('on'));
  document.querySelector('[data-tab="system"]').classList.add('on');
  document.getElementById('pane-system').classList.add('on');
  editMode = false;
  document.getElementById('edit-banner').classList.remove('show');
}

function initOtaInterval() {
  const sl = document.getElementById('sl-ota-interval');
  if (sl) sl.addEventListener('input', updateOtaIntervalVal);
}

function updateOtaIntervalVal() {
  const sl = document.getElementById('sl-ota-interval');
  const el = document.getElementById('sy-ota-interval-v');
  if (sl && el) el.textContent = sl.value + ' h';
}

async function loadOtaInterval() {
  try {
    const r = await fetch('/api/ota/interval');
    if (!r.ok) return;
    const d = await r.json();
    const sl = document.getElementById('sl-ota-interval');
    if (sl) { sl.value = d.interval_hours; updateOtaIntervalVal(); }
  } catch (e) {}
}

async function saveOtaInterval() {
  const sl = document.getElementById('sl-ota-interval');
  if (!sl) return;
  try {
    const r = await fetch('/api/ota/interval', {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify({ interval_hours: Number(sl.value) })
    });
    const d = await r.json();
    if (d.status === 'ok') toast('SAVED', 'ok');
    else                   toast('ERROR', 'err');
  } catch (e) {
    toast('SAVE FAILED', 'err');
  }
}

async function doRestart() {
  if (!confirm('Restart the device?')) return;
  await fetch('/api/restart', { method: 'POST' });
  toast('RESTARTING…', 'ok');
}

async function doOtaFileUpload() {
  const inp = document.getElementById('inp-ota-file');
  if (!inp || !inp.files[0]) { toast('SELECT A FILE FIRST', 'err'); return; }
  const file = inp.files[0];
  if (!confirm(`Flash ${file.name} (${(file.size / 1024).toFixed(0)} KB)? Device will reboot.`)) return;
  toast('UPLOADING…', 'ok');
  try {
    const r = await fetch('/api/ota/upload', {
      method:  'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
      body:    file,
    });
    if (r.ok)                 toast('FLASHED — REBOOTING…', 'ok');
    else if (r.status === 409) toast('OTA ALREADY IN PROGRESS', 'err');
    else                       toast('UPLOAD FAILED', 'err');
  } catch (e) {
    toast('UPLOAD FAILED', 'err');
  }
}

async function doZbReset() {
  if (!confirm('Reset Zigbee stack? Device config and zone coordinates will be kept.')) return;
  await fetch('/api/zb-reset', { method: 'POST' });
  toast('ZIGBEE RESET…', 'ok');
}

async function doFactoryReset() {
  if (!confirm('Full factory reset? All config and Zigbee data will be erased.')) return;
  await fetch('/api/factory-reset', { method: 'POST' });
  toast('FACTORY RESET…', 'ok');
}

/* ─────────────────────────────────────────────────────────────
   Toast
───────────────────────────────────────────────────────────── */
let toastTimer = null;
function toast(msg, type) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.className = 'show ' + (type || '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.className = ''; }, 2200);
}

/* ─────────────────────────────────────────────────────────────
   Helpers
───────────────────────────────────────────────────────────── */
function fmtUptime(s) {
  if (s == null) return '—';
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  return `${h}h ${m}m ${sec}s`;
}

function fmtBytes(b) {
  if (b == null) return '—';
  return (b / 1024).toFixed(1) + ' KB';
}

/* ─────────────────────────────────────────────────────────────
   OTA Index URL
───────────────────────────────────────────────────────────── */
async function loadOtaIndexUrl() {
  try {
    const r = await fetch('/api/ota/index-url');
    const d = await r.json();
    const inp = document.getElementById('inp-ota-url');
    if (inp && d.url) inp.value = d.url;
  } catch (e) {}
}

async function saveOtaIndexUrl() {
  const inp = document.getElementById('inp-ota-url');
  if (!inp) return;
  try {
    const r = await fetch('/api/ota/index-url', {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify({ url: inp.value.trim() })
    });
    const d = await r.json();
    inp.value = d.url;
    toast('SAVED', 'ok');
  } catch (e) {
    toast('SAVE FAILED', 'err');
  }
}

async function resetOtaIndexUrl() {
  try {
    const r = await fetch('/api/ota/index-url', {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify({ url: '' })
    });
    const d = await r.json();
    const inp = document.getElementById('inp-ota-url');
    if (inp) inp.value = d.url;
    toast('RESET TO DEFAULT', 'ok');
  } catch (e) {
    toast('RESET FAILED', 'err');
  }
}
