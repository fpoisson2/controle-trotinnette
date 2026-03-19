'use strict';

// fleet.js — Gestion de flotte avec persistance JSON + routes API

const express = require('express');
const path    = require('path');
const fs      = require('fs');
const crypto  = require('crypto');

const { registerScooter, updateScooterStatus, getScooterRegistry, addAuditLog } = require('./database');
const { requireAuth, requireRole } = require('./auth-routes');

const router = express.Router();

const DATA_DIR   = path.join(__dirname, 'data');
const FLEET_PATH = path.join(DATA_DIR, 'fleet.json');

// ─── Registre en mémoire (chargé depuis fleet.json) ──────────────────────────
// Format: { [mac]: { name, firstSeen, lastSeen, qcStatus, notes, fwVersion } }
let fleet = {};
let savePending = false;
let saveTimer   = null;

// ─── Charger le fichier fleet.json ────────────────────────────────────────────
function loadFleet() {
  if (!fs.existsSync(DATA_DIR)) {
    fs.mkdirSync(DATA_DIR, { recursive: true });
  }

  if (fs.existsSync(FLEET_PATH)) {
    try {
      fleet = JSON.parse(fs.readFileSync(FLEET_PATH, 'utf8'));
      console.log(`[fleet] ${Object.keys(fleet).length} trottinette(s) chargée(s) depuis fleet.json`);
    } catch (err) {
      console.error('[fleet] erreur lecture fleet.json :', err.message);
      fleet = {};
    }
  } else {
    fleet = {};
    console.log('[fleet] fleet.json inexistant — registre vide');
  }
}

// ─── Sauvegarder fleet.json (debounce 5 secondes) ────────────────────────────
function saveFleet() {
  if (savePending) return;
  savePending = true;

  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    try {
      if (!fs.existsSync(DATA_DIR)) fs.mkdirSync(DATA_DIR, { recursive: true });
      fs.writeFileSync(FLEET_PATH, JSON.stringify(fleet, null, 2));
      console.log('[fleet] fleet.json sauvegardé');
    } catch (err) {
      console.error('[fleet] erreur sauvegarde fleet.json :', err.message);
    }
    savePending = false;
  }, 5000);
}

// ─── Enregistrement automatique à la connexion WebSocket ─────────────────────
function registerOnConnect(mac, fwVersion) {
  const now = new Date().toISOString();

  if (!fleet[mac]) {
    fleet[mac] = {
      name: mac.split(':').slice(-2).join(':'),
      firstSeen: now,
      lastSeen: now,
      qcStatus: null,
      notes: '',
      fwVersion: fwVersion || null
    };
    console.log(`[fleet] nouvelle trottinette enregistrée : ${mac}`);
  } else {
    fleet[mac].lastSeen = now;
    if (fwVersion) fleet[mac].fwVersion = fwVersion;
  }

  saveFleet();

  // Enregistrer aussi dans la base SQLite
  try {
    registerScooter(mac, fleet[mac].name, fwVersion);
  } catch (err) {
    console.error(`[fleet] erreur enregistrement DB pour ${mac} :`, err.message);
  }
}

// ─── Fonctions exportées ─────────────────────────────────────────────────────
function getFleet() {
  return { ...fleet };
}

function renameScooter(mac, name) {
  if (!fleet[mac]) return null;
  fleet[mac].name = name;
  saveFleet();
  return fleet[mac];
}

function setNotes(mac, notes) {
  if (!fleet[mac]) return null;
  fleet[mac].notes = notes;
  saveFleet();
  return fleet[mac];
}

function removeScooter(mac) {
  if (!fleet[mac]) return false;
  delete fleet[mac];
  saveFleet();
  return true;
}

function getScooterInfo(mac) {
  return fleet[mac] || null;
}

// ─── Routes API ──────────────────────────────────────────────────────────────

// GET /api/fleet — registre complet (connectées + hors ligne), fusionné avec le statut live
router.get('/api/fleet', requireAuth, (req, res) => {
  // Accéder aux scooters connectées via le module parent (proxy.js)
  const liveScooters = router._getLiveScooters ? router._getLiveScooters() : new Map();
  const WebSocket = require('ws');

  const result = [];

  // Toutes les trottinettes du registre
  const allMacs = new Set([...Object.keys(fleet), ...liveScooters.keys()]);

  for (const mac of allMacs) {
    const fleetEntry = fleet[mac] || {};
    const live = liveScooters.get(mac);

    result.push({
      mac,
      name: fleetEntry.name || mac.split(':').slice(-2).join(':'),
      firstSeen: fleetEntry.firstSeen || null,
      lastSeen: fleetEntry.lastSeen || null,
      fwVersion: live?.fwVersion || fleetEntry.fwVersion || null,
      qcStatus: fleetEntry.qcStatus || null,
      notes: fleetEntry.notes || '',
      connected: !!live && live.ws.readyState === WebSocket.OPEN,
      locked: live ? live.locked !== false : null,
      telemetry: live?.telemetry || null
    });
  }

  // Trier : connectées en premier, puis par lastSeen
  result.sort((a, b) => {
    if (a.connected !== b.connected) return b.connected ? 1 : -1;
    return (b.lastSeen || '').localeCompare(a.lastSeen || '');
  });

  res.json(result);
});

// POST /api/fleet/:mac/rename — renommer (manager/admin)
router.post('/api/fleet/:mac/rename', requireAuth, requireRole('manager', 'admin'), (req, res) => {
  const { name } = req.body || {};
  if (!name) return res.status(400).json({ error: 'Nom requis' });

  const entry = renameScooter(req.params.mac, name);
  if (!entry) return res.status(404).json({ error: 'Trottinette non trouvée dans le registre' });

  addAuditLog(req.user.sub, 'rename_scooter', req.params.mac, { name });
  console.log(`[fleet] ${req.params.mac} renommée en "${name}" par ${req.user.email}`);
  res.json({ ok: true, scooter: entry });
});

