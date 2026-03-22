function toggleTheme() {
  var isLight = document.documentElement.getAttribute('data-theme') === 'light';
  document.documentElement.setAttribute('data-theme', isLight ? '' : 'light');
  localStorage.setItem('theme', isLight ? 'dark' : 'light');
  updateMapTiles();
}

// ── Detection mobile ─────────────────────────────────────────────────────────
const isMobile = () => window.innerWidth <= 767;

// ── Configuration ────────────────────────────────────────────────────────────
const AUDIO_CHUNK = 4096;

// ── Auth ─────────────────────────────────────────────────────────────────────
const TOKEN_KEY = 'trottinette_token';
function getToken() { return sessionStorage.getItem(TOKEN_KEY); }
function authFetch(url, opts = {}) {
  const token = getToken();
  return fetch(url, {
    ...opts,
    headers: { ...opts.headers, ...(token ? { Authorization: `Bearer ${token}` } : {}) }
  });
}
function logout() { sessionStorage.removeItem(TOKEN_KEY); location.reload(); }

// ── Login ────────────────────────────────────────────────────────────────────
document.getElementById('login-submit').addEventListener('click', doLogin);
document.getElementById('login-pass').addEventListener('keydown', e => { if (e.key === 'Enter') doLogin(); });
document.getElementById('login-user').addEventListener('keydown', e => { if (e.key === 'Enter') document.getElementById('login-pass').focus(); });

async function doLogin() {
  const btn   = document.getElementById('login-submit');
  const card  = document.getElementById('login-card');
  const errEl = document.getElementById('login-error');
  const user  = document.getElementById('login-user').value.trim();
  const pass  = document.getElementById('login-pass').value;
  errEl.textContent = ''; btn.disabled = true; btn.textContent = 'CONNEXION...';
  try {
    const r    = await fetch('/login', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({username:user,password:pass}) });
    const data = await r.json();
    if (!r.ok) {
      errEl.textContent = data.error || 'Erreur inconnue';
      card.classList.remove('shake'); void card.offsetWidth; card.classList.add('shake');
      document.getElementById('login-pass').value = '';
      document.getElementById('login-user').classList.add('error');
      document.getElementById('login-pass').classList.add('error');
    } else {
      sessionStorage.setItem(TOKEN_KEY, data.token);
      const overlay = document.getElementById('login-overlay');
      overlay.classList.add('hidden');
      setTimeout(() => { overlay.style.display = 'none'; }, 300);
      initApp();
    }
  } catch { errEl.textContent = 'Erreur de connexion au serveur'; }
  finally { btn.disabled = false; btn.textContent = 'CONNEXION'; }
}

(async function checkAuth() {
  const token = getToken();
  if (!token) return;
  try {
    const r = await fetch('/status', { headers: { Authorization: `Bearer ${token}` } });
    if (r.ok) { document.getElementById('login-overlay').style.display = 'none'; initApp(); }
  } catch (_) {}
})();

// ── Layout ───────────────────────────────────────────────────────────────────
function applyLayout() {
  const mobile = isMobile();
  document.getElementById('desktop-layout').style.display = mobile ? 'none' : '';
  document.getElementById('mobile-layout').style.display  = mobile ? 'flex' : 'none';
}
applyLayout();
window.addEventListener('resize', applyLayout);

// ── Bottom sheets (mobile) ───────────────────────────────────────────────────
let activeSheet = null;

let _sheetScrollY = 0;

function toggleSheet(name) {
  if (activeSheet === name) {
    closeSheet();
    return;
  }
  closeSheet();
  activeSheet = name;
  const sheet = document.getElementById('sheet-' + name);
  const overlay = document.getElementById('sheet-overlay');
  if (sheet) {
    overlay.classList.add('visible');
    sheet.classList.add('open');
    sheet.style.removeProperty('transform');
    document.body.classList.add('sheet-active');
  }
  document.querySelectorAll('.sheet-pill').forEach(p => {
    p.classList.toggle('active', p.dataset.sheet === name);
  });
}

function closeSheet() {
  document.querySelectorAll('.bottom-sheet').forEach(s => {
    s.classList.remove('open');
    s.style.removeProperty('transform');
  });
  activeSheet = null;
  document.getElementById('sheet-overlay').classList.remove('visible');
  document.querySelectorAll('.sheet-pill').forEach(p => p.classList.remove('active'));
  document.body.classList.remove('sheet-active');
}

// ── Settings dropdown ────────────────────────────────────────────────────────
function toggleSettings() {
  const dd = document.getElementById('settings-dropdown');
  dd.classList.toggle('open');
}
// Close settings when clicking outside
document.addEventListener('click', (e) => {
  const dd = document.getElementById('settings-dropdown');
  const btn = document.getElementById('settings-btn');
  if (!dd.contains(e.target) && !btn.contains(e.target)) {
    dd.classList.remove('open');
  }
});

// ── Etat ─────────────────────────────────────────────────────────────────────
let simMode       = false;
let simInterval   = null;
let sseSource     = null;
let audioCtx      = null;
let mediaStream   = null;
let processor     = null;
let isRecording   = false;
let audioQueue    = [];
let isPlayingAudio= false;
let map, mapDesktop;
let lastPos = [46.8219, -71.2372];
const scooterMarkers = new Map();
let selectedScooterIdLocal = null;

