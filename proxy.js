'use strict';

require('dotenv').config();

const express  = require('express');
const http     = require('http');
const WebSocket = require('ws');
const path     = require('path');
const fs       = require('fs');
const { execSync, spawn } = require('child_process');
const crypto   = require('crypto');
const os       = require('os');
const bcrypt   = require('bcryptjs');
const jwt      = require('jsonwebtoken');

// ─── Modules ajoutés ──────────────────────────────────────────────────────────
const { initDB, registerScooter: dbRegisterScooter, updateScooterStatus, createRide: dbCreateRide, endRide: dbEndRide, addAuditLog, logTelemetry, getTelemetryHistory, getLastPosition, purgeTelemetry } = require('./database');
const authRoutes = require('./auth-routes');
const { requireAuth: authRequireAuth, requireRole } = require('./auth-routes');
const fleetRouter = require('./fleet');
const { registerOnConnect: fleetRegisterOnConnect } = require('./fleet');

// Initialiser la base de données SQLite
initDB();

// ─── Timestamps dans les logs ────────────────────────────────────────────────
for (const method of ['log', 'warn', 'error']) {
  const orig = console[method].bind(console);
  console[method] = (...args) => orig(`[${new Date().toISOString()}]`, ...args);
}

// ─── Auth ─────────────────────────────────────────────────────────────────────
const AUTH_USERNAME = process.env.AUTH_USERNAME || 'admin';
const AUTH_PASSWORD = process.env.AUTH_PASSWORD || '';
let   AUTH_PASSWORD_HASH = process.env.AUTH_PASSWORD_HASH || '';
const JWT_SECRET  = process.env.JWT_SECRET || '';
const JWT_EXPIRY  = '12h';

// Si le hash est absent ou corrompu mais le mot de passe en clair est dispo, générer le hash
if (AUTH_PASSWORD && (!AUTH_PASSWORD_HASH || !AUTH_PASSWORD_HASH.startsWith('$2'))) {
  AUTH_PASSWORD_HASH = bcrypt.hashSync(AUTH_PASSWORD, 12);
  console.log('[auth] hash bcrypt généré à partir de AUTH_PASSWORD');
}
if (!AUTH_PASSWORD_HASH) console.error('[auth] ATTENTION : AUTH_PASSWORD ni AUTH_PASSWORD_HASH définis dans .env');
if (!JWT_SECRET)         console.error('[auth] ATTENTION : JWT_SECRET non défini dans .env');

// Tentatives de login par IP (rate limiting)
const loginAttempts = new Map(); // ip → { count, lockedUntil }
const MAX_ATTEMPTS  = 5;
const LOCKOUT_MS    = 15 * 60 * 1000; // 15 minutes

function checkRateLimit(ip) {
  const now = Date.now();
  const rec = loginAttempts.get(ip) || { count: 0, lockedUntil: 0 };
  if (rec.lockedUntil > now) {
    const remaining = Math.ceil((rec.lockedUntil - now) / 60000);
    return { blocked: true, remaining };
  }
  return { blocked: false, rec };
}

function recordFailedAttempt(ip) {
  const rec = loginAttempts.get(ip) || { count: 0, lockedUntil: 0 };
  rec.count++;
  if (rec.count >= MAX_ATTEMPTS) {
    rec.lockedUntil = Date.now() + LOCKOUT_MS;
    rec.count = 0;
  }
  loginAttempts.set(ip, rec);
}

function clearAttempts(ip) { loginAttempts.delete(ip); }

// Middleware JWT — accepte le token en header ou en query param (pour EventSource)
// Décode le payload dans req.user pour accéder au rôle et à l'identité
function requireAuth(req, res, next) {
  const header = req.headers.authorization;
  const token  = (header?.startsWith('Bearer ') ? header.slice(7) : null)
               ?? req.query.token;
  if (!token) return res.status(401).json({ error: 'Non autorisé' });
  try {
    const decoded = jwt.verify(token, JWT_SECRET);
    req.user = decoded;
    next();
  } catch {
    res.status(401).json({ error: 'Token invalide ou expiré' });
  }
}

// ─── Config ──────────────────────────────────────────────────────────────────
const USE_LOCAL_AI   = process.env.USE_LOCAL_AI === 'true';
const OPENAI_API_KEY = process.env.OPENAI_API_KEY || '';
const WHISPER_URL    = process.env.WHISPER_URL    || 'http://localhost:10300';
const OLLAMA_URL     = process.env.OLLAMA_URL     || 'http://localhost:11434';
const OLLAMA_MODEL   = process.env.OLLAMA_MODEL   || 'qwen3:8b';
const PIPER_URL      = process.env.PIPER_URL      || 'http://localhost:5000';
const PORT           = parseInt(process.env.PROXY_PORT || '3000', 10);

// ─── État global ─────────────────────────────────────────────────────────────
let sseClients        = [];        // res objects des clients SSE (navigateur)
let openaiWs          = null;      // WebSocket vers OpenAI Realtime
let openaiConnected   = false;

// ─── Multi-trottinettes ─────────────────────────────────────────────────────
// Map<scooterId, { ws, telemetry, fwVersion, debugMode, connectedAt, name, locked }>
const scooters = new Map();
let selectedScooterId = null;

// ─── Courses (ride sessions) ────────────────────────────────────────────────
// Map<rideId, { scooterId, startedAt, endedAt, startTelemetry, endTelemetry, distanceKm, durationSec }>
const rides = new Map();
let rideCounter = 0;

function generateRideId() {
  rideCounter++;
  return `ride-${Date.now()}-${rideCounter}`;
}

function getActiveRide(scooterId) {
  for (const [id, ride] of rides) {
    if (ride.scooterId === scooterId && !ride.endedAt) return { id, ...ride };
  }
  return null;
}

function getSelectedScooter() {
  return scooters.get(selectedScooterId) || null;
}

function getScooterList() {
  return Array.from(scooters.entries()).map(([id, s]) => {
    const activeRide = getActiveRide(id);
    return {
      id,
      name: s.name || id.split(':').slice(-2).join(':'),
      fwVersion: s.fwVersion,
      connected: s.ws.readyState === WebSocket.OPEN,
      telemetry: s.telemetry,
      selected: id === selectedScooterId,
      connectedAt: s.connectedAt,
      locked: s.locked !== false,  // verrouillée par défaut
      activeRide: activeRide ? { id: activeRide.id, startedAt: activeRide.startedAt } : null
    };
  });
}

function getSelectedTelemetry() {
  const selected = getSelectedScooter();
  return selected ? selected.telemetry : { speed: 0, voltage: 0, current: 0, temp: 0, lat: 0, lon: 0, rssi: -999, conn: 'Aucun' };
}

// ─── Broadcast vers navigateurs (SSE) + trottinette sélectionnée (WS) ──────

// ── Audio vers ESP : binaire PCM8 8kHz, accumulé par lots ───────────────────
// Le modem LTE met ~1.5s par message WebSocket. On accumule ~1.5s de son
// pour que chaque message contienne assez d'audio pour couvrir le gap.
const AUDIO_FLUSH_MS = 1500;
let audioAccum = [];
let audioAccumBytes = 0;
let audioFlushTimer = null;

function flushAudioToEsp() {
  audioFlushTimer = null;
  if (audioAccumBytes === 0) return;
  const selected = getSelectedScooter();
  if (selected && selected.ws.readyState === WebSocket.OPEN) {
    const merged = Buffer.concat(audioAccum, audioAccumBytes);
    try { selected.ws.send(merged); } catch (_) {}
  }
  audioAccum = [];
  audioAccumBytes = 0;
}

function sendAudioToEsp(pcm8buf) {
  audioAccum.push(pcm8buf);
  audioAccumBytes += pcm8buf.length;
  // Flush si assez accumulé (~12000 bytes = 1.5s à 8kHz) ou par timer
  if (audioAccumBytes >= 12000) {
    if (audioFlushTimer) { clearTimeout(audioFlushTimer); audioFlushTimer = null; }
    flushAudioToEsp();
  } else if (!audioFlushTimer) {
    audioFlushTimer = setTimeout(flushAudioToEsp, AUDIO_FLUSH_MS);
  }
}