// POST /api/fleet/:mac/notes — ajouter des notes (manager/admin)
router.post('/api/fleet/:mac/notes', requireAuth, requireRole('manager', 'admin'), (req, res) => {
  const { notes } = req.body || {};

  const entry = setNotes(req.params.mac, notes || '');
  if (!entry) return res.status(404).json({ error: 'Trottinette non trouvée dans le registre' });

  addAuditLog(req.user.sub, 'set_notes', req.params.mac, { notes });
  res.json({ ok: true, scooter: entry });
});

// DELETE /api/fleet/:mac — retirer du registre (admin)
router.delete('/api/fleet/:mac', requireAuth, requireRole('admin'), (req, res) => {
  const removed = removeScooter(req.params.mac);
  if (!removed) return res.status(404).json({ error: 'Trottinette non trouvée dans le registre' });

  addAuditLog(req.user.sub, 'remove_scooter', req.params.mac, null);
  console.log(`[fleet] ${req.params.mac} retirée du registre par ${req.user.email}`);
  res.json({ ok: true });
});

// POST /api/fleet/:mac/qc — lancer un test QC (admin)
router.post('/api/fleet/:mac/qc', requireAuth, requireRole('admin'), (req, res) => {
  const mac = req.params.mac;
  const liveScooters = router._getLiveScooters ? router._getLiveScooters() : new Map();
  const WebSocket = require('ws');
  const live = liveScooters.get(mac);

  if (!live || live.ws.readyState !== WebSocket.OPEN) {
    return res.status(503).json({ error: 'Trottinette non connectée' });
  }

  // Envoyer la demande de test QC à l'ESP32
  live.ws.send(JSON.stringify({ type: 'qc_request' }));

  if (fleet[mac]) {
    fleet[mac].qcStatus = 'pending';
    saveFleet();
  }

  addAuditLog(req.user.sub, 'qc_request', mac, null);
  console.log(`[fleet] test QC lancé sur ${mac} par ${req.user.email}`);
  res.json({ ok: true, message: 'Test QC envoyé' });
});

// POST /api/ota/flash-all — flasher toutes les trottinettes connectées (admin)
router.post('/api/ota/flash-all', requireAuth, requireRole('admin'), async (req, res) => {
  const liveScooters = router._getLiveScooters ? router._getLiveScooters() : new Map();
  const flashFirmware = router._flashFirmware;
  const broadcastSSE  = router._broadcastSSE;
  const WebSocket = require('ws');

  if (!flashFirmware) {
    return res.status(500).json({ error: 'Fonction flashFirmware non disponible' });
  }

  const { version } = req.body || {};
  if (!version) {
    return res.status(400).json({ error: 'Version requise' });
  }

  // Vérifier que le build existe
  const buildsDir = path.join(__dirname, 'builds');
  const binPath = path.join(buildsDir, `${version}.bin`);
  if (!fs.existsSync(binPath)) {
    return res.status(404).json({ error: `Build v${version} non trouvé — compilez d'abord` });
  }

  const firmware = fs.readFileSync(binPath);

  // Lister les trottinettes connectées
  const connected = [];
  for (const [mac, scooter] of liveScooters) {
    if (scooter.ws.readyState === WebSocket.OPEN) {
      connected.push(mac);
    }
  }

  if (connected.length === 0) {
    return res.status(503).json({ error: 'Aucune trottinette connectée' });
  }

  addAuditLog(req.user.sub, 'flash_all', null, { version, count: connected.length });
  console.log(`[fleet] flash-all v${version} lancé sur ${connected.length} trottinette(s) par ${req.user.email}`);

  // Répondre immédiatement
  res.json({ ok: true, message: `Flash v${version} lancé sur ${connected.length} trottinette(s)`, targets: connected });

  // Flash séquentiel avec progression SSE
  (async () => {
    let success = 0;
    let failed  = 0;

    for (let i = 0; i < connected.length; i++) {
      const mac = connected[i];
      if (broadcastSSE) {
        broadcastSSE({
          type: 'flash_all_progress',
          current: i + 1,
          total: connected.length,
          mac,
          status: 'flashing',
          version
        });
      }

      try {
        await flashFirmware(firmware, mac);
        success++;
        console.log(`[fleet] flash-all : ${mac} OK (${i + 1}/${connected.length})`);
      } catch (err) {
        failed++;
        console.error(`[fleet] flash-all : ${mac} ERREUR — ${err.message}`);
      }

      if (broadcastSSE) {
        broadcastSSE({
          type: 'flash_all_progress',
          current: i + 1,
          total: connected.length,
          mac,
          status: failed > success ? 'error' : 'done',
          version
        });
      }
    }

    if (broadcastSSE) {
      broadcastSSE({
        type: 'flash_all_complete',
        success,
        failed,
        total: connected.length,
        version
      });
    }

    console.log(`[fleet] flash-all terminé : ${success} OK, ${failed} échecs`);
  })();
});

// ─── Initialisation ──────────────────────────────────────────────────────────
loadFleet();

module.exports = router;
module.exports.registerOnConnect = registerOnConnect;
module.exports.getFleet          = getFleet;
module.exports.renameScooter     = renameScooter;
module.exports.setNotes          = setNotes;
module.exports.removeScooter     = removeScooter;
module.exports.getScooterInfo    = getScooterInfo;
module.exports.loadFleet         = loadFleet;