// ── Carte ────────────────────────────────────────────────────────────────────
function makeScooterIcon(color = '#00f8ce') {
  // Icône trottinette électrique — guidon + deck + roues
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="36" height="36" viewBox="0 0 36 36">
    <circle cx="18" cy="18" r="16" fill="${color}22" stroke="${color}" stroke-width="1.5"/>
    <circle cx="11" cy="26" r="3" fill="none" stroke="${color}" stroke-width="1.8"/>
    <circle cx="25" cy="26" r="3" fill="none" stroke="${color}" stroke-width="1.8"/>
    <line x1="25" y1="26" x2="23" y2="12" stroke="${color}" stroke-width="2" stroke-linecap="round"/>
    <line x1="23" y1="12" x2="20" y2="8" stroke="${color}" stroke-width="2.5" stroke-linecap="round"/>
    <line x1="17" y1="8" x2="23" y2="8" stroke="${color}" stroke-width="2" stroke-linecap="round"/>
    <line x1="11" y1="26" x2="22" y2="24" stroke="${color}" stroke-width="2.5" stroke-linecap="round"/>
  </svg>`;
  return L.divIcon({
    html: svg,
    iconSize: [36, 36], iconAnchor: [18, 18],
    className: ''
  });
}

const DARK_TILES = 'https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png';
const LIGHT_TILES = 'https://{s}.basemaps.cartocdn.com/light_all/{z}/{x}/{y}{r}.png';
let tileLayerMobile, tileLayerDesktop;

function getTilesUrl() {
  return document.documentElement.getAttribute('data-theme') === 'light' ? LIGHT_TILES : DARK_TILES;
}

function updateMapTiles() {
  const url = getTilesUrl();
  if (tileLayerMobile) tileLayerMobile.setUrl(url);
  if (tileLayerDesktop) tileLayerDesktop.setUrl(url);
}

function initMap() {
  map = L.map('map', { zoomControl: false }).setView(lastPos, 15);
  tileLayerMobile = L.tileLayer(getTilesUrl(), { attribution: '&copy; CartoDB' }).addTo(map);

  mapDesktop = L.map('map-desktop', { zoomControl: true }).setView(lastPos, 15);
  tileLayerDesktop = L.tileLayer(getTilesUrl(), { attribution: '&copy; CartoDB' }).addTo(mapDesktop);
}

function updateScooterMarker(scooterId, lat, lon) {
  if (!lat || !lon) return;
  const pos = [lat, lon];
  const isSelected = scooterId === selectedScooterIdLocal;
  const color = isSelected ? '#00f8ce' : '#888';

  let markers = scooterMarkers.get(scooterId);
  if (!markers) {
    markers = {
      mobile:  L.marker(pos, { icon: makeScooterIcon(color) }).addTo(map),
      desktop: L.marker(pos, { icon: makeScooterIcon(color) }).addTo(mapDesktop)
    };
    const shortId = scooterId.split(':').slice(-2).join(':');
    markers.mobile.bindTooltip(shortId, { permanent: false });
    markers.desktop.bindTooltip(shortId, { permanent: false });
    scooterMarkers.set(scooterId, markers);
  } else {
    markers.mobile.setLatLng(pos);
    markers.desktop.setLatLng(pos);
  }

  if (isSelected) {
    lastPos = pos;
    map.panTo(pos);
    mapDesktop.panTo(pos);
  }
}

function refreshMarkerColors() {
  for (const [id, markers] of scooterMarkers) {
    const color = id === selectedScooterIdLocal ? '#00f8ce' : '#888';
    markers.mobile.setIcon(makeScooterIcon(color));
    markers.desktop.setIcon(makeScooterIcon(color));
  }
}

function removeScooterMarker(scooterId) {
  const markers = scooterMarkers.get(scooterId);
  if (markers) {
    map.removeLayer(markers.mobile);
    mapDesktop.removeLayer(markers.desktop);
    scooterMarkers.delete(scooterId);
  }
}

function updateMapPos(lat, lon) {
  if (selectedScooterIdLocal) {
    updateScooterMarker(selectedScooterIdLocal, lat, lon);
  }
}

// ── Speedometer + telemetry ──────────────────────────────────────────────────
const SPEED_MAX = 30;

function updateSpeedometer(speed) {
  const pct = Math.min(Math.max(speed / SPEED_MAX, 0), 1) * 100;
  document.getElementById('speedo-desktop').style.setProperty('--speed-pct', pct);
  document.getElementById('speedo-mobile').style.setProperty('--speed-pct', pct);
  document.getElementById('val-speed').textContent = Math.round(speed);
  document.getElementById('val-speed-m').textContent = Math.round(speed);
}

function updateSignalBars(rssi) {
  // rssi en dBm : -113 (aucun) à -51 (excellent)
  // Convertir en 0-4 barres
  let bars = 0;
  if (rssi > -70) bars = 4;
  else if (rssi > -85) bars = 3;
  else if (rssi > -100) bars = 2;
  else if (rssi > -110) bars = 1;
  ['signal-bars', 'signal-bars-m'].forEach(id => {
    const el = document.getElementById(id);
    if (!el) return;
    const spans = el.querySelectorAll('.sbar');
    spans.forEach((s, i) => {
      s.style.opacity = (i < bars) ? '1' : '0.25';
      s.style.background = bars <= 1 ? '#f44' : bars <= 2 ? '#fa0' : '#4f4';
    });
  });
}

function updatePill(id, idM, value, decimals) {
  const text = value.toFixed(decimals);
  const el = document.getElementById(id); if (el) el.textContent = text;
  const elM = document.getElementById(idM); if (elM) elM.textContent = text;
}

function applyTelemetry(data) {
  if (data.speed   !== undefined) updateSpeedometer(data.speed);
  if (data.voltage !== undefined) updatePill('val-voltage', 'val-voltage-m', data.voltage, 1);
  if (data.current !== undefined) updatePill('val-current', 'val-current-m', data.current, 1);
  if (data.temp    !== undefined) updatePill('val-temp',    'val-temp-m',    data.temp,    0);
  if (data.rssi !== undefined) updateSignalBars(data.rssi);
  if (data.conn !== undefined) {
    const el = document.getElementById('val-conn'); if (el) el.textContent = data.conn;
    const elM = document.getElementById('val-conn-m'); if (elM) elM.textContent = data.conn;
  }
  if (data.gps_fix !== undefined) {
    const fix = data.gps_fix;
    let label = 'Aucun', color = 'var(--muted)';
    if (fix === 'ok') { label = 'GPS'; color = '#4f4'; }
    else if (fix && fix.startsWith('wifi')) { label = 'WiFi'; color = '#4cf'; }
    else if (fix === 'db') { label = 'BD'; color = '#fa0'; }
    else if (fix && fix !== 'none') { label = 'Cell'; color = '#fa0'; }
    ['gps-status', 'gps-status-m'].forEach(id => {
      const el = document.getElementById(id);
      if (el) { el.textContent = label; el.style.color = color; }
    });
  }
  if (data.lat !== undefined && data.lon !== undefined) updateMapPos(data.lat, data.lon);
}

// ── Journal vocal ────────────────────────────────────────────────────────────
let _currentAiEntry = null;

function getLogEl() {
  return isMobile()
    ? document.getElementById('voice-log')
    : document.getElementById('voice-log-desktop');
}

function addLog(type, text) {
  _currentAiEntry = null;
  _appendLogEntry(type, text);
}

function appendAiDelta(delta) {
  ['voice-log', 'voice-log-desktop'].forEach(id => {
    const el = document.getElementById(id); if (!el) return;
    let entry = el._currentAiEntry;
    if (!entry) {
      const now  = new Date();
      const time = `${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}`;
      entry = document.createElement('div');
      entry.className = 'log-entry ai';
      entry.innerHTML = `<span class="log-time">${time}</span><span class="ai-text"></span>`;
      el.appendChild(entry);
      el._currentAiEntry = entry;
      while (el.children.length > 200) el.removeChild(el.firstChild);
    }
    entry.querySelector('.ai-text').textContent += delta;
    el.scrollTop = el.scrollHeight;
  });
}

function _appendLogEntry(type, text) {
  ['voice-log', 'voice-log-desktop'].forEach(id => {
    const el = document.getElementById(id); if (!el) return;
    el._currentAiEntry = null;
    const div = document.createElement('div');
    div.className = `log-entry ${type}`;
    const now  = new Date();
    const time = `${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}`;
    div.innerHTML = `<span class="log-time">${time}</span>${escapeHtml(text)}`;
    el.appendChild(div);
    el.scrollTop = el.scrollHeight;
    while (el.children.length > 200) el.removeChild(el.firstChild);
  });
}

function pad(n) { return String(n).padStart(2,'0'); }
function escapeHtml(s) { return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }

// ── Indicateurs ──────────────────────────────────────────────────────────────
function setDot(id, state) {
  const el = document.getElementById(id);
  if (el) el.className = `dot${state ? ' '+state : ''}`;
}

// ── SSE ──────────────────────────────────────────────────────────────────────
function connectSSE() {
  if (simMode) return;
  if (sseSource) sseSource.close();
  const tok = getToken();
  sseSource = new EventSource(`/stream?token=${encodeURIComponent(tok || '')}`);
  sseSource.onopen  = () => { setDot('dot-sse', 'on'); fetchStatus(); };
  sseSource.onerror = () => { setDot('dot-sse', 'err'); setTimeout(connectSSE, 3000); };
  sseSource.onmessage = (e) => {
    try { handleSSEMessage(JSON.parse(e.data)); } catch (_) {}
  };
}

function handleSSEMessage(msg) {
  switch (msg.type) {
    case 'transcript':  addLog('transcript', msg.text); break;
    case 'ai_response': appendAiDelta(msg.text); break;
    case 'audio':       enqueueAudio(msg.data); break;
    case 'cmd':
      addLog('cmd', `${msg.action.toUpperCase()}${msg.intensity != null ? ' @ ' + Math.round(msg.intensity * 100) + '%' : ''}`);
      break;
    case 'telemetry':
      if (msg.scooterId) {
        updateScooterMarker(msg.scooterId, msg.lat, msg.lon);
      }
      if (!msg.scooterId || msg.scooterId === selectedScooterIdLocal) {
        applyTelemetry(msg);
      }
      break;
    case 'esp32_log': appendDebugLog(msg.msg); break;
    case 'ota_progress':
      document.getElementById('ota-bar').style.width = msg.percent + '%';
      document.getElementById('ota-status').textContent = 'Flash ESP32: ' + msg.percent + '%';
      break;
    case 'ota_build':
      handleBuildEvent(msg);
      break;
    case 'scooter_list':
      updateScooterList(msg.scooters);
      break;
    case 'lock_changed':
      if (!msg.scooterId || msg.scooterId === selectedScooterIdLocal) {
        updateLockUI(msg.locked);
      }
      break;
    case 'lock_blocked':
      addLog('warn', msg.message || 'Commande bloquee -- trottinette verrouillee');
      break;
    case 'ride_started':
      if (msg.scooterId === selectedScooterIdLocal) {
        activeRideId = msg.rideId;
        rideStartTime = new Date().toISOString();
        updateLockUI(false);
        if (!rideTimerInterval) rideTimerInterval = setInterval(updateRideTimer, 1000);
        addLog('sys', `Course ${msg.rideId} demarree`);
      }
      break;
    case 'ride_ended':
      if (msg.scooterId === selectedScooterIdLocal) {
        activeRideId = null;
        rideStartTime = null;
        if (rideTimerInterval) { clearInterval(rideTimerInterval); rideTimerInterval = null; }
        const timerEl = document.getElementById('ride-timer');
        if (timerEl) timerEl.style.display = 'none';
        hideRideTimerSpeedo();
        updateLockUI(true);
        addLog('sys', `Course terminee (${msg.ride?.durationSec || 0}s)`);
      }
      break;
  }
}

// ── Lecture audio PCM16 ──────────────────────────────────────────────────────
let assistantMuted = false;

function toggleMute() {
  assistantMuted = !assistantMuted;
  const item = document.getElementById('mute-item');
  if (item) item.classList.toggle('active', assistantMuted);
  if (assistantMuted) { audioQueue.length = 0; isPlayingAudio = false; }
}

function enqueueAudio(base64) {
  if (assistantMuted) return;
  audioQueue.push(base64);
  if (!isPlayingAudio) playNextAudio();
}

async function playNextAudio() {
  if (!audioQueue.length) { isPlayingAudio = false; return; }
  isPlayingAudio = true;
  const b64 = audioQueue.shift();
  try {
    if (!audioCtx) audioCtx = new AudioContext({ sampleRate: 24000 });
    const raw   = atob(b64);
    const bytes = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
    const isWav = bytes[0] === 0x52 && bytes[1] === 0x49;
    if (isWav) {
      const decoded = await audioCtx.decodeAudioData(bytes.buffer.slice(0));
      const src = audioCtx.createBufferSource();
      src.buffer = decoded; src.connect(audioCtx.destination);
      src.start(); src.onended = playNextAudio; return;
    }
    const samples = bytes.length / 2;
    const buf = audioCtx.createBuffer(1, samples, 24000);
    const ch  = buf.getChannelData(0);
    const view = new DataView(bytes.buffer, bytes.byteOffset);
    for (let i = 0; i < samples; i++) ch[i] = view.getInt16(i * 2, true) / 32768;
    const src = audioCtx.createBufferSource();
    src.buffer = buf; src.connect(audioCtx.destination);
    src.start(); src.onended = playNextAudio;
  } catch (err) { console.warn('[audio]', err); playNextAudio(); }
}

// ── Statut proxy ─────────────────────────────────────────────────────────────
function fetchStatus() {
  authFetch('/status')
    .then(r => r.ok ? r.json() : Promise.reject())
    .then(s => {
      document.getElementById('mode-badge').textContent = s.mode.toUpperCase();
      setDot('dot-ai', s.openai_connected || s.mode === 'local' ? 'on' : 'warn');
      setDot('dot-ws', s.scooter_connected ? 'on' : 'err');
      const fwEl = document.getElementById('fw-version');
      if (fwEl) fwEl.textContent = s.fw_version ? 'FW ' + s.fw_version : '';
      const otaInstalled = document.getElementById('ota-installed');
      if (otaInstalled) otaInstalled.textContent = s.fw_version || '\u2014';
      esp32Debug = !!s.debug;
      updateDebugBtn();
      if (s.selected_scooter) selectedScooterIdLocal = s.selected_scooter;
      const countEl = document.getElementById('scooter-count');
      if (countEl) countEl.textContent = s.scooter_count || 0;
      updateLockUI(s.locked !== false);
      if (s.active_ride) {
        activeRideId = s.active_ride.id;
        rideStartTime = s.active_ride.startedAt;
        if (!rideTimerInterval) rideTimerInterval = setInterval(updateRideTimer, 1000);
        updateLockUI(false);
      }
    })
    .catch(() => {});
  fetchScooters();
}

// ── Multi-trottinettes ──────────────────────────────────────────────────────
function fetchScooters() {
  authFetch('/api/scooters')
    .then(r => r.ok ? r.json() : Promise.reject())
    .then(list => updateScooterList(list))
    .catch(() => {});
}

function updateScooterList(list) {
  const select = document.getElementById('scooter-select');
  const countEl = document.getElementById('scooter-count');
  if (!select) return;

  const currentVal = select.value;
  select.innerHTML = '';

  if (list.length === 0) {
    select.innerHTML = '<option value="">Aucune</option>';
    selectedScooterIdLocal = null;
  } else {
    for (const s of list) {
      const opt = document.createElement('option');
      opt.value = s.id;
      const shortId = s.name || s.id.split(':').slice(-2).join(':');
      const status = s.connected ? '' : ' [hors ligne]';
      opt.textContent = `${shortId}${status}`;
      opt.selected = s.selected;
      if (s.selected) {
        selectedScooterIdLocal = s.id;
        updateLockUI(s.locked);
        if (s.activeRide) {
          activeRideId = s.activeRide.id;
          rideStartTime = s.activeRide.startedAt;
          if (!rideTimerInterval) rideTimerInterval = setInterval(updateRideTimer, 1000);
        } else {
          activeRideId = null;
          rideStartTime = null;
          if (rideTimerInterval) { clearInterval(rideTimerInterval); rideTimerInterval = null; }
          const timerEl = document.getElementById('ride-timer');
          if (timerEl) timerEl.style.display = 'none';
          hideRideTimerSpeedo();
        }
      }
      select.appendChild(opt);
    }
  }

  if (countEl) countEl.textContent = list.length;
  setDot('dot-ws', list.some(s => s.selected && s.connected) ? 'on' : 'err');

  const connectedIds = new Set(list.filter(s => s.connected).map(s => s.id));
  for (const id of scooterMarkers.keys()) {
    if (!connectedIds.has(id)) removeScooterMarker(id);
  }
  for (const s of list) {
    if (s.connected && s.telemetry.lat && s.telemetry.lon) {
      updateScooterMarker(s.id, s.telemetry.lat, s.telemetry.lon);
    }
  }
  refreshMarkerColors();
}

async function selectScooter(id) {
  try {
    const r = await authFetch('/api/scooters/select', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id })
    });
    if (r.ok) {
      selectedScooterIdLocal = id;
      refreshMarkerColors();
      fetchStatus();
    }
  } catch (_) {}
}

// ── Commandes manuelles ──────────────────────────────────────────────────────
function getIntensity() {
  const d = document.getElementById('intensity-slider');
  const m = document.getElementById('intensity-slider-m');
  const el = isMobile() ? m : d;
  return el ? parseInt(el.value) / 100 : 0.5;
}

function syncSlider(src) {
  const val = src.value + '%';
  document.getElementById('intensity-val-m').textContent = val;
  const d = document.getElementById('intensity-slider');
  if (d) { d.value = src.value; document.getElementById('intensity-val').textContent = val; }
}

function sendCmd(action) {
  if (simMode) { addLog('cmd', `[SIM] ${action.toUpperCase()}`); return; }
  if (scooterLocked && ['avancer', 'freiner', 'vitesse_lente', 'vitesse_moyenne', 'vitesse_haute'].includes(action)) {
    addLog('warn', 'Trottinette verrouillee -- demarrez une course ou deverrouillez');
    return;
  }
  authFetch('/cmd', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ action, intensity: getIntensity() })
  }).catch(console.error);
}

// ── Musique ──────────────────────────────────────────────────────────────────
function musicCmd(action) {
  const body = { action };
  if (action === 'play') {
    const sel = document.getElementById('music-track-m') || document.getElementById('music-track');
    body.track = parseInt(sel.value);
  }
  authFetch('/api/music', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body)
  }).catch(console.error);
}

// ── Verrouillage / Deverrouillage ───────────────────────────────────────────
let scooterLocked = true;
let activeRideId = null;
let rideStartTime = null;
let rideTimerInterval = null;

function updateLockUI(locked) {
  scooterLocked = locked;

  // Lock overlay on speedometer
  document.getElementById('lock-overlay-desktop').classList.toggle('visible', locked);
  document.getElementById('lock-overlay-mobile').classList.toggle('visible', locked);

  // Riding state on speedometer
  const isRiding = !!activeRideId;
  document.getElementById('speedo-wrap-desktop').classList.toggle('riding', isRiding);
  document.getElementById('speedo-wrap-mobile').classList.toggle('riding', isRiding);

  updatePlayBtn();
}

function hideRideTimerSpeedo() {
  ['ride-timer-speedo', 'ride-timer-speedo-m'].forEach(id => {
    const el = document.getElementById(id);
    if (el) el.style.display = 'none';
  });
}

function updateRideTimer() {
  if (!rideStartTime) return;
  const elapsed = Math.round((Date.now() - new Date(rideStartTime).getTime()) / 1000);
  const min = Math.floor(elapsed / 60);
  const sec = elapsed % 60;
  const timeStr = `${min}:${sec.toString().padStart(2, '0')}`;

  const timerEl = document.getElementById('ride-timer');
  if (timerEl) {
    timerEl.style.display = '';
    timerEl.textContent = timeStr;
  }
  // Show ride timer in speedometer
  ['ride-timer-speedo', 'ride-timer-speedo-m'].forEach(id => {
    const el = document.getElementById(id);
    if (el) {
      el.style.display = '';
      el.textContent = timeStr;
    }
  });
}

// Bouton play unifie : play = demarrer course (deverrouille auto), stop = terminer course (reverrouille auto)
async function togglePlay() {
  try {
    if (activeRideId) {
      // Arreter la course (reverrouille automatiquement)
      const r = await authFetch('/api/rides/end', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ rideId: activeRideId })
      });
      if (r.ok) {
        const data = await r.json();
        activeRideId = null;
        rideStartTime = null;
        if (rideTimerInterval) { clearInterval(rideTimerInterval); rideTimerInterval = null; }
        const timerEl = document.getElementById('ride-timer');
        if (timerEl) timerEl.style.display = 'none';
        hideRideTimerSpeedo();
        updateLockUI(true);
        updatePlayBtn();
        addLog('sys', `Course terminee`);
      }
    } else {
      // Demarrer une course (deverrouille automatiquement)
      const r = await authFetch('/api/rides/start', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({})
      });
      if (r.ok) {
        const data = await r.json();
        activeRideId = data.rideId;
        rideStartTime = new Date().toISOString();
        updateLockUI(false);
        if (!rideTimerInterval) rideTimerInterval = setInterval(updateRideTimer, 1000);
        updatePlayBtn();
        addLog('sys', `Course demarree`);
      }
    }
  } catch (err) { console.error('[play]', err); }
}

function updatePlayBtn() {
  const btn = document.getElementById('play-btn');
  const icon = document.getElementById('play-icon');
  const label = document.getElementById('play-label');
  if (!btn || !icon) return;
  if (activeRideId) {
    icon.innerHTML = '<rect x="4" y="4" width="16" height="16" rx="2"/>';
    btn.style.background = 'var(--red)';
    btn.style.borderColor = 'var(--red)';
    btn.style.color = '#fff';
    btn.title = 'Arreter la course';
    if (label) label.textContent = 'STOP';
  } else {
    icon.innerHTML = '<polygon points="6,3 20,12 6,21"/>';
    btn.style.background = 'var(--green)';
    btn.style.borderColor = 'var(--green)';
    btn.style.color = '#000';
    btn.title = 'Demarrer une course';
    if (label) label.textContent = 'GO';
  }
  // Sync mobile GO overlay
  const mBtn = document.getElementById('mobile-go-btn');
  const mIcon = document.getElementById('mobile-go-icon');
  const mLabel = document.getElementById('mobile-go-label');
  if (mBtn) {
    if (activeRideId) {
      // Course en cours → cacher l'overlay pour voir la vitesse
      mBtn.classList.add('hidden');
    } else {
      mBtn.classList.remove('hidden');
      mBtn.classList.remove('riding');
      if (mIcon) mIcon.innerHTML = '<polygon points="8,5 19,12 8,19"/>';
      if (mLabel) mLabel.textContent = 'GO';
    }
  }
}

async function toggleLock() {
  // Garde pour compatibilite (appels internes)
  try {
    const r = await authFetch('/api/scooters/lock', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ locked: !scooterLocked })
    });
    if (r.ok) {
      const data = await r.json();
      updateLockUI(data.locked);
      addLog('sys', `Trottinette ${data.locked ? 'verrouillee' : 'deverrouillee'}`);
    }
  } catch (err) { console.error('[lock]', err); }
}

// toggleRide remplace par togglePlay — garde pour compatibilite SSE
async function toggleRide() { return togglePlay(); }

// ── Bouton PTT ───────────────────────────────────────────────────────────────
const PTT_IDS = ['ptt-btn', 'ptt-btn-desktop'];

function setVoiceState(recording) {
  PTT_IDS.forEach(id => {
    const btn = document.getElementById(id); if (!btn) return;
    btn.classList.toggle('active', recording);
    // Change SVG icon when recording
    if (recording) {
      btn.innerHTML = '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="6" fill="currentColor"/></svg>';
    } else {
      btn.innerHTML = '<svg viewBox="0 0 24 24"><path d="M12 1a3 3 0 0 0-3 3v8a3 3 0 0 0 6 0V4a3 3 0 0 0-3-3z"/><path d="M19 10v2a7 7 0 0 1-14 0v-2" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"/><line x1="12" y1="19" x2="12" y2="23" stroke="currentColor" stroke-width="2" stroke-linecap="round"/><line x1="8" y1="23" x2="16" y2="23" stroke="currentColor" stroke-width="2" stroke-linecap="round"/></svg>';
    }
  });
  const statEl = document.getElementById('voice-status');
  if (statEl) statEl.textContent = recording ? 'Actif -- appuyer pour arreter' : 'Appuyer pour parler';
  const statD = document.getElementById('voice-status-desktop');
  if (statD) statD.textContent = recording ? 'Actif -- cliquer pour arreter' : 'Cliquer pour parler';
}

async function startRecording() {
  if (simMode) { simulateVoiceCmd(); return; }
  try {
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
      addLog('system', 'Microphone non disponible (HTTPS requis)');
      return;
    }
    if (!audioCtx) audioCtx = new AudioContext({ sampleRate: 16000 });
    // Reprendre l'AudioContext si suspendu (politique autoplay mobile)
    if (audioCtx.state === 'suspended') await audioCtx.resume();
    mediaStream = await navigator.mediaDevices.getUserMedia({ audio: { sampleRate: 16000, channelCount: 1 } });
    const source = audioCtx.createMediaStreamSource(mediaStream);
    processor = audioCtx.createScriptProcessor(AUDIO_CHUNK, 1, 1);
    processor.onaudioprocess = (e) => {
      if (!isRecording) return;
      sendAudioChunk(float32ToPCM16(e.inputBuffer.getChannelData(0)));
    };
    source.connect(processor);
    processor.connect(audioCtx.destination);
    isRecording = true;
    setVoiceState(true);
    addLog('system', 'Microphone actif');
  } catch (err) {
    console.error('[mic]', err);
    addLog('system', 'Microphone inaccessible : ' + err.message);
  }
}

function stopRecording() {
  if (!isRecording) return;
  isRecording = false;
  if (processor)   { processor.disconnect(); processor = null; }
  if (mediaStream) { mediaStream.getTracks().forEach(t => t.stop()); mediaStream = null; }
  setVoiceState(false);
  addLog('system', 'Microphone coupe');
}

function toggleRecording() {
  if (isRecording) stopRecording(); else startRecording();
}

function float32ToPCM16(input) {
  const buf = new ArrayBuffer(input.length * 2);
  const view = new DataView(buf);
  for (let i = 0; i < input.length; i++) {
    const s = Math.max(-1, Math.min(1, input[i]));
    view.setInt16(i * 2, s < 0 ? s * 32768 : s * 32767, true);
  }
  return new Uint8Array(buf);
}

function sendAudioChunk(pcm16) {
  authFetch('/audio', {
    method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: pcm16
  }).catch(console.error);
}

// ── Monitor micro ESP32 ──────────────────────────────────────────────────────
let monitorWs = null;
let monitorCtx = null;
let monitorActive = false;
let monitorNextTime = 0;
const MONITOR_JITTER_MS = 0.25;

function toggleMonitor() {
  if (monitorActive) stopMonitor(); else startMonitor();
}

function startMonitor() {
  monitorCtx = new AudioContext({ sampleRate: 16000 });
  monitorNextTime = 0;
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  monitorWs = new WebSocket(proto + '//' + location.host + '/ws-debug-audio');
  monitorWs.binaryType = 'arraybuffer';
  monitorWs.onopen = () => {
    monitorActive = true;
    updateMonitorBtn();
    addLog('system', 'Monitor micro ESP32 actif');
  };
  monitorWs.onclose = () => { stopMonitor(); };
  monitorWs.onmessage = (e) => {
    if (!monitorCtx) return;
    const pcm16 = new Int16Array(e.data);
    const buf = monitorCtx.createBuffer(1, pcm16.length, 16000);
    const ch = buf.getChannelData(0);
    for (let i = 0; i < pcm16.length; i++) ch[i] = pcm16[i] / 32768;

    const now = monitorCtx.currentTime;
    if (monitorNextTime < now) {
      monitorNextTime = now + MONITOR_JITTER_MS;
    }

    const src = monitorCtx.createBufferSource();
    src.buffer = buf;
    src.connect(monitorCtx.destination);
    src.start(monitorNextTime);
    monitorNextTime += buf.duration;
  };
}

function stopMonitor() {
  monitorActive = false;
  if (monitorWs) { monitorWs.close(); monitorWs = null; }
  if (monitorCtx) { monitorCtx.close(); monitorCtx = null; }
  updateMonitorBtn();
}

function updateMonitorBtn() {
  const item = document.getElementById('monitor-item');
  if (item) item.classList.toggle('active', monitorActive);
}

// ── Debug ESP32 ──────────────────────────────────────────────────────────────
let esp32Debug = false;

function toggleDebug() {
  esp32Debug = !esp32Debug;
  authFetch('/debug', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ enabled: esp32Debug })
  }).catch(console.error);
  updateDebugBtn();
}

function updateDebugBtn() {
  const item = document.getElementById('debug-item');
  if (item) item.classList.toggle('active', esp32Debug);
  const modal = document.getElementById('debug-modal');
  if (modal) modal.classList.toggle('open', esp32Debug);
  const fwEl = document.getElementById('modal-fw-version');
  const fwMain = document.getElementById('fw-version');
  if (fwEl && fwMain) fwEl.textContent = fwMain.textContent;
}

let debugPaused = false;

function toggleDebugPause() {
  debugPaused = !debugPaused;
  const btn = document.getElementById('debug-pause-btn');
  if (btn) {
    btn.textContent = debugPaused ? '\u25b6' : '\u23f8';
    btn.style.color = debugPaused ? 'var(--yellow)' : 'var(--muted)';
  }
}

function toggleDebugMode(enabled) {
  authFetch('/debug', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ enabled })
  }).catch(console.error);
}

function clearDebugLog() {
  ['debug-log', 'debug-log-mobile', 'debug-log-panel'].forEach(id => {
    const el = document.getElementById(id);
    if (el) el.innerHTML = '';
  });
}

function appendDebugLog(msg) {
  ['debug-log', 'debug-log-mobile', 'debug-log-panel'].forEach(id => {
    const el = document.getElementById(id);
    if (!el) return;
    const line = document.createElement('div');
    line.textContent = new Date().toLocaleTimeString() + ' ' + msg;
    el.appendChild(line);
    while (el.children.length > 500) el.removeChild(el.firstChild);
    if (!debugPaused) el.scrollTop = el.scrollHeight;
  });
}

// ── Upload firmware OTA ──────────────────────────────────────────────────────
document.addEventListener('DOMContentLoaded', () => {
  const fwFile = document.getElementById('fw-file');
  if (fwFile) fwFile.addEventListener('change', () => {
    const btn = document.getElementById('ota-upload-btn');
    if (btn) btn.disabled = !fwFile.files.length;
  });
});

async function uploadFirmware() {
  const fwFile = document.getElementById('fw-file');
  if (!fwFile || !fwFile.files.length) return;
  const file = fwFile.files[0];
  const statusEl = document.getElementById('ota-status');
  const barEl = document.getElementById('ota-bar');
  const btn = document.getElementById('ota-upload-btn');

  statusEl.textContent = 'Upload en cours...';
  barEl.style.width = '0';
  btn.disabled = true;

  try {
    const r = await authFetch('/ota', {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: await file.arrayBuffer()
    });
    const data = await r.json();
    if (r.ok) {
      statusEl.textContent = data.message || 'Firmware flashe !';
      barEl.style.width = '100%';
      setTimeout(fetchStatus, 8000);
    } else {
      statusEl.textContent = 'Erreur: ' + (data.error || r.status);
      barEl.style.background = 'var(--red)';
    }
  } catch (err) {
    statusEl.textContent = 'Erreur: ' + err.message;
  }
  btn.disabled = false;
}

// ── Mise a jour firmware depuis GitHub Releases ──────────────────────────────
let latestRelease = null;

function compareVersions(a, b) {
  const pa = a.split('.').map(Number);
  const pb = b.split('.').map(Number);
  for (let i = 0; i < 3; i++) {
    if ((pa[i] || 0) !== (pb[i] || 0)) return (pa[i] || 0) - (pb[i] || 0);
  }
  return 0;
}

async function checkFirmwareUpdate() {
  const statusEl = document.getElementById('ota-status');
  const availEl = document.getElementById('ota-available');
  const badgeEl = document.getElementById('ota-update-badge');
  const ghBtn = document.getElementById('ota-github-btn');
  const checkBtn = document.getElementById('ota-check-btn');

  statusEl.textContent = 'Verification...';
  checkBtn.disabled = true;

  try {
    const r = await authFetch('/api/releases/latest');
    if (!r.ok) {
      const data = await r.json();
      throw new Error(data.error || 'Erreur ' + r.status);
    }
    latestRelease = await r.json();
    availEl.textContent = latestRelease.version;

    if (latestRelease.build_ready) {
      availEl.textContent += ' (pret)';
    } else {
      availEl.textContent += ' (compilation...)';
    }

    const installedEl = document.getElementById('ota-installed');
    const installed = installedEl ? installedEl.textContent : '\u2014';

    if (installed !== '\u2014' && compareVersions(latestRelease.version, installed) > 0) {
      badgeEl.style.display = 'inline';
      ghBtn.disabled = false;
      statusEl.textContent = latestRelease.build_ready
        ? 'Nouvelle version prete a flasher !'
        : 'Nouvelle version disponible (compilation en cours sur le serveur)';
    } else if (installed !== '\u2014' && compareVersions(latestRelease.version, installed) <= 0) {
      badgeEl.style.display = 'none';
      ghBtn.disabled = true;
      statusEl.textContent = 'Firmware a jour.';
    } else {
      ghBtn.disabled = false;
      badgeEl.style.display = 'none';
      statusEl.textContent = '';
    }

    if (latestRelease.sha256) {
      const sha256El = document.getElementById('ota-sha256');
      sha256El.textContent = 'SHA256: ' + latestRelease.sha256;
      sha256El.style.display = 'block';
    }

    loadReleasesList();
  } catch (err) {
    statusEl.textContent = 'Erreur: ' + err.message;
    availEl.textContent = '\u2014';
    badgeEl.style.display = 'none';
    ghBtn.disabled = true;
  }
  checkBtn.disabled = false;
}

async function loadReleasesList() {
  const select = document.getElementById('ota-release-select');
  const rollbackBtn = document.getElementById('ota-rollback-btn');
  try {
    const r = await authFetch('/api/releases');
    if (!r.ok) return;
    const releases = await r.json();
    select.innerHTML = '<option value="">-- Rollback vers une version --</option>';
    for (const rel of releases) {
      const opt = document.createElement('option');
      opt.value = rel.version;
      const readyTag = rel.build_ready ? ' [pret]' : '';
      opt.textContent = `v${rel.version} (${new Date(rel.published_at).toLocaleDateString('fr-FR')})${readyTag}`;
      select.appendChild(opt);
    }
    select.onchange = () => { rollbackBtn.disabled = !select.value; };
  } catch (_) {}
}

async function flashSelectedVersion() {
  const select = document.getElementById('ota-release-select');
  const version = select.value;
  if (!version) return;

  const statusEl = document.getElementById('ota-status');
  const barEl = document.getElementById('ota-bar');
  const rollbackBtn = document.getElementById('ota-rollback-btn');
  const ghBtn = document.getElementById('ota-github-btn');

  statusEl.textContent = `Flash v${version}...`;
  barEl.style.width = '0';
  barEl.style.background = 'var(--accent)';
  rollbackBtn.disabled = true;
  if (ghBtn) ghBtn.disabled = true;

  try {
    const r = await authFetch('/api/ota/flash', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ version })
    });
    const data = await r.json();
    if (!r.ok) {
      statusEl.textContent = 'Erreur: ' + (data.error || r.status);
      barEl.style.background = 'var(--red)';
      rollbackBtn.disabled = false;
      if (ghBtn) ghBtn.disabled = false;
    }
  } catch (err) {
    statusEl.textContent = 'Erreur: ' + err.message;
    barEl.style.background = 'var(--red)';
    rollbackBtn.disabled = false;
    if (ghBtn) ghBtn.disabled = false;
  }
}

function handleBuildEvent(msg) {
  const statusEl = document.getElementById('ota-status');
  const barEl = document.getElementById('ota-bar');
  const ghBtn = document.getElementById('ota-github-btn');
  const sha256El = document.getElementById('ota-sha256');
  const rollbackBtn = document.getElementById('ota-rollback-btn');

  statusEl.textContent = msg.msg || '';

  if (msg.sha256) {
    sha256El.textContent = 'SHA256: ' + msg.sha256;
    sha256El.style.display = 'block';
  }

  if (msg.step === 'compile' && msg.percent != null) {
    barEl.style.background = 'var(--accent)';
    barEl.style.width = msg.percent + '%';
  } else if (msg.step === 'flash') {
    barEl.style.background = 'var(--green)';
    barEl.style.width = '0';
  } else if (msg.step === 'success') {
    barEl.style.background = 'var(--green)';
    barEl.style.width = '100%';
    if (ghBtn) ghBtn.disabled = false;
    if (rollbackBtn) rollbackBtn.disabled = false;
    setTimeout(fetchStatus, 8000);
  } else if (msg.step === 'error') {
    barEl.style.background = 'var(--red)';
    statusEl.textContent = 'Erreur: ' + msg.msg;
    if (ghBtn) ghBtn.disabled = false;
    if (rollbackBtn) rollbackBtn.disabled = false;
  }
}

async function flashFromGitHub() {
  if (!latestRelease) return;

  const statusEl = document.getElementById('ota-status');
  const barEl = document.getElementById('ota-bar');
  const ghBtn = document.getElementById('ota-github-btn');

  statusEl.textContent = latestRelease.build_ready ? 'Flash en cours...' : 'Compilation + flash...';
  barEl.style.width = '0';
  barEl.style.background = 'var(--accent)';
  ghBtn.disabled = true;

  try {
    const r = await authFetch('/api/ota/flash', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ version: latestRelease.version })
    });
    const data = await r.json();
    if (!r.ok) {
      statusEl.textContent = 'Erreur: ' + (data.error || r.status);
      barEl.style.background = 'var(--red)';
      ghBtn.disabled = false;
    }
  } catch (err) {
    statusEl.textContent = 'Erreur: ' + err.message;
    barEl.style.background = 'var(--red)';
    ghBtn.disabled = false;
  }
}

// ── Gestion unifiee touch/click (mobile + desktop) ──────────────────────────
// Sur mobile, touchstart+preventDefault bloque le click synthetique.
// On utilise un wrapper qui detecte le support touch et agit en consequence.
function onTap(el, callback, label) {
  if (!el) return;
  let touched = false;
  el.addEventListener('touchstart', (e) => {
    e.preventDefault();
    touched = true;
    console.log('[tap:touchstart]', label || el.id || el.className);
  }, { passive: false });
  el.addEventListener('touchend', (e) => {
    e.preventDefault();
    if (touched) {
      touched = false;
      console.log('[tap:touchend → action]', label || el.id || el.className);
      callback();
    }
  }, { passive: false });
  // Fallback click pour desktop (souris, pas de touch)
  el.addEventListener('click', (e) => {
    if (touched) { touched = false; return; } // deja gere par touch
    console.log('[tap:click → action]', label || el.id || el.className);
    callback();
  });
  el.addEventListener('contextmenu', (e) => e.preventDefault());
}

// PTT buttons
PTT_IDS.forEach(id => {
  const btn = document.getElementById(id);
  onTap(btn, () => {
    if (audioCtx && audioCtx.state === 'suspended') audioCtx.resume();
    toggleRecording();
  }, 'PTT-' + id);
});

// Bottom sheet pills
document.querySelectorAll('.sheet-pill').forEach(pill => {
  onTap(pill, () => toggleSheet(pill.dataset.sheet), 'pill-' + pill.dataset.sheet);
});

// Bottom sheet overlay (fermeture)
onTap(document.getElementById('sheet-overlay'), closeSheet, 'sheet-overlay');

// Play button
onTap(document.getElementById('play-btn'), togglePlay, 'play-btn');
onTap(document.getElementById('mobile-go-btn'), togglePlay, 'mobile-go-btn');

// ── Bottom sheet drag ────────────────────────────────────────────────────────
(function() {
  let dragSheet = null, startY = 0, startPx = 0, sheetH = 0, lastY = 0, lastTime = 0;

  document.querySelectorAll('.bottom-sheet .sheet-drag-zone').forEach(dragZone => {
    dragZone.addEventListener('touchstart', function(e) {
      dragSheet = dragZone.closest('.bottom-sheet');
      if (!dragSheet) return;
      sheetH = dragSheet.offsetHeight;
      startY = lastY = e.touches[0].clientY;
      lastTime = Date.now();
      const m = new DOMMatrix(getComputedStyle(dragSheet).transform);
      startPx = m.m42;
      dragSheet.style.transition = 'none';
      e.preventDefault();
    }, { passive: false });
  });

  document.addEventListener('touchmove', function(e) {
    if (!dragSheet) return;
    const y = e.touches[0].clientY;
    lastY = y;
    lastTime = Date.now();
    let newY = Math.max(0, Math.min(sheetH, startPx + (y - startY)));
    dragSheet.style.transform = 'translate3d(0,' + newY + 'px,0)';
    e.preventDefault();
  }, { passive: false });

  document.addEventListener('touchend', function() {
    if (!dragSheet) return;
    const sheet = dragSheet;
    dragSheet = null;
    const m = new DOMMatrix(getComputedStyle(sheet).transform);
    const finalY = m.m42;
    // Velocite : si swipe rapide vers le bas → fermer
    const velocity = (lastY - startY) / Math.max(1, Date.now() - lastTime + 100);
    const full = 0, half = sheetH * 0.5, closed = sheetH;

    sheet.style.transition = 'transform 0.3s cubic-bezier(0.32, 0.72, 0, 1)';

    if (velocity > 0.5 || finalY > sheetH * 0.75) {
      // Swipe rapide vers le bas ou position basse → fermer
      sheet.style.transform = 'translate3d(0,' + closed + 'px,0)';
      setTimeout(() => { sheet.style.removeProperty('transform'); sheet.style.removeProperty('transition'); closeSheet(); }, 300);
    } else if (velocity < -0.5 || finalY < sheetH * 0.2) {
      // Swipe rapide vers le haut ou position haute → full
      sheet.style.transform = 'translate3d(0,0,0)';
      setTimeout(() => sheet.style.transition = '', 300);
    } else {
      // Snap à la position la plus proche
      const dFull = Math.abs(finalY - full);
      const dHalf = Math.abs(finalY - half);
      const dClose = Math.abs(finalY - closed);
      const min = Math.min(dFull, dHalf, dClose);
      const snapY = (min === dClose) ? closed : (min === dFull) ? full : half;
      if (snapY === closed) {
        sheet.style.transform = 'translate3d(0,' + closed + 'px,0)';
        setTimeout(() => { sheet.style.removeProperty('transform'); sheet.style.removeProperty('transition'); closeSheet(); }, 300);
      } else {
        sheet.style.transform = 'translate3d(0,' + snapY + 'px,0)';
        setTimeout(() => sheet.style.transition = '', 300);
      }
    }
  });
})();

console.log('[init] Touch handlers + sheet drag, v=' + Date.now());

// ── Mode simulation ──────────────────────────────────────────────────────────
function toggleSim() {
  simMode = !simMode;
  const item = document.getElementById('sim-item');
  if (item) item.classList.toggle('active', simMode);
  if (simMode) {
    if (sseSource) { sseSource.close(); sseSource = null; }
    setDot('dot-sse','warn'); setDot('dot-ws','warn'); setDot('dot-ai','warn');
    document.getElementById('mode-badge').textContent = 'SIM';
    addLog('system', 'Mode simulation active');
    startSimTelemetry();
  } else {
    stopSimTelemetry();
    addLog('system', 'Mode simulation desactive');
    connectSSE();
  }
}

let simLat = 46.8219, simLon = -71.2372, simAngle = 0;

function startSimTelemetry() {
  stopSimTelemetry();
  simInterval = setInterval(() => {
    const t = Date.now() / 1000;
    simAngle += 0.01; simLat += Math.cos(simAngle) * 0.0001; simLon += Math.sin(simAngle) * 0.0001;
    applyTelemetry({
      speed:   15 + 10 * Math.sin(t * 0.3),
      voltage: 42 + 5  * Math.sin(t * 0.1),
      current: 8  + 6  * Math.abs(Math.sin(t * 0.4)),
      temp:    45 + 10 * Math.sin(t * 0.05),
      lat: simLat, lon: simLon
    });
  }, 500);
}

function stopSimTelemetry() {
  if (simInterval) { clearInterval(simInterval); simInterval = null; }
}

const SIM_RESPONSES = [
  { transcript:'Avance a vitesse moderee', ai:'Bien recu, j\'accelere doucement.', cmd:{action:'forward',intensity:0.4} },
  { transcript:'Freine maintenant',        ai:'Freinage en cours.',                cmd:{action:'brake',  intensity:0.8} },
  { transcript:'Stop',                     ai:'Arret complet.',                    cmd:{action:'stop'} }
];
let simRespIdx = 0;

function simulateVoiceCmd() {
  const r = SIM_RESPONSES[simRespIdx++ % SIM_RESPONSES.length];
  setVoiceState(true);
  setTimeout(() => {
    addLog('transcript', r.transcript);
    setTimeout(() => {
      appendAiDelta(r.ai);
      addLog('cmd', `[SIM] ${r.cmd.action.toUpperCase()}${r.cmd.intensity != null ? ' @ '+Math.round(r.cmd.intensity*100)+'%' : ''}`);
      setVoiceState(false);
    }, 800);
  }, 1200);
}

// ── Init ─────────────────────────────────────────────────────────────────────
// ── Users modal ──────────────────────────────────────────────────────────────
let currentUserRole = null;

async function checkCurrentUserRole() {
  try {
    const r = await authFetch('/api/auth/me');
    if (r.ok) {
      const data = await r.json();
      currentUserRole = data.role;
      if (currentUserRole === 'admin') {
        document.getElementById('users-item').style.display = '';
      }
    }
  } catch (_) {}
}

function toggleUsersModal() {
  const modal = document.getElementById('users-modal');
  modal.classList.toggle('open');
  if (modal.classList.contains('open')) loadUsers();
  document.getElementById('settings-dropdown').classList.remove('open');
}

async function loadUsers() {
  const list = document.getElementById('users-list');
  list.innerHTML = '<div style="color:var(--muted);text-align:center;padding:20px">Chargement...</div>';
  try {
    const r = await authFetch('/api/users');
    if (!r.ok) { list.innerHTML = '<div style="color:var(--red);padding:12px">Acces refuse</div>'; return; }
    const users = await r.json();
    list.innerHTML = '';
    users.forEach(u => {
      const row = document.createElement('div');
      row.style.cssText = 'display:flex;align-items:center;gap:10px;padding:10px 12px;background:var(--surface);border:1px solid var(--border);border-radius:10px;';
      const roleColor = u.role === 'admin' ? 'var(--red)' : u.role === 'manager' ? 'var(--yellow)' : 'var(--accent)';
      const disabledBadge = u.disabled ? '<span style="font-size:10px;color:var(--red);font-weight:700;margin-left:4px">DESACTIVE</span>' : '';
      row.innerHTML = `
        <div style="flex:1;min-width:0">
          <div style="font-weight:700;font-size:13px;color:var(--text);overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${u.display_name || u.email}${disabledBadge}</div>
          <div style="font-size:11px;color:var(--muted)">${u.email}</div>
        </div>
        <select onchange="changeRole(${u.id},this.value)" style="background:var(--surface-raised);border:1px solid var(--border);color:${roleColor};border-radius:6px;padding:4px 8px;font-size:11px;font-weight:700;cursor:pointer">
          <option value="user" ${u.role==='user'?'selected':''}>Utilisateur</option>
          <option value="manager" ${u.role==='manager'?'selected':''}>Manager</option>
          <option value="admin" ${u.role==='admin'?'selected':''}>Admin</option>
        </select>
        <button onclick="toggleDisableUser(${u.id},${u.disabled?0:1})" title="${u.disabled?'Reactiver':'Desactiver'}"
          style="background:none;border:1px solid ${u.disabled?'var(--green)':'var(--red)'};color:${u.disabled?'var(--green)':'var(--red)'};border-radius:6px;padding:4px 8px;font-size:11px;font-weight:700;cursor:pointer">
          ${u.disabled ? 'Activer' : 'Desactiver'}
        </button>
      `;
      list.appendChild(row);
    });
  } catch (e) { list.innerHTML = '<div style="color:var(--red);padding:12px">Erreur: ' + e.message + '</div>'; }
}

async function changeRole(userId, newRole) {
  try {
    const r = await authFetch(`/api/users/${userId}/role`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ role: newRole })
    });
    if (!r.ok) { const d = await r.json(); alert(d.error || 'Erreur'); }
    loadUsers();
  } catch (e) { alert('Erreur: ' + e.message); }
}

async function toggleDisableUser(userId, disable) {
  try {
    const r = await authFetch(`/api/users/${userId}/disable`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ disabled: !!disable })
    });
    if (!r.ok) { const d = await r.json(); alert(d.error || 'Erreur'); }
    loadUsers();
  } catch (e) { alert('Erreur: ' + e.message); }
}

// ── Onglets panneau droit (desktop) ───────────────────────────────────────────
function switchRightTab(btn) {
  const panelId = btn.dataset.panel;
  document.querySelectorAll('.right-tab').forEach(t => t.classList.remove('active'));
  document.querySelectorAll('.right-panel').forEach(p => p.classList.remove('active'));
  btn.classList.add('active');
  const panel = document.getElementById(panelId);
  if (panel) panel.classList.add('active');
  // Charger l'historique automatiquement a la premiere ouverture
  if (panelId === 'panel-history' && !historyLoaded) loadTelemetryHistory();
}

// ── Historique telemetrie ────────────────────────────────────────────────────
let historyPeriod = '1h';
let historyLoaded = false;
let historyTrailVisible = true;
let historyTrailMobile = null;
let historyTrailDesktop = null;

function setHistoryFilter(btn) {
  document.querySelectorAll('.history-filter-btn').forEach(b => b.classList.remove('active'));
  // Activer tous les boutons avec la meme periode (desktop + mobile)
  document.querySelectorAll(`.history-filter-btn[data-period="${btn.dataset.period}"]`).forEach(b => b.classList.add('active'));
  historyPeriod = btn.dataset.period;
  loadTelemetryHistory();
}

function getHistorySince() {
  const now = new Date();
  switch (historyPeriod) {
    case '1h':  return new Date(now - 3600000).toISOString();
    case '6h':  return new Date(now - 6 * 3600000).toISOString();
    case '24h': return new Date(now - 24 * 3600000).toISOString();
    case '7d':  return new Date(now - 7 * 24 * 3600000).toISOString();
    default:    return new Date(now - 3600000).toISOString();
  }
}

async function loadTelemetryHistory() {
  if (!selectedScooterIdLocal) {
    setHistoryStatus('Aucune trottinette selectionnee');
    return;
  }
  const refreshBtn = document.getElementById('history-refresh-btn');
  if (refreshBtn) refreshBtn.disabled = true;
  setHistoryStatus('Chargement...');

  try {
    const since = getHistorySince();
    const limit = historyPeriod === '7d' ? 500 : 200;
    const url = `/api/telemetry/${encodeURIComponent(selectedScooterIdLocal)}?limit=${limit}&since=${encodeURIComponent(since)}`;
    const r = await authFetch(url);
    if (!r.ok) throw new Error('Erreur ' + r.status);
    const data = await r.json();
    historyLoaded = true;
    renderHistoryTable(data);
    renderHistoryTrail(data);
  } catch (err) {
    setHistoryStatus('Erreur: ' + err.message);
  }
  if (refreshBtn) refreshBtn.disabled = false;
}

function setHistoryStatus(msg) {
  ['history-status', 'history-status-m'].forEach(id => {
    const el = document.getElementById(id);
    if (el) el.textContent = msg;
  });
}

function renderHistoryTable(entries) {
  const html = buildHistoryTableHtml(entries);
  // Desktop
  const wrap = document.getElementById('history-table-wrap');
  if (wrap) wrap.innerHTML = entries.length ? html : '<div class="history-status">Aucune donnee pour cette periode</div>';
  // Mobile
  const wrapM = document.getElementById('history-table-wrap-m');
  if (wrapM) wrapM.innerHTML = entries.length ? html : '<div class="history-status">Aucune donnee pour cette periode</div>';
}

function buildHistoryTableHtml(entries) {
  if (!entries.length) return '';
  let html = `<table class="history-table"><thead><tr>
    <th>Heure</th><th>Vitesse</th><th>Tension</th><th>Temp</th><th>Position</th><th>GPS</th>
  </tr></thead><tbody>`;
  for (const e of entries) {
    // SQLite stocke en UTC sans 'Z' — l'ajouter pour que JS parse correctement
    const t = new Date(e.created_at + (e.created_at.endsWith('Z') ? '' : 'Z'));
    const time = `${pad(t.getHours())}:${pad(t.getMinutes())}:${pad(t.getSeconds())}`;
    const date = `${pad(t.getDate())}/${pad(t.getMonth()+1)}`;
    const timestamp = historyPeriod === '7d' ? `${date} ${time}` : time;
    const speed = e.speed != null ? e.speed.toFixed(1) + ' km/h' : '--';
    const voltage = e.voltage != null ? e.voltage.toFixed(1) + ' V' : '--';
    const temp = e.temp != null ? Math.round(e.temp) + ' °C' : '--';
    const pos = (e.lat && e.lon) ? `${e.lat.toFixed(4)}, ${e.lon.toFixed(4)}` : '--';
    const gpsCls = e.gps_fix === 'ok' ? 'gps-ok' : 'gps-no';
    const gpsText = e.gps_fix === 'ok' ? 'GPS' : (e.gps_fix || 'Cell');
    html += `<tr>
      <td>${timestamp}</td><td>${speed}</td><td>${voltage}</td>
      <td>${temp}</td><td>${pos}</td><td class="${gpsCls}">${gpsText}</td>
    </tr>`;
  }
  html += '</tbody></table>';
  return html;
}

function renderHistoryTrail(entries) {
  // Supprimer les anciens traces
  clearHistoryTrail();
  // Filtrer les entrees avec coordonnees GPS valides
  const points = entries
    .filter(e => e.lat && e.lon && e.lat !== 0 && e.lon !== 0)
    .map(e => [e.lat, e.lon]);
  if (points.length < 2 || !historyTrailVisible) return;

  const trailOpts = { color: '#42f7fb', weight: 3, opacity: 0.7, dashArray: '6, 8' };
  if (map) historyTrailMobile = L.polyline(points, trailOpts).addTo(map);
  if (mapDesktop) historyTrailDesktop = L.polyline(points, trailOpts).addTo(mapDesktop);
}

function clearHistoryTrail() {
  if (historyTrailMobile && map) { map.removeLayer(historyTrailMobile); historyTrailMobile = null; }
  if (historyTrailDesktop && mapDesktop) { mapDesktop.removeLayer(historyTrailDesktop); historyTrailDesktop = null; }
}

function toggleHistoryTrail(visible) {
  historyTrailVisible = visible;
  // Synchroniser les checkboxes desktop et mobile
  document.querySelectorAll('#trail-toggle, .sheet-body input[type="checkbox"]').forEach(cb => cb.checked = visible);
  if (!visible) clearHistoryTrail();
  else if (historyLoaded) loadTelemetryHistory();
}

function initApp() {
  initMap();
  fetchStatus();
  connectSSE();
  setInterval(fetchStatus, 5000);
  checkCurrentUserRole();
  addLog('system', 'Interface v2 initialisee (touch fix)');
  // Invalidate map after a short delay for proper sizing
  setTimeout(() => {
    if (map) map.invalidateSize();
    if (mapDesktop) mapDesktop.invalidateSize();
  }, 200);
}