function broadcastSSE(obj) {
  const payload = `data: ${JSON.stringify(obj)}\n\n`;
  for (const client of sseClients) {
    try { client.write(payload); } catch (_) { /* client déconnecté */ }
  }
  // Envoyer à la trottinette SEULEMENT les messages qu'elle traite
  // (audio est géré séparément via sendAudioToEsp avec resampling)
  const espTypes = new Set(['cmd', 'lock', 'music', 'debug', 'ota_begin', 'ota_end']);
  if (espTypes.has(obj.type)) {
    const selected = getSelectedScooter();
    if (selected && selected.ws.readyState === WebSocket.OPEN) {
      try { selected.ws.send(JSON.stringify(obj)); } catch (_) {}
    }
  }
}

// ─── Outils IA (schémas partagés cloud/local) ─────────────────────────────────
const TOOL_SCHEMA = {
  type: 'function',
  name: 'commande_trottinette',
  description:
    'Envoie une commande de contrôle à la trottinette électrique. ' +
    'Utilise cet outil pour toute demande de mouvement, de vitesse ou de direction.',
  parameters: {
    type: 'object',
    properties: {
      action: {
        type: 'string',
        enum: ['avancer', 'freiner', 'arreter', 'vitesse_lente', 'vitesse_moyenne', 'vitesse_haute'],
        description:
          'avancer = accélérer (utilise intensity), ' +
          'freiner = ralentir/freiner (utilise intensity), ' +
          'arreter = arrêt complet immédiat, ' +
          'vitesse_lente = passer en vitesse lente (limite ~33 %), ' +
          'vitesse_moyenne = passer en vitesse moyenne (limite ~66 %), ' +
          'vitesse_haute = passer en vitesse haute (plein régime)'
      },
      intensity: {
        type: 'number',
        minimum: 0,
        maximum: 1,
        description:
          'Intensité 0.0–1.0 — pour avancer et freiner. ' +
          'Exemples : avancer 0.3 = lent, 0.7 = rapide, 1.0 = plein gaz ; ' +
          'freiner 0.5 = freinage normal, 1.0 = freinage d\'urgence.'
      }
    },
    required: ['action']
  }
};

const TELEMETRY_TOOL_SCHEMA = {
  type: 'function',
  name: 'lire_telemetrie',
  description:
    'Retourne les données de télémétrie actuelles de la trottinette : ' +
    'vitesse (km/h), tension batterie (V), courant (A), température (°C), position GPS (lat/lon)',
  parameters: {
    type: 'object',
    properties: {},
    required: []
  }
};

function sendScooterCmd(action, intensity) {
  // Bloquer les commandes moteur si la trottinette est verrouillée
  const selected = getSelectedScooter();
  if (selected && selected.locked !== false) {
    const motorActions = ['avancer', 'freiner', 'vitesse_lente', 'vitesse_moyenne', 'vitesse_haute'];
    if (motorActions.includes(action)) {
      console.log(`[lock] commande ${action} bloquée — trottinette verrouillée`);
      broadcastSSE({ type: 'lock_blocked', action, message: 'Trottinette verrouillée — déverrouillez pour rouler' });
      return;
    }
  }

  const cmd = { type: 'cmd', action };
  if (intensity !== undefined) cmd.intensity = intensity;

  // Diffusion SSE aux clients web
  broadcastSSE({ type: 'cmd', action, intensity: intensity ?? null });

}

// ─── Mode cloud : OpenAI Realtime ─────────────────────────────────────────────
function connectOpenAI() {
  if (!OPENAI_API_KEY) {
    console.warn('[openai] OPENAI_API_KEY manquant – mode cloud désactivé');
    return;
  }

  console.log('[openai] connexion WebSocket Realtime...');
  openaiWs = new WebSocket(
    'wss://api.openai.com/v1/realtime?model=gpt-realtime-1.5',
    {
      headers: {
        Authorization: `Bearer ${OPENAI_API_KEY}`,
        'OpenAI-Beta': 'realtime=v1'
      }
    }
  );

  // Heartbeat pour détecter les connexions mortes
  let openaiPingInterval = null;
  let openaiLastPong = Date.now();

  openaiWs.on('pong', () => { openaiLastPong = Date.now(); });

  openaiWs.on('open', () => {
    openaiConnected = true;
    openaiLastPong = Date.now();
    console.log('[openai] connecté – envoi session.update');

    // Ping toutes les 20s, ferme si pas de pong depuis 45s
    openaiPingInterval = setInterval(() => {
      if (Date.now() - openaiLastPong > 45000) {
        console.warn('[openai] pas de pong depuis 45s – reconnexion');
        clearInterval(openaiPingInterval);
        openaiWs.terminate();
        return;
      }
      if (openaiWs.readyState === WebSocket.OPEN) openaiWs.ping();
    }, 20000);

    openaiWs.send(JSON.stringify({
      type: 'session.update',
      session: {
        modalities: ['text', 'audio'],
        instructions:
          'Contrôle vocal d\'une trottinette. Réponds en français, MAX 1 phrase courte.\n' +
          'Question (batterie, vitesse, tension, temp) → lire_telemetrie\n' +
          'Mouvement (avance, freine, stop) → commande_trottinette\n' +
          'Appelle l\'outil PUIS confirme brièvement.',
        voice: 'alloy',
        input_audio_format: 'pcm16',
        output_audio_format: 'pcm16',
        input_audio_transcription: null,
        turn_detection: {
          type: 'server_vad',
          threshold: 0.5,
          prefix_padding_ms: 300,
          silence_duration_ms: 500,
          create_response: true,
          interrupt_response: false
        },
        tools: [TOOL_SCHEMA, TELEMETRY_TOOL_SCHEMA],
        tool_choice: 'auto'
      }
    }));
  });

  openaiWs.on('message', (raw) => {
    let event;
    try { event = JSON.parse(raw.toString()); } catch (_) { return; }

    switch (event.type) {
      // Transcription de l'audio entrant (journal navigateur uniquement)
      case 'conversation.item.input_audio_transcription.completed': {
        const payload = `data: ${JSON.stringify({ type: 'transcript', text: event.transcript })}\n\n`;
        for (const c of sseClients) { try { c.write(payload); } catch (_) {} }
        break;
      }

      // Delta audio de la réponse (GA et beta)
      // Resample 24kHz → 8kHz (1 sample sur 3) pour réduire le débit LTE ×3
      case 'response.output_audio.delta':
      case 'response.audio.delta': {
        const b64 = event.delta ?? '';
        // Décoder base64 → PCM16 24kHz
        const pcm24 = Buffer.from(b64, 'base64');
        const nSamples = Math.floor(pcm24.length / 2);
        // Downsampler 3:1 (24kHz → 8kHz) + convertir 16→8 bits unsigned
        // Le DAC ESP32 est 8 bits — inutile d'envoyer 16 bits sur LTE
        // Débit : 48 KB/s → 8 KB/s (÷6)
        const nOut = Math.floor(nSamples / 3);
        const pcm8 = Buffer.alloc(nOut);
        for (let i = 0; i < nOut; i++) {
          const s0 = pcm24.readInt16LE(i * 6);
          const s1 = pcm24.readInt16LE(i * 6 + 2);
          const s2 = pcm24.readInt16LE(i * 6 + 4);
          // Moyenne + conversion signed 16→unsigned 8 (>> 8 puis +128)
          const avg = Math.round((s0 + s1 + s2) / 3);
          pcm8[i] = Math.max(0, Math.min(255, (avg >> 8) + 128));
        }
        // Envoyer en base64 au dashboard (24kHz original)
        const CHUNK = 4096;
        for (let i = 0; i < b64.length; i += CHUNK) {
          const ssePayload = `data: ${JSON.stringify({ type: 'audio', data: b64.slice(i, i + CHUNK) })}\n\n`;
          for (const c of sseClients) { try { c.write(ssePayload); } catch (_) {} }
        }
        // Envoyer le PCM8 brut en binaire à l'ESP (accumulé par lots)
        sendAudioToEsp(pcm8);
        break;
      }

      // Delta texte de la transcription de sortie (navigateur uniquement)
      case 'response.output_audio_transcript.delta':
      case 'response.audio_transcript.delta': {
        const payload = `data: ${JSON.stringify({ type: 'ai_response', text: event.delta })}\n\n`;
        for (const c of sseClients) { try { c.write(payload); } catch (_) {} }
        break;
      }
        break;

      // Appel d'outil
      case 'response.function_call_arguments.done': {
        if (event.name === 'commande_trottinette') {
          try {
            const args = JSON.parse(event.arguments);
            sendScooterCmd(args.action, args.intensity);
            console.log(`[openai] commande: ${args.action} intensity=${args.intensity ?? 'n/a'}`);

            openaiWs.send(JSON.stringify({
              type: 'conversation.item.create',
              item: {
                type: 'function_call_output',
                call_id: event.call_id,
                output: JSON.stringify({
                  success: true,
                  action: args.action,
                  intensity: args.intensity ?? null,
                  telemetry: getSelectedTelemetry()
                })
              }
            }));
            openaiWs.send(JSON.stringify({ type: 'response.create' }));
          } catch (err) {
            console.error('[openai] erreur parsing tool call :', err.message);
          }
        } else if (event.name === 'lire_telemetrie') {
          console.log('[openai] lire_telemetrie :', JSON.stringify(getSelectedTelemetry()));
          openaiWs.send(JSON.stringify({
            type: 'conversation.item.create',
            item: {
              type: 'function_call_output',
              call_id: event.call_id,
              output: JSON.stringify(getSelectedTelemetry())
            }
          }));
          openaiWs.send(JSON.stringify({ type: 'response.create' }));
        }
        break;
      }

      case 'error':
        console.error('[openai] erreur :', event.error?.message);
        break;

      default:
        // Log des événements non gérés pour diagnostic
        if (!event.type?.startsWith('rate_limits') &&
            !event.type?.startsWith('session') &&
            !event.type?.startsWith('input_audio_buffer') &&
            !event.type?.startsWith('conversation.item.created') &&
            !event.type?.startsWith('response.created') &&
            !event.type?.startsWith('response.output_item') &&
            !event.type?.startsWith('response.content_part') &&
            !event.type?.startsWith('response.output_text') &&
            !event.type?.startsWith('response.done') &&
            !event.type?.startsWith('response.output_audio.done') &&
            !event.type?.startsWith('response.output_audio_transcript.done')) {
          console.log('[openai] event non géré :', event.type);
        }
    }
  });

  openaiWs.on('close', () => {
    openaiConnected = false;
    if (openaiPingInterval) clearInterval(openaiPingInterval);
    console.log('[openai] déconnecté – reconnexion dans 5 s');
    setTimeout(connectOpenAI, 5000);
  });

  openaiWs.on('error', (err) => {
    console.error('[openai] erreur WS :', err.message);
    openaiWs.close();
  });
}

