'use strict';

require('dotenv').config();

const express  = require('express');
const http     = require('http');
const WebSocket = require('ws');
const path     = require('path');
const fs       = require('fs');
const { execSync } = require('child_process');
const os       = require('os');
const bcrypt   = require('bcryptjs');
const jwt      = require('jsonwebtoken');

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
function requireAuth(req, res, next) {
  const header = req.headers.authorization;
  const token  = (header?.startsWith('Bearer ') ? header.slice(7) : null)
               ?? req.query.token;
  if (!token) return res.status(401).json({ error: 'Non autorisé' });
  try {
    jwt.verify(token, JWT_SECRET);
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
let esp32Ws           = null;      // WebSocket ESP32 (/ws-esp32)
let openaiWs          = null;      // WebSocket vers OpenAI Realtime
let openaiConnected   = false;
let lastTelemetry     = { speed: 0, voltage: 0, current: 0, temp: 0, lat: 0, lon: 0 };
let esp32FwVersion    = null;      // version firmware ESP32 (reçue via hello)
let esp32DebugMode    = false;     // mode debug ESP32 actif

// ─── Broadcast vers navigateurs (SSE) + ESP32 (WS) ───────────────────────────
function broadcastSSE(obj) {
  const payload = `data: ${JSON.stringify(obj)}\n\n`;
  for (const client of sseClients) {
    try { client.write(payload); } catch (_) { /* client déconnecté */ }
  }
  // Envoyer aussi à l'ESP32 via WebSocket
  if (esp32Ws && esp32Ws.readyState === WebSocket.OPEN) {
    try { esp32Ws.send(JSON.stringify(obj)); } catch (_) {}
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

  openaiWs.on('open', () => {
    openaiConnected = true;
    console.log('[openai] connecté – envoi session.update');

    openaiWs.send(JSON.stringify({
      type: 'session.update',
      session: {
        modalities: ['text', 'audio'],
        instructions:
          'Tu es le système de contrôle vocal d\'une trottinette électrique. ' +
          'Tu communiques UNIQUEMENT en français, en une phrase courte. ' +
          'IL Y A DEUX TYPES DE DEMANDES — ne les confonds JAMAIS :\n' +
          '1) QUESTIONS / INFORMATIONS (batterie, tension, vitesse, température, position) ' +
          '→ appelle UNIQUEMENT lire_telemetrie. N\'appelle JAMAIS commande_trottinette pour une question.\n' +
          '2) COMMANDES DE MOUVEMENT (avancer, accélérer, freiner, arrêter, changer de vitesse) ' +
          '→ appelle UNIQUEMENT commande_trottinette.\n' +
          'Exemples QUESTIONS : "quelle est la tension ?" → lire_telemetrie ; ' +
          '"on roule à combien ?" → lire_telemetrie ; ' +
          '"batterie ?" → lire_telemetrie.\n' +
          'Exemples COMMANDES : "avance" → commande_trottinette(avancer, 0.6) ; ' +
          '"à fond" → commande_trottinette(avancer, 1.0) ; ' +
          '"doucement" → commande_trottinette(avancer, 0.3) ; ' +
          '"freine" → commande_trottinette(freiner, 0.8) ; ' +
          '"stop" → commande_trottinette(arreter).\n' +
          'Appelle TOUJOURS l\'outil EN PREMIER, puis confirme en une phrase.',
        voice: 'alloy',
        input_audio_format: 'pcm16',
        output_audio_format: 'pcm16',
        input_audio_transcription: null,
        turn_detection: {
          type: 'semantic_vad',
          eagerness: 'high',
          create_response: true,
          interrupt_response: true
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

      // Delta audio de la réponse (GA et beta) — envoyé en morceaux de 4096 chars
      case 'response.output_audio.delta':
      case 'response.audio.delta': {
        const b64 = event.delta ?? '';
        const CHUNK = 4096;
        for (let i = 0; i < b64.length; i += CHUNK) {
          broadcastSSE({ type: 'audio', data: b64.slice(i, i + CHUNK) });
        }
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
                  telemetry: lastTelemetry
                })
              }
            }));
            openaiWs.send(JSON.stringify({ type: 'response.create' }));
          } catch (err) {
            console.error('[openai] erreur parsing tool call :', err.message);
          }
        } else if (event.name === 'lire_telemetrie') {
          console.log('[openai] lire_telemetrie :', JSON.stringify(lastTelemetry));
          openaiWs.send(JSON.stringify({
            type: 'conversation.item.create',
            item: {
              type: 'function_call_output',
              call_id: event.call_id,
              output: JSON.stringify(lastTelemetry)
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
  res.sendFile(path.join(__dirname, 'index.html'));
});

// ── POST /login ──
app.post('/login', async (req, res) => {
  const ip = req.headers['x-forwarded-for']?.split(',')[0].trim() || req.socket.remoteAddress;
  const { username, password } = req.body || {};

  const { blocked, remaining, rec } = checkRateLimit(ip);
  if (blocked) {
    return res.status(429).json({ error: `Trop de tentatives. Réessayer dans ${remaining} min.` });
  }

  const validUser = username === AUTH_USERNAME;
  const validPass = AUTH_PASSWORD_HASH
    ? await bcrypt.compare(password ?? '', AUTH_PASSWORD_HASH)
    : false;

  if (!validUser || !validPass) {
    recordFailedAttempt(ip);
    const attemptsLeft = MAX_ATTEMPTS - (rec.count + 1);
    return res.status(401).json({
      error: attemptsLeft > 0
        ? `Identifiants incorrects (${attemptsLeft} tentative${attemptsLeft > 1 ? 's' : ''} restante${attemptsLeft > 1 ? 's' : ''})`
        : 'Compte verrouillé 15 minutes'
    });
  }

  clearAttempts(ip);
  const token = jwt.sign({ username }, JWT_SECRET, { expiresIn: JWT_EXPIRY });
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

// ── POST /debug — activer/désactiver le mode debug ESP32 ──
app.post('/debug', (req, res) => {
  const { enabled } = req.body;
  esp32DebugMode = !!enabled;
  if (esp32Ws && esp32Ws.readyState === WebSocket.OPEN) {
    esp32Ws.send(JSON.stringify({ type: 'debug', enabled: esp32DebugMode }));
  }
  console.log(`[debug] mode ${esp32DebugMode ? 'activé' : 'désactivé'}`);
  res.json({ ok: true, debug: esp32DebugMode });
});

// ── GET /status ──
app.get('/status', (req, res) => {
  const esp32Connected = esp32Ws && esp32Ws.readyState === WebSocket.OPEN;
  res.json({
    mode: USE_LOCAL_AI ? 'local' : 'cloud',
    openai_connected: USE_LOCAL_AI ? null : openaiConnected,
    scooter_connected: esp32Connected,
    fw_version: esp32FwVersion,
    debug: esp32DebugMode
  });
});

// ─── OTA via proxy : flashFirmware() partagé ──────────────────────────────────
let otaInProgress = false;
let otaResolve    = null;

// Fonction partagée : envoie un Buffer firmware à l'ESP32 via WebSocket
// Retourne une Promise<{ok, message, error}>
function flashFirmware(firmware) {
  return new Promise((resolve, reject) => {
    if (!esp32Ws || esp32Ws.readyState !== WebSocket.OPEN) {
      return reject(new Error('ESP32 non connecté'));
    }
    if (otaInProgress) {
      return reject(new Error('OTA déjà en cours'));
    }
    if (!Buffer.isBuffer(firmware) || firmware.length < 1000) {
      return reject(new Error('Firmware invalide (trop petit ou pas un Buffer)'));
    }

    console.log(`[ota] envoi firmware ${firmware.length} bytes à l'ESP32`);
    otaInProgress = true;

    esp32Ws.send(JSON.stringify({ type: 'ota_begin', size: firmware.length }));

    const CHUNK = 1024;
    let offset = 0;

    function sendNextChunk() {
      if (offset >= firmware.length) {
        esp32Ws.send(JSON.stringify({ type: 'ota_end' }));
        return;
      }
      const end = Math.min(offset + CHUNK, firmware.length);
      const chunk = firmware.slice(offset, end);
      esp32Ws.send(chunk, { binary: true }, () => {
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

// POST /ota — upload manuel .bin
app.post('/ota', async (req, res) => {
  try {
    const result = await flashFirmware(req.body);
    res.json(result);
  } catch (err) {
    const status = err.message.includes('non connecté') ? 503
                 : err.message.includes('déjà en cours') ? 409
                 : err.message.includes('Timeout') ? 504
                 : err.message.includes('invalide') ? 400 : 500;
    res.status(status).json({ error: err.message });
  }
});

// ─── GitHub Releases API — compilation locale ────────────────────────────────
const GITHUB_REPO = 'fpoisson2/controle-trotinnette';
let releaseCache = { data: null, ts: 0 };
const RELEASE_CACHE_MS = 5 * 60 * 1000; // 5 minutes

async function fetchLatestRelease() {
  const now = Date.now();
  if (releaseCache.data && (now - releaseCache.ts) < RELEASE_CACHE_MS) {
    return releaseCache.data;
  }

  const url = `https://api.github.com/repos/${GITHUB_REPO}/releases/latest`;
  const resp = await fetch(url, {
    headers: { 'Accept': 'application/vnd.github+json', 'User-Agent': 'trottinette-proxy' }
  });
  if (!resp.ok) {
    if (resp.status === 404) return null;
    throw new Error(`GitHub API ${resp.status}: ${resp.statusText}`);
  }

  const release = await resp.json();
  const result = {
    version: release.tag_name.replace(/^v/, ''),
    tag: release.tag_name,
    published_at: release.published_at,
    tarball_url: release.tarball_url,
    body: release.body || ''
  };

  releaseCache = { data: result, ts: now };
  return result;
}

// GET /api/releases/latest
app.get('/api/releases/latest', async (req, res) => {
  try {
    const release = await fetchLatestRelease();
    if (!release) return res.status(404).json({ error: 'Aucune release trouvée' });
    res.json(release);
  } catch (err) {
    console.error('[github] erreur fetch release :', err.message);
    res.status(502).json({ error: 'Impossible de contacter GitHub : ' + err.message });
  }
});

// Compile le firmware depuis le source GitHub + config.h local
// Retourne le Buffer du .bin compilé
const { spawn } = require('child_process');

let buildInProgress = false;

async function buildFirmwareFromRelease(release) {
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'fw-build-'));
  const tarball = path.join(tmpDir, 'source.tar.gz');
  const localConfigH = path.join(__dirname, 'firmware', 'include', 'config.h');

  try {
    // Télécharger le tarball source
    broadcastSSE({ type: 'ota_build', step: 'download', msg: 'Téléchargement du source...' });
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

    // Trouver le répertoire extrait (GitHub le nomme owner-repo-sha/)
    const extracted = fs.readdirSync(tmpDir).find(f =>
      f !== 'source.tar.gz' && fs.statSync(path.join(tmpDir, f)).isDirectory()
    );
    if (!extracted) throw new Error('Répertoire source introuvable après extraction');
    const srcDir = path.join(tmpDir, extracted, 'firmware');

    if (!fs.existsSync(srcDir)) throw new Error('Répertoire firmware/ introuvable dans le source');

    // Copier le config.h local (contient WiFi, EAP, proxy, OTA)
    if (!fs.existsSync(localConfigH)) {
      throw new Error('config.h local introuvable — nécessaire pour la compilation');
    }
    fs.copyFileSync(localConfigH, path.join(srcDir, 'include', 'config.h'));

    // Injecter la version du tag dans config.h
    const configPath = path.join(srcDir, 'include', 'config.h');
    let configContent = fs.readFileSync(configPath, 'utf8');
    configContent = configContent.replace(
      /#define\s+FW_VERSION\s+"[^"]*"/,
      `#define FW_VERSION  "${release.version}"`
    );
    fs.writeFileSync(configPath, configContent);

    // Build avec PlatformIO (spawn pour streamer la sortie)
    broadcastSSE({ type: 'ota_build', step: 'compile', msg: 'Compilation PlatformIO...' });
    console.log('[build] compilation PlatformIO...');

    await new Promise((resolve, reject) => {
      const proc = spawn('pio', ['run', '-e', 'esp32dev'], {
        cwd: srcDir,
        timeout: 300000
      });

      let lastPct = 0;
      proc.stdout.on('data', (data) => {
        const line = data.toString().trim();
        if (!line) return;
        // PlatformIO affiche [XX%] pendant la compilation
        const pctMatch = line.match(/\[(\d+)%\]/);
        if (pctMatch) {
          const pct = parseInt(pctMatch[1]);
          if (pct > lastPct) {
            lastPct = pct;
            broadcastSSE({ type: 'ota_build', step: 'compile', percent: pct, msg: `Compilation: ${pct}%` });
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

    // Lire le .bin résultant
    const binPath = path.join(srcDir, '.pio', 'build', 'esp32dev', 'firmware.bin');
    if (!fs.existsSync(binPath)) throw new Error('firmware.bin introuvable après compilation');

    const firmware = fs.readFileSync(binPath);
    console.log(`[build] compilation réussie — ${firmware.length} bytes`);
    broadcastSSE({ type: 'ota_build', step: 'done', msg: `Compilation terminée (${(firmware.length / 1024).toFixed(0)} Ko)` });
    return firmware;
  } finally {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

// POST /api/ota/github — lance le build + flash en arrière-plan, retourne immédiatement
app.post('/api/ota/github', async (req, res) => {
  if (buildInProgress) return res.status(409).json({ error: 'Build déjà en cours' });
  if (otaInProgress) return res.status(409).json({ error: 'OTA déjà en cours' });

  let release;
  try {
    release = await fetchLatestRelease();
    if (!release) return res.status(404).json({ error: 'Aucune release trouvée' });
  } catch (err) {
    return res.status(502).json({ error: err.message });
  }

  // Répondre immédiatement — le reste se fait en arrière-plan
  buildInProgress = true;
  res.json({ ok: true, message: `Build ${release.version} lancé` });

  // Arrière-plan : build → flash → broadcast résultat
  (async () => {
    try {
      console.log(`[ota-github] build + flash version ${release.version}...`);
      const firmware = await buildFirmwareFromRelease(release);

      broadcastSSE({ type: 'ota_build', step: 'flash', msg: 'Flash OTA en cours...' });
      await flashFirmware(firmware);

      broadcastSSE({ type: 'ota_build', step: 'success', msg: `Firmware ${release.version} flashé — redémarrage ESP32` });
    } catch (err) {
      console.error('[ota-github] erreur :', err.message);
      broadcastSSE({ type: 'ota_build', step: 'error', msg: err.message });
    } finally {
      buildInProgress = false;
    }
  })();
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

// ─── WebSocket ESP32 (/ws-esp32) — audio entrant + events sortants ────────────
const esp32Wss = new WebSocket.Server({ noServer: true });

esp32Wss.on('connection', (ws) => {
  console.log('[esp32-ws] ESP32 connecté');
  esp32Ws = ws;

  ws.on('message', (data, isBinary) => {
    if (isBinary) {
      // Audio PCM → OpenAI Realtime
      if (openaiWs && openaiWs.readyState === WebSocket.OPEN) {
        openaiWs.send(JSON.stringify({
          type: 'input_audio_buffer.append',
          audio: Buffer.from(data).toString('base64')
        }));
      } else {
        console.warn(`[esp32-ws] audio reçu mais OpenAI non connecté (ws=${!!openaiWs}, state=${openaiWs?.readyState})`);
      }
      // Relayer le PCM brut aux clients debug-audio
      for (const c of debugAudioClients) {
        if (c.readyState === WebSocket.OPEN) {
          try { c.send(data, { binary: true }); } catch (_) {}
        }
      }
    } else {
      // Message texte : télémétrie JSON venant de l'ESP32
      try {
        const msg = JSON.parse(data.toString());
        if (msg.type === 'telemetry') {
          const { type, ...fields } = msg;
          lastTelemetry = { ...lastTelemetry, ...fields };
          // Relayer aux clients SSE (navigateur)
          const payload = `data: ${JSON.stringify({ type: 'telemetry', ...fields })}\n\n`;
          for (const c of sseClients) { try { c.write(payload); } catch (_) {} }
        } else if (msg.type === 'hello') {
          esp32FwVersion = msg.version || null;
          console.log(`[esp32-ws] firmware version: ${esp32FwVersion}`);
          // Restaurer l'état debug si actif
          if (esp32DebugMode) {
            ws.send(JSON.stringify({ type: 'debug', enabled: true }));
          }
        } else if (msg.type === 'log') {
          // Relayer les logs ESP32 aux clients SSE
          const payload = `data: ${JSON.stringify({ type: 'esp32_log', msg: msg.msg })}\n\n`;
          for (const c of sseClients) { try { c.write(payload); } catch (_) {} }
        } else if (msg.type === 'ota_progress') {
          console.log(`[ota] ESP32 : ${msg.percent}%`);
          broadcastSSE({ type: 'ota_progress', percent: msg.percent });
        } else if (msg.type === 'ota_result') {
          if (otaResolve) otaResolve(msg);
        }
      } catch (_) { /* ignore */ }
    }
  });

  ws.on('close', () => {
    console.log('[esp32-ws] ESP32 déconnecté');
    if (esp32Ws === ws) esp32Ws = null;
    // Si l'ESP32 se déconnecte pendant un OTA, c'est un reboot → succès
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
});

if (!USE_LOCAL_AI) connectOpenAI();