// ─── Mode local : pipeline Whisper → Ollama → Piper ──────────────────────────
async function processLocalAudio(audioBuffer) {
  // ── Étape 1 : STT avec faster-whisper ──
  let transcript = '';
  try {
    const { default: fetch } = await import('node-fetch');
    const sttRes = await fetch(`${WHISPER_URL}/asr`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: audioBuffer
    });
    if (!sttRes.ok) throw new Error(`STT HTTP ${sttRes.status}`);
    const sttJson = await sttRes.json();
    transcript = sttJson.text || '';
    broadcastSSE({ type: 'transcript', text: transcript });
    console.log('[whisper] transcription :', transcript);
  } catch (err) {
    console.error('[whisper] erreur :', err.message);
    return;
  }

  if (!transcript.trim()) return;

  // ── Étape 2 : LLM avec Ollama ──
  let responseText = '';
  let toolCall = null;
  try {
    const { default: fetch } = await import('node-fetch');
    const llmRes = await fetch(`${OLLAMA_URL}/api/chat`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        model: OLLAMA_MODEL,
        messages: [
          {
            role: 'system',
            content:
              'Tu es le cerveau d\'une trottinette électrique intelligente. ' +
              'Tu reçois des commandes vocales en français. ' +
              'Quand l\'utilisateur te donne une commande de mouvement, ' +
              'tu réponds de façon courte et tu appelles l\'outil approprié.'
          },
          { role: 'user', content: transcript }
        ],
        tools: [TOOL_SCHEMA],
        stream: true
      })
    });

    if (!llmRes.ok) throw new Error(`LLM HTTP ${llmRes.status}`);

    // Lecture du stream NDJSON
    const reader = llmRes.body;
    let buffer = '';
    for await (const chunk of reader) {
      buffer += chunk.toString();
      const lines = buffer.split('\n');
      buffer = lines.pop(); // conserver la ligne incomplète

      for (const line of lines) {
        if (!line.trim()) continue;
        try {
          const part = JSON.parse(line);
          const msg = part.message;
          if (!msg) continue;

          // Texte de réponse
          if (msg.content) {
            responseText += msg.content;
            broadcastSSE({ type: 'ai_response', text: msg.content });
          }

          // Appel d'outil
          if (msg.tool_calls && msg.tool_calls.length > 0) {
            for (const tc of msg.tool_calls) {
              if (tc.function?.name === 'commande_trottinette') {
                toolCall = tc.function.arguments;
              }
            }
          }
        } catch (_) { /* ligne non JSON */ }
      }
    }
  } catch (err) {
    console.error('[ollama] erreur :', err.message);
    return;
  }

  // Exécuter l'outil si demandé
  if (toolCall) {
    try {
      const args = typeof toolCall === 'string' ? JSON.parse(toolCall) : toolCall;
      sendScooterCmd(args.action, args.intensity);
    } catch (err) {
      console.error('[ollama] erreur parsing tool :', err.message);
    }
  }

  // ── Étape 3 : TTS avec Piper ──
  if (!responseText.trim()) return;
  try {
    const { default: fetch } = await import('node-fetch');
    const ttsRes = await fetch(`${PIPER_URL}/api/tts`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ text: responseText, voice: 'fr_FR-siwis-medium' })
    });
    if (!ttsRes.ok) throw new Error(`TTS HTTP ${ttsRes.status}`);
    const audioData = await ttsRes.buffer();
    broadcastSSE({ type: 'audio', data: audioData.toString('base64') });
  } catch (err) {
    console.error('[piper] erreur :', err.message);
  }
}

// ─── Express app ──────────────────────────────────────────────────────────────
const app = express();
app.use(express.json());
app.use(express.raw({ type: 'application/octet-stream', limit: '10mb' }));

// ── Routes publiques ──
app.get('/', (req, res) => {
  res.sendFile(path.join(__dirname, 'landing.html'));
});
app.get('/dashboard', (req, res) => {
  res.sendFile(path.join(__dirname, 'index.html'));
});

// ── POST /login — rétro-compatible, vérifie DB puis AUTH_PASSWORD legacy ──
app.post('/login', async (req, res) => {
  const ip = req.headers['x-forwarded-for']?.split(',')[0].trim() || req.socket.remoteAddress;
  const { username, password } = req.body || {};

  const { blocked, remaining, rec } = checkRateLimit(ip);
  if (blocked) {
    return res.status(429).json({ error: `Trop de tentatives. Réessayer dans ${remaining} min.` });
  }

  // 1) Vérifier dans la base de données (par email = username)
  const { findUserByEmail } = require('./database');
  const dbUser = findUserByEmail((username || '').toLowerCase());
  if (dbUser && !dbUser.disabled) {
    const validPass = await bcrypt.compare(password ?? '', dbUser.password_hash);
    if (validPass) {
      clearAttempts(ip);
      addAuditLog(dbUser.id, 'login_legacy', dbUser.email, { ip });
      const token = jwt.sign(
        { sub: dbUser.id, email: dbUser.email, role: dbUser.role, display_name: dbUser.display_name, username: dbUser.email },
        JWT_SECRET, { expiresIn: JWT_EXPIRY }
      );
      return res.json({ token });
    }
  }

  // 2) Rétro-compatibilité : ancien système AUTH_PASSWORD
  const validUser = username === AUTH_USERNAME;
  const validPass = AUTH_PASSWORD_HASH
    ? await bcrypt.compare(password ?? '', AUTH_PASSWORD_HASH)
    : false;

  if (!validUser || !validPass) {
    recordFailedAttempt(ip);
    const attemptsLeft = MAX_ATTEMPTS - ((rec?.count || 0) + 1);
    return res.status(401).json({
      error: attemptsLeft > 0
        ? `Identifiants incorrects (${attemptsLeft} tentative${attemptsLeft > 1 ? 's' : ''} restante${attemptsLeft > 1 ? 's' : ''})`
        : 'Compte verrouillé 15 minutes'
    });
  }

  clearAttempts(ip);
  const token = jwt.sign(
    { sub: 0, email: AUTH_USERNAME, role: 'admin', display_name: 'Administrateur', username },
    JWT_SECRET, { expiresIn: JWT_EXPIRY }
  );
  res.json({ token });
});

// ── GET /stream (SSE) — route publique (ESP32 n'a pas de JWT) ──
app.get('/stream', (req, res) => {
  res.setHeader('Content-Type', 'text/event-stream');
  res.setHeader('Cache-Control', 'no-cache');
  res.setHeader('Connection', 'keep-alive');
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.flushHeaders();

  // Ping toutes les 15 s pour maintenir la connexion (Cloudflare coupe à ~100 s)
  const ping = setInterval(() => {
    try { res.write(': ping\n\n'); } catch (_) { clearInterval(ping); }
  }, 15000);

  sseClients.push(res);
  console.log(`[sse] client connecté (total: ${sseClients.length})`);

  req.on('close', () => {
    clearInterval(ping);
    sseClients = sseClients.filter(c => c !== res);
    console.log(`[sse] client déconnecté (total: ${sseClients.length})`);
  });
});

// ── POST /audio — gardé pour compatibilité (ESP32 utilise maintenant /ws-esp32)
app.post('/audio', async (req, res) => {
  const chunk = req.body;
  if (!USE_LOCAL_AI && openaiWs && openaiWs.readyState === WebSocket.OPEN) {
    openaiWs.send(JSON.stringify({
      type: 'input_audio_buffer.append',
      audio: chunk.toString('base64')
    }));
  }
  res.sendStatus(200);
});

// ── Monter les routes d'authentification (publiques : register, login) ──
app.use(authRoutes);

// ── Toutes les routes suivantes requièrent un JWT valide ──
app.use(requireAuth);

// ── POST /audio/commit (utilisé en mode local uniquement — semantic_vad gère les tours en mode cloud) ──

app.post('/audio/commit', (req, res) => {
  if (USE_LOCAL_AI && openaiWs && openaiWs.readyState === WebSocket.OPEN) {
    openaiWs.send(JSON.stringify({ type: 'input_audio_buffer.commit' }));
    openaiWs.send(JSON.stringify({ type: 'response.create' }));
  }
  res.sendStatus(200);
});

// ── POST /cmd ──
app.post('/cmd', (req, res) => {
  const { action, intensity } = req.body;
  if (!action) return res.status(400).json({ error: 'action requise' });
  sendScooterCmd(action, intensity);
  res.json({ ok: true });
});

// ── POST /api/music — contrôle musique sur la trottinette sélectionnée ──
app.post('/api/music', (req, res) => {
  const selected = getSelectedScooter();
  if (!selected) return res.status(503).json({ error: 'Aucune trottinette connectée' });
  const { action, track } = req.body;
  if (!action) return res.status(400).json({ error: 'action requise (play|pause|next|prev|stop)' });
  const msg = { type: 'music', action };
  if (track !== undefined) msg.track = track;
  if (selected.ws.readyState === WebSocket.OPEN) {
    selected.ws.send(JSON.stringify(msg));
  }
  res.json({ ok: true });
});

// ── POST /debug — activer/désactiver le mode debug sur la trottinette sélectionnée ──
app.post('/debug', (req, res) => {
  const selected = getSelectedScooter();
  if (!selected) return res.status(503).json({ error: 'Aucune trottinette sélectionnée' });
  selected.debugMode = !!req.body.enabled;
  if (selected.ws.readyState === WebSocket.OPEN) {
    selected.ws.send(JSON.stringify({ type: 'debug', enabled: selected.debugMode }));
  }
  console.log(`[debug] ${selectedScooterId} mode ${selected.debugMode ? 'activé' : 'désactivé'}`);
  res.json({ ok: true, debug: selected.debugMode });
});

// ── GET /status ──
app.get('/status', (req, res) => {
  const selected = getSelectedScooter();
  res.json({
    mode: USE_LOCAL_AI ? 'local' : 'cloud',
    openai_connected: USE_LOCAL_AI ? null : openaiConnected,
    scooter_connected: !!selected && selected.ws.readyState === WebSocket.OPEN,
    scooter_count: scooters.size,
    selected_scooter: selectedScooterId,
    fw_version: selected?.fwVersion ?? null,
    debug: selected?.debugMode ?? false,
    locked: selected?.locked !== false,
    active_ride: selected ? getActiveRide(selectedScooterId) : null
  });
});

// ── GET /api/scooters — liste toutes les trottinettes connectées ──
app.get('/api/scooters', (req, res) => {
  res.json(getScooterList());
});

// ── POST /api/scooters/select — sélectionner une trottinette ──
app.post('/api/scooters/select', (req, res) => {
  const { id } = req.body;
  if (id && !scooters.has(id)) {
    return res.status(404).json({ error: 'Trottinette non trouvée' });
  }
  selectedScooterId = id || null;
  console.log(`[scooter] sélection: ${selectedScooterId || 'aucune'}`);
  const list = getScooterList();
  broadcastSSE({ type: 'scooter_list', scooters: list });
  res.json(list);
});

// ── POST /api/scooters/rename — renommer une trottinette ──
app.post('/api/scooters/rename', (req, res) => {
  const { id, name } = req.body;
  const scooter = scooters.get(id);
  if (!scooter) return res.status(404).json({ error: 'Trottinette non trouvée' });
  scooter.name = name;
  broadcastSSE({ type: 'scooter_list', scooters: getScooterList() });
  res.json({ ok: true });
});

// ── POST /api/scooters/lock — verrouiller/déverrouiller une trottinette ──
app.post('/api/scooters/lock', (req, res) => {
  const { id, locked } = req.body;
  const targetId = id || selectedScooterId;
  const scooter = scooters.get(targetId);
  if (!scooter) return res.status(404).json({ error: 'Trottinette non trouvée' });

  const newLocked = locked !== undefined ? !!locked : !scooter.locked;
  scooter.locked = newLocked;

  // Envoyer la commande lock à l'ESP32
  if (scooter.ws.readyState === WebSocket.OPEN) {
    scooter.ws.send(JSON.stringify({ type: 'lock', locked: newLocked }));
  }

  // Si on verrouille et qu'il y a une course active, la terminer
  if (newLocked) {
    const activeRide = getActiveRide(targetId);
    if (activeRide) {
      endRideInternal(activeRide.id, targetId, scooter);
    }
  }

  console.log(`[lock] ${targetId} ${newLocked ? 'verrouillée' : 'déverrouillée'}`);
  broadcastSSE({ type: 'scooter_list', scooters: getScooterList() });
  broadcastSSE({ type: 'lock_changed', scooterId: targetId, locked: newLocked });
  res.json({ ok: true, locked: newLocked });
});

// ── POST /api/rides/start — démarrer une course ──
app.post('/api/rides/start', (req, res) => {
  const { id } = req.body;
  const targetId = id || selectedScooterId;
  const scooter = scooters.get(targetId);
  if (!scooter) return res.status(404).json({ error: 'Trottinette non trouvée' });
  if (scooter.ws.readyState !== WebSocket.OPEN) {
    return res.status(503).json({ error: 'Trottinette hors ligne' });
  }

  // Vérifier qu'il n'y a pas déjà une course active
  const existing = getActiveRide(targetId);
  if (existing) {
    return res.status(409).json({ error: 'Course déjà en cours', ride: existing });
  }

  // Déverrouiller la trottinette
  scooter.locked = false;
  scooter.ws.send(JSON.stringify({ type: 'lock', locked: false }));

  // Créer la course
  const rideId = generateRideId();
  rides.set(rideId, {
    scooterId: targetId,
    startedAt: new Date().toISOString(),
    endedAt: null,
    startTelemetry: { ...scooter.telemetry },
    endTelemetry: null,
    distanceKm: 0,
    durationSec: 0
  });

  console.log(`[ride] course ${rideId} démarrée sur ${targetId}`);
  broadcastSSE({ type: 'scooter_list', scooters: getScooterList() });
  broadcastSSE({ type: 'ride_started', rideId, scooterId: targetId });
  res.json({ ok: true, rideId, scooterId: targetId });
});

// Fonction interne pour terminer une course
function endRideInternal(rideId, scooterId, scooter) {
  const ride = rides.get(rideId);
  if (!ride || ride.endedAt) return null;

  ride.endedAt = new Date().toISOString();
  ride.endTelemetry = { ...scooter.telemetry };
  ride.durationSec = Math.round((new Date(ride.endedAt) - new Date(ride.startedAt)) / 1000);

  console.log(`[ride] course ${rideId} terminée — ${ride.durationSec}s`);
  broadcastSSE({ type: 'ride_ended', rideId, scooterId, ride: rides.get(rideId) });
  return ride;
}

// ── POST /api/rides/end — terminer une course ──
app.post('/api/rides/end', (req, res) => {
  const { id, rideId } = req.body;
  const targetId = id || selectedScooterId;
  const scooter = scooters.get(targetId);
  if (!scooter) return res.status(404).json({ error: 'Trottinette non trouvée' });

  // Trouver la course active
  const activeRide = rideId ? { id: rideId, ...rides.get(rideId) } : getActiveRide(targetId);
  if (!activeRide) {
    return res.status(404).json({ error: 'Aucune course active' });
  }

  // Terminer la course
  const ride = endRideInternal(activeRide.id, targetId, scooter);

  // Reverrouiller la trottinette
  scooter.locked = true;
  if (scooter.ws.readyState === WebSocket.OPEN) {
    scooter.ws.send(JSON.stringify({ type: 'lock', locked: true }));
  }

  broadcastSSE({ type: 'scooter_list', scooters: getScooterList() });
  broadcastSSE({ type: 'lock_changed', scooterId: targetId, locked: true });
  res.json({ ok: true, ride });
});

// ── GET /api/rides — historique des courses ──
app.get('/api/rides', (req, res) => {
  const scooterId = req.query.scooterId;
  let rideList = Array.from(rides.entries()).map(([id, r]) => ({ id, ...r }));
  if (scooterId) rideList = rideList.filter(r => r.scooterId === scooterId);
  rideList.sort((a, b) => new Date(b.startedAt) - new Date(a.startedAt));
  const limit = Math.min(parseInt(req.query.limit) || 50, 200);
  res.json(rideList.slice(0, limit));
});

// ── GET /api/rides/active — courses en cours ──
app.get('/api/rides/active', (req, res) => {
  const activeRides = Array.from(rides.entries())
    .filter(([, r]) => !r.endedAt)
    .map(([id, r]) => ({ id, ...r }));
  res.json(activeRides);
});

// ── Historique de télémétrie ─────────────────────────────────────────────────
app.get('/api/telemetry/:scooterId', (req, res) => {
  const { scooterId } = req.params;
  const limit = parseInt(req.query.limit) || 100;
  const since = req.query.since || null;  // ISO date string
  try {
    const history = getTelemetryHistory(scooterId, { limit, since });
    res.json(history);
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// ── Monter les routes de flotte (requièrent auth via le routeur interne) ──
// Injecter les dépendances du proxy dans le routeur fleet
fleetRouter._getLiveScooters = () => scooters;
fleetRouter._broadcastSSE    = broadcastSSE;
// flashFirmware sera injecté après sa définition (voir plus bas)
app.use(fleetRouter);

// ─── OTA via proxy : flashFirmware() partagé ──────────────────────────────────
let otaInProgress = false;
let otaResolve    = null;

// Fonction partagée : envoie un Buffer firmware à une trottinette via WebSocket
// targetId optionnel — si absent, utilise la trottinette sélectionnée
function flashFirmware(firmware, targetId = null) {
  const id = targetId || selectedScooterId;
  const scooter = id ? scooters.get(id) : null;

  return new Promise((resolve, reject) => {
    if (!scooter || scooter.ws.readyState !== WebSocket.OPEN) {
      return reject(new Error('Trottinette non connectée'));
    }
    if (otaInProgress) {
      return reject(new Error('OTA déjà en cours'));
    }
    if (!Buffer.isBuffer(firmware) || firmware.length < 1000) {
      return reject(new Error('Firmware invalide (trop petit ou pas un Buffer)'));
    }

    const ws = scooter.ws;
    const md5 = crypto.createHash('md5').update(firmware).digest('hex');
    console.log(`[ota] envoi firmware ${firmware.length} bytes à ${id} — MD5: ${md5}`);
    otaInProgress = true;

    ws.send(JSON.stringify({ type: 'ota_begin', size: firmware.length, md5 }));

    const CHUNK = 1024;
    let offset = 0;

    function sendNextChunk() {
      if (offset >= firmware.length) {
        ws.send(JSON.stringify({ type: 'ota_end' }));
        return;
      }
      const end = Math.min(offset + CHUNK, firmware.length);
      const chunk = firmware.slice(offset, end);
      ws.send(chunk, { binary: true }, () => {
        offset = end;
        const pct = Math.round(offset / firmware.length * 100);
        if (pct % 10 === 0) console.log(`[ota] envoyé ${pct}%`);
        setTimeout(sendNextChunk, 20);
      });
    }

    const otaTimeout = setTimeout(() => {
      otaInProgress = false;
      otaResolve = null;
      reject(new Error('Timeout — pas de réponse de l\'ESP32'));
    }, 120000);

    otaResolve = (result) => {
      clearTimeout(otaTimeout);
      otaInProgress = false;
      otaResolve = null;
      if (result.success) {
        console.log('[ota] succès — ESP32 redémarre');
        resolve({ ok: true, message: 'Firmware flashé, ESP32 redémarre' });
      } else {
        console.error('[ota] échec :', result.error);
        reject(new Error(result.error || 'Échec OTA'));
      }
    };

    sendNextChunk();
  });
}

// Injecter flashFirmware dans le routeur fleet (après sa définition)
fleetRouter._flashFirmware = flashFirmware;

// POST /ota — upload manuel .bin
app.post('/ota', async (req, res) => {
  try {
    const sha256 = crypto.createHash('sha256').update(req.body).digest('hex');
    console.log(`[ota] upload manuel — ${req.body.length} bytes — SHA256: ${sha256}`);
    broadcastSSE({ type: 'ota_build', step: 'flash', msg: `Flash OTA (${(req.body.length / 1024).toFixed(0)} Ko)`, sha256 });
    const result = await flashFirmware(req.body);
    result.sha256 = sha256;
    res.json(result);
  } catch (err) {
    const status = err.message.includes('non connecté') ? 503
                 : err.message.includes('déjà en cours') ? 409
                 : err.message.includes('Timeout') ? 504
                 : err.message.includes('invalide') ? 400 : 500;
    res.status(status).json({ error: err.message });
  }
});

// ─── GitHub Releases + pré-compilation automatique ──────────────────────────
const GITHUB_REPO = 'fpoisson2/controle-trotinnette';
const BUILDS_DIR  = path.join(__dirname, 'builds');
const POLL_INTERVAL_MS = 5 * 60 * 1000; // vérifier toutes les 5 minutes
let buildInProgress = false;

// Charger l'index des builds déjà compilés depuis le disque
// Format : builds/<version>.json  = { version, sha256, size, built_at }
//          builds/<version>.bin   = le firmware
function getLocalBuilds() {
  if (!fs.existsSync(BUILDS_DIR)) fs.mkdirSync(BUILDS_DIR, { recursive: true });
  const files = fs.readdirSync(BUILDS_DIR).filter(f => f.endsWith('.json'));
  return files.map(f => {
    try { return JSON.parse(fs.readFileSync(path.join(BUILDS_DIR, f), 'utf8')); }
    catch (_) { return null; }
  }).filter(Boolean).sort((a, b) => b.version.localeCompare(a.version));
}

function getBuild(version) {
  const metaPath = path.join(BUILDS_DIR, `${version}.json`);
  const binPath  = path.join(BUILDS_DIR, `${version}.bin`);
  if (!fs.existsSync(metaPath) || !fs.existsSync(binPath)) return null;
  const meta = JSON.parse(fs.readFileSync(metaPath, 'utf8'));
  return { ...meta, binPath };
}

// Récupérer les releases depuis GitHub
async function fetchGitHubReleases(count = 10) {
  const url = `https://api.github.com/repos/${GITHUB_REPO}/releases?per_page=${count}`;
  const resp = await fetch(url, {
    headers: { 'Accept': 'application/vnd.github+json', 'User-Agent': 'trottinette-proxy' }
  });
  if (!resp.ok) throw new Error(`GitHub API ${resp.status}: ${resp.statusText}`);
  const releases = await resp.json();
  return releases.map(r => ({
    version: r.tag_name.replace(/^v/, ''),
    tag: r.tag_name,
    published_at: r.published_at,
    tarball_url: r.tarball_url,
    body: r.body || ''
  }));
}

async function fetchLatestRelease() {
  const url = `https://api.github.com/repos/${GITHUB_REPO}/releases/latest`;
  const resp = await fetch(url, {
    headers: { 'Accept': 'application/vnd.github+json', 'User-Agent': 'trottinette-proxy' }
  });
  if (!resp.ok) {
    if (resp.status === 404) return null;
    throw new Error(`GitHub API ${resp.status}: ${resp.statusText}`);
  }
  const release = await resp.json();
  return {
    version: release.tag_name.replace(/^v/, ''),
    tag: release.tag_name,
    published_at: release.published_at,
    tarball_url: release.tarball_url,
    body: release.body || ''
  };
}

// Compiler une release et stocker le .bin sur disque
async function buildAndStoreRelease(release) {
  if (buildInProgress) {
    console.log(`[build] build déjà en cours, skip ${release.version}`);
    return null;
  }
  // Déjà compilé ?
  const existing = getBuild(release.version);
  if (existing) return existing;

  buildInProgress = true;
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'fw-build-'));
  const tarball = path.join(tmpDir, 'source.tar.gz');
  const localConfigH = path.join(__dirname, 'firmware', 'include', 'config.h');

  try {
    // Télécharger le tarball source
    broadcastSSE({ type: 'ota_build', step: 'download', msg: `Téléchargement v${release.version}...` });
    console.log(`[build] téléchargement source ${release.tag}...`);
    const resp = await fetch(release.tarball_url, {
      headers: { 'User-Agent': 'trottinette-proxy' },
      redirect: 'follow'
    });
    if (!resp.ok) throw new Error(`Téléchargement source échoué: HTTP ${resp.status}`);
    const buf = Buffer.from(await resp.arrayBuffer());
    fs.writeFileSync(tarball, buf);

    // Extraire
    broadcastSSE({ type: 'ota_build', step: 'extract', msg: 'Extraction...' });
    execSync(`tar xzf source.tar.gz`, { cwd: tmpDir });

    const extracted = fs.readdirSync(tmpDir).find(f =>
      f !== 'source.tar.gz' && fs.statSync(path.join(tmpDir, f)).isDirectory()
    );
    if (!extracted) throw new Error('Répertoire source introuvable après extraction');
    const srcDir = path.join(tmpDir, extracted, 'firmware');

    if (!fs.existsSync(srcDir)) throw new Error('Répertoire firmware/ introuvable dans le source');

    // Copier le config.h local
    if (!fs.existsSync(localConfigH)) {
      throw new Error('config.h local introuvable — nécessaire pour la compilation');
    }
    fs.copyFileSync(localConfigH, path.join(srcDir, 'include', 'config.h'));

    // Injecter la version dans config.h
    const configPath = path.join(srcDir, 'include', 'config.h');
    let configContent = fs.readFileSync(configPath, 'utf8');
    configContent = configContent.replace(
      /#define\s+FW_VERSION\s+"[^"]*"/,
      `#define FW_VERSION  "${release.version}"`
    );
    fs.writeFileSync(configPath, configContent);

    // Installer les libs d'abord, puis patcher WebSockets avec le header custom
    broadcastSSE({ type: 'ota_build', step: 'compile', msg: `Compilation v${release.version}...` });
    console.log(`[build] compilation v${release.version}...`);

    // Pré-installer les dépendances pour pouvoir patcher avant la compilation
    try {
      execSync('pio pkg install -e esp32dev', { cwd: srcDir, timeout: 120000, stdio: 'pipe' });
    } catch (_) { /* ignore — pio run installera aussi */ }

    // Copier le header custom WebSockets dans les libdeps
    const customHeader = path.join(srcDir, 'include', 'WebSocketsNetworkClientSecure.h');
    if (fs.existsSync(customHeader)) {
      const libdepsWs = path.join(srcDir, '.pio', 'libdeps', 'esp32dev', 'WebSockets', 'src');
      if (fs.existsSync(libdepsWs)) {
        fs.copyFileSync(customHeader, path.join(libdepsWs, 'WebSocketsNetworkClientSecure.h'));
        console.log('[build] header WebSocketsNetworkClientSecure.h copié dans libdeps');
      }
    }

    await new Promise((resolve, reject) => {
      const proc = spawn('pio', ['run', '-e', 'esp32dev'], {
        cwd: srcDir,
        timeout: 300000
      });

      let lastPct = 0;
      proc.stdout.on('data', (data) => {
        const line = data.toString().trim();
        if (!line) return;
        const pctMatch = line.match(/\[(\d+)%\]/);
        if (pctMatch) {
          const pct = parseInt(pctMatch[1]);
          if (pct > lastPct) {
            lastPct = pct;
            broadcastSSE({ type: 'ota_build', step: 'compile', percent: pct, msg: `Compilation v${release.version}: ${pct}%` });
          }
        }
        console.log(`[build] ${line}`);
      });

      proc.stderr.on('data', (data) => {
        const line = data.toString().trim();
        if (line) console.log(`[build] ${line}`);
      });

      proc.on('close', (code) => {
        if (code === 0) resolve();
        else reject(new Error(`PlatformIO exit code ${code}`));
      });

      proc.on('error', reject);
    });

    // Lire le .bin et stocker
    const pioBinPath = path.join(srcDir, '.pio', 'build', 'esp32dev', 'firmware.bin');
    if (!fs.existsSync(pioBinPath)) throw new Error('firmware.bin introuvable après compilation');

    const firmware = fs.readFileSync(pioBinPath);
    const sha256 = crypto.createHash('sha256').update(firmware).digest('hex');

    // Sauvegarder dans builds/
    if (!fs.existsSync(BUILDS_DIR)) fs.mkdirSync(BUILDS_DIR, { recursive: true });
    fs.writeFileSync(path.join(BUILDS_DIR, `${release.version}.bin`), firmware);
    const meta = {
      version: release.version,
      tag: release.tag,
      sha256,
      size: firmware.length,
      built_at: new Date().toISOString(),
      published_at: release.published_at
    };
    fs.writeFileSync(path.join(BUILDS_DIR, `${release.version}.json`), JSON.stringify(meta, null, 2));

    console.log(`[build] v${release.version} compilé — ${firmware.length} bytes — SHA256: ${sha256}`);
    broadcastSSE({ type: 'ota_build', step: 'ready', msg: `v${release.version} prêt (${(firmware.length / 1024).toFixed(0)} Ko)`, sha256, version: release.version });

    return meta;
  } catch (err) {
    console.error(`[build] erreur compilation v${release.version} :`, err.message);
    broadcastSSE({ type: 'ota_build', step: 'error', msg: `Erreur build v${release.version}: ${err.message}` });
    return null;
  } finally {
    buildInProgress = false;
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

// Boucle de polling : vérifie les nouvelles releases et compile automatiquement
const failedBuilds = new Set();
async function pollAndBuild() {
  try {
    const latest = await fetchLatestRelease();
    if (!latest) return;

    if (failedBuilds.has(latest.version)) return;

    const existing = getBuild(latest.version);
    if (!existing) {
      console.log(`[poll] nouvelle release détectée : v${latest.version} — lancement du build`);
      try {
        await buildAndStoreRelease(latest);
      } catch (buildErr) {
        failedBuilds.add(latest.version);
        console.error(`[poll] build v${latest.version} échoué, ne sera pas retenté : ${buildErr.message}`);
      }
    }
  } catch (err) {
    console.error('[poll] erreur :', err.message);
  }
}

// GET /api/releases/latest
app.get('/api/releases/latest', async (req, res) => {
  try {
    const release = await fetchLatestRelease();
    if (!release) return res.status(404).json({ error: 'Aucune release trouvée' });
    // Ajouter l'état du build local
    const build = getBuild(release.version);
    release.build_ready = !!build;
    if (build) { release.sha256 = build.sha256; release.size = build.size; }
    res.json(release);
  } catch (err) {
    console.error('[github] erreur fetch release :', err.message);
    res.status(502).json({ error: 'Impossible de contacter GitHub : ' + err.message });
  }
});

// GET /api/releases — liste les releases avec état du build local
app.get('/api/releases', async (req, res) => {
  const count = Math.min(parseInt(req.query.count) || 10, 30);
  try {
    const releases = await fetchGitHubReleases(count);
    // Enrichir avec l'état du build local
    for (const rel of releases) {
      const build = getBuild(rel.version);
      rel.build_ready = !!build;
      if (build) { rel.sha256 = build.sha256; rel.size = build.size; }
    }
    res.json(releases);
  } catch (err) {
    console.error('[github] erreur fetch releases :', err.message);
    res.status(502).json({ error: 'Impossible de contacter GitHub : ' + err.message });
  }
});

// GET /api/builds — liste uniquement les builds pré-compilés disponibles localement
app.get('/api/builds', (req, res) => {
  res.json(getLocalBuilds());
});

// POST /api/ota/flash — flasher un .bin pré-compilé (ou compiler + flasher si pas encore prêt)
// Body : { version: "2025.03.01" } — si absent, utilise la dernière release
app.post('/api/ota/flash', async (req, res) => {
  if (otaInProgress) return res.status(409).json({ error: 'OTA déjà en cours' });

  const targetVersion = req.body?.version;
  let version;

  try {
    if (targetVersion) {
      version = targetVersion;
    } else {
      const latest = await fetchLatestRelease();
      if (!latest) return res.status(404).json({ error: 'Aucune release trouvée' });
      version = latest.version;
    }
  } catch (err) {
    return res.status(502).json({ error: err.message });
  }

  let build = getBuild(version);

  // Si pas encore compilé, compiler maintenant
  if (!build) {
    if (buildInProgress) return res.status(409).json({ error: 'Build en cours pour une autre version' });

    try {
      // Chercher la release GitHub pour cette version
      const url = `https://api.github.com/repos/${GITHUB_REPO}/releases/tags/v${version}`;
      const resp = await fetch(url, {
        headers: { 'Accept': 'application/vnd.github+json', 'User-Agent': 'trottinette-proxy' }
      });
      if (!resp.ok) return res.status(404).json({ error: `Release v${version} introuvable` });
      const r = await resp.json();
      const release = {
        version: r.tag_name.replace(/^v/, ''),
        tag: r.tag_name,
        published_at: r.published_at,
        tarball_url: r.tarball_url,
        body: r.body || ''
      };

      // Répondre immédiatement — compilation + flash en arrière-plan
      res.json({ ok: true, message: `Build v${version} en cours, flash suivra` });

      (async () => {
        try {
          build = await buildAndStoreRelease(release);
          if (!build) return;
          const firmware = fs.readFileSync(path.join(BUILDS_DIR, `${version}.bin`));
          broadcastSSE({ type: 'ota_build', step: 'flash', msg: 'Flash OTA en cours...', sha256: build.sha256 });
          await flashFirmware(firmware);
          broadcastSSE({ type: 'ota_build', step: 'success', msg: `Firmware v${version} flashé — redémarrage ESP32`, sha256: build.sha256 });
        } catch (err) {
          console.error('[ota] erreur :', err.message);
          broadcastSSE({ type: 'ota_build', step: 'error', msg: err.message });
        }
      })();
      return;
    } catch (err) {
      return res.status(502).json({ error: err.message });
    }
  }

  // Build disponible → flash direct
  res.json({ ok: true, message: `Flash v${version} lancé`, sha256: build.sha256 });

  (async () => {
    try {
      const firmware = fs.readFileSync(path.join(BUILDS_DIR, `${version}.bin`));
      broadcastSSE({ type: 'ota_build', step: 'flash', msg: `Flash v${version} en cours...`, sha256: build.sha256 });
      await flashFirmware(firmware);
      broadcastSSE({ type: 'ota_build', step: 'success', msg: `Firmware v${version} flashé — redémarrage ESP32`, sha256: build.sha256 });
    } catch (err) {
      console.error('[ota] erreur flash :', err.message);
      broadcastSSE({ type: 'ota_build', step: 'error', msg: err.message });
    }
  })();
});

// POST /api/ota/github — compatibilité avec l'ancien endpoint, redirige vers /api/ota/flash
app.post('/api/ota/github', async (req, res) => {
  // Réécrire la requête vers le nouvel endpoint
  req.url = '/api/ota/flash';
  app.handle(req, res);
});

// ─── Debug audio : écouter le micro ESP32 depuis le navigateur ────────────────
let debugAudioClients = [];  // WebSocket clients qui écoutent le micro
const debugAudioWss = new WebSocket.Server({ noServer: true });
debugAudioWss.on('connection', (ws) => {
  debugAudioClients.push(ws);
  console.log(`[debug-audio] client connecté (total: ${debugAudioClients.length})`);
  ws.on('close', () => {
    debugAudioClients = debugAudioClients.filter(c => c !== ws);
    console.log(`[debug-audio] client déconnecté (total: ${debugAudioClients.length})`);
  });
});

// ─── WebSocket ESP32 (/ws-esp32) — multi-trottinettes ───────────────────────
const esp32Wss = new WebSocket.Server({ noServer: true });

esp32Wss.on('connection', (ws) => {
  console.log('[esp32-ws] nouvelle connexion (attente hello)');
  let scooterId = null;

  // Ping WebSocket toutes les 20s pour garder la connexion vivante via Cloudflare
  const espPing = setInterval(() => {
    if (ws.readyState === WebSocket.OPEN) ws.ping();
  }, 20000);
  ws.on('close', () => clearInterval(espPing));
  ws.on('error', () => clearInterval(espPing));

  ws.on('message', (data, isBinary) => {
    if (isBinary) {
      // Audio PCM — uniquement de la trottinette sélectionnée vers OpenAI
      if (scooterId === selectedScooterId) {
        if (openaiWs && openaiWs.readyState === WebSocket.OPEN) {
          openaiWs.send(JSON.stringify({
            type: 'input_audio_buffer.append',
            audio: Buffer.from(data).toString('base64')
          }));
        } else {
          console.warn(`[audio] openaiWs pas prêt (state=${openaiWs?.readyState}, connected=${openaiConnected})`);
        }
        // Relayer le PCM brut aux clients debug-audio (trottinette sélectionnée seulement)
        for (const c of debugAudioClients) {
          if (c.readyState === WebSocket.OPEN) {
            try { c.send(data, { binary: true }); } catch (_) {}
          }
        }
      }
    } else {
      try {
        const msg = JSON.parse(data.toString());

        if (msg.type === 'hello') {
          scooterId = msg.mac || `esp32-${Date.now()}`;
          scooters.set(scooterId, {
            ws,
            telemetry: { speed: 0, voltage: 0, current: 0, temp: 0, lat: 0, lon: 0, rssi: -999, conn: 'Aucun' },
            fwVersion: msg.version || null,
            debugMode: false,
            connectedAt: Date.now(),
            name: null,
            locked: true  // verrouillée par défaut au démarrage
          });
          // Auto-sélection si c'est la première trottinette
          if (!selectedScooterId) selectedScooterId = scooterId;
          console.log(`[esp32-ws] enregistré: ${scooterId} (FW ${msg.version}) — verrouillée`);

          // Restaurer la dernière position connue depuis la BD
          try {
            const lastPos = getLastPosition(scooterId);
            if (lastPos && lastPos.last_lat && lastPos.last_lon) {
              const entry = scooters.get(scooterId);
              entry.telemetry.lat = lastPos.last_lat;
              entry.telemetry.lon = lastPos.last_lon;
              entry.telemetry.gps_fix = lastPos.last_gps_fix || 'db';
              console.log(`[esp32-ws] position restaurée: ${lastPos.last_lat}, ${lastPos.last_lon}`);
            }
          } catch (_) {}

          // Enregistrer dans fleet.json et base SQLite
          try {
            fleetRegisterOnConnect(scooterId, msg.version || null);
            // Récupérer le nom du registre fleet si disponible
            const { getScooterInfo } = require('./fleet');
            const fleetInfo = getScooterInfo(scooterId);
            if (fleetInfo && fleetInfo.name) {
              scooters.get(scooterId).name = fleetInfo.name;
            }
          } catch (err) {
            console.error(`[esp32-ws] erreur enregistrement fleet/DB : ${err.message}`);
          }

          // Envoyer l'état verrouillé à l'ESP32
          ws.send(JSON.stringify({ type: 'lock', locked: true }));
          // Notifier le dashboard
          broadcastSSE({ type: 'scooter_list', scooters: getScooterList() });

        } else if (msg.type === 'audio_in') {
          // Audio PCM base64 depuis l'ESP32 (alternative aux frames binaires pour Cloudflare)
          if (scooterId === selectedScooterId) {
            if (openaiWs && openaiWs.readyState === WebSocket.OPEN) {
              openaiWs.send(JSON.stringify({
                type: 'input_audio_buffer.append',
                audio: msg.data
              }));
            }
            // Relayer le PCM brut aux clients debug-audio
            const pcmBuf = Buffer.from(msg.data, 'base64');
            for (const c of debugAudioClients) {
              if (c.readyState === WebSocket.OPEN) {
                try { c.send(pcmBuf, { binary: true }); } catch (_) {}
              }
            }
          }

        } else if (msg.type === 'telemetry') {
          const entry = scooters.get(scooterId);
          if (entry) {
            const { type, ...fields } = msg;
            entry.telemetry = { ...entry.telemetry, ...fields };
            // Persister en BD (throttle : 1 écriture par 30s par trottinette)
            const now = Date.now();
            if (!entry._lastDbLog || now - entry._lastDbLog > 30000) {
              entry._lastDbLog = now;
              try { logTelemetry(scooterId, entry.telemetry); } catch (_) {}
            }
            // Relayer aux clients SSE avec l'ID de la trottinette
            const payload = `data: ${JSON.stringify({ type: 'telemetry', scooterId, ...fields })}\n\n`;
            for (const c of sseClients) { try { c.write(payload); } catch (_) {} }
          }

        } else if (msg.type === 'log') {
          const payload = `data: ${JSON.stringify({ type: 'esp32_log', scooterId, msg: msg.msg })}\n\n`;
          for (const c of sseClients) { try { c.write(payload); } catch (_) {} }

        } else if (msg.type === 'ota_progress') {
          console.log(`[ota] ${scooterId} : ${msg.percent}%`);
          broadcastSSE({ type: 'ota_progress', scooterId, percent: msg.percent });

        } else if (msg.type === 'ota_result') {
          if (otaResolve) otaResolve(msg);

        } else if (msg.type === 'wifi_scan') {
          // Géolocalisation WiFi — seulement si pas de fix GPS précis
          const entry = scooters.get(scooterId);
          const hasGpsFix = entry && entry.telemetry.gps_fix === 'ok';
          const geoKey = process.env.GOOGLE_GEOLOCATION_KEY;
          console.log(`[wifi-scan] ${scooterId}: ${msg.aps?.length || 0} APs, gpsfix=${hasGpsFix}, key=${geoKey ? 'oui' : 'NON'}`);
          if (!geoKey) console.warn('[wifi-scan] GOOGLE_GEOLOCATION_KEY manquante — ajouter au .env');
          if (geoKey && msg.aps && msg.aps.length > 0 && !hasGpsFix) {
            const body = {
              wifiAccessPoints: msg.aps.map(ap => ({
                macAddress: ap.mac,
                signalStrength: ap.rssi,
                channel: ap.ch
              }))
            };
            fetch(`https://www.googleapis.com/geolocation/v1/geolocate?key=${geoKey}`, {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify(body)
            }).then(r => r.json()).then(data => {
              if (data.error) {
                console.error(`[wifi-geo] API erreur: ${data.error.code} ${data.error.message}`);
              } else if (data.location) {
                console.log(`[wifi-geo] ${scooterId}: ${data.location.lat}, ${data.location.lng} (±${data.accuracy}m)`);
                const entry = scooters.get(scooterId);
                if (entry) {
                  entry.telemetry.lat = data.location.lat;
                  entry.telemetry.lon = data.location.lng;
                  entry.telemetry.gps_fix = `wifi±${Math.round(data.accuracy)}m`;
                  // Persister la position WiFi en BD
                  try { logTelemetry(scooterId, entry.telemetry); } catch (_) {}
                  broadcastSSE({ type: 'telemetry', scooterId,
                    lat: data.location.lat, lon: data.location.lng,
                    gps_fix: entry.telemetry.gps_fix });
                }
              }
            }).catch(err => console.error(`[wifi-geo] erreur: ${err.message}`));
          }

        } else if (msg.type === 'lock_ack') {
          console.log(`[lock] ${scooterId} confirme: ${msg.locked ? 'verrouillée' : 'déverrouillée'}`);
          const entry = scooters.get(scooterId);
          if (entry) entry.locked = msg.locked;

        } else if (msg.type === 'qc_result') {
          // Résultat du test de contrôle qualité
          console.log(`[qc] ${scooterId} résultat QC : ${msg.passed ? 'OK' : 'ÉCHEC'}`);
          const { getFleet } = require('./fleet');
          const fleetData = getFleet();
          if (fleetData[scooterId]) {
            const { renameScooter, setNotes } = require('./fleet');
            // Mettre à jour le statut QC via accès direct au fleet
            fleetData[scooterId].qcStatus = msg.passed ? 'passed' : 'failed';
          }
          broadcastSSE({ type: 'qc_result', scooterId, passed: msg.passed, details: msg.details || null });
        }
      } catch (_) { /* ignore */ }
    }
  });

  ws.on('close', () => {
    if (scooterId) {
      console.log(`[esp32-ws] déconnecté: ${scooterId}`);
      // Terminer toute course active sur cette trottinette
      const activeRide = getActiveRide(scooterId);
      const scooterEntry = scooters.get(scooterId);
      if (activeRide && scooterEntry) {
        endRideInternal(activeRide.id, scooterId, scooterEntry);
      }
      scooters.delete(scooterId);
      // Si c'était la trottinette sélectionnée, en choisir une autre
      if (selectedScooterId === scooterId) {
        selectedScooterId = scooters.size > 0 ? scooters.keys().next().value : null;
      }
      broadcastSSE({ type: 'scooter_list', scooters: getScooterList() });
    }
    // Si déconnexion pendant OTA → c'est un reboot → succès
    if (otaInProgress && otaResolve) {
      otaResolve({ success: true, message: 'ESP32 rebooté après flash' });
    }
  });
});

// ─── Démarrage ────────────────────────────────────────────────────────────────
const server = http.createServer(app);

server.on('upgrade', (req, socket, head) => {
  if (req.url === '/ws-esp32') {
    esp32Wss.handleUpgrade(req, socket, head, (ws) => {
      esp32Wss.emit('connection', ws, req);
    });
  } else if (req.url === '/ws-debug-audio') {
    debugAudioWss.handleUpgrade(req, socket, head, (ws) => {
      debugAudioWss.emit('connection', ws, req);
    });
  }
});

server.listen(PORT, () => {
  console.log(`[proxy] démarré sur http://0.0.0.0:${PORT}`);
  console.log(`[proxy] mode IA : ${USE_LOCAL_AI ? 'local' : 'cloud'}`);

  // Vérifier les nouvelles releases et pré-compiler au démarrage, puis toutes les 5 minutes
  const builds = getLocalBuilds();
  console.log(`[build] ${builds.length} build(s) pré-compilé(s) en cache`);
  setTimeout(pollAndBuild, 10000); // 10s après le démarrage
  setInterval(pollAndBuild, POLL_INTERVAL_MS);

  // Purger la télémétrie ancienne (>7 jours) au démarrage puis toutes les 24h
  purgeTelemetry(7);
  setInterval(() => purgeTelemetry(7), 24 * 60 * 60 * 1000);
});

if (!USE_LOCAL_AI) connectOpenAI();
