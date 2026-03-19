'use strict';

// database.js — Base de données SQLite pour utilisateurs, courses, trottinettes, géofences, journal d'audit

const path    = require('path');
const fs      = require('fs');
const bcrypt  = require('bcryptjs');

const DATA_DIR = path.join(__dirname, 'data');
const DB_PATH  = path.join(DATA_DIR, 'trottinette.db');

let db = null;

// ─── Initialisation ────────────────────────────────────────────────────────────
function initDB() {
  // Créer le répertoire data/ si nécessaire
  if (!fs.existsSync(DATA_DIR)) {
    fs.mkdirSync(DATA_DIR, { recursive: true });
    console.log('[db] répertoire data/ créé');
  }

  const Database = require('better-sqlite3');
  db = new Database(DB_PATH);

  // Activer le mode WAL pour de meilleures performances concurrentes
  db.pragma('journal_mode = WAL');
  db.pragma('foreign_keys = ON');

  // ── Création des tables ──
  db.exec(`
    CREATE TABLE IF NOT EXISTS users (
      id             INTEGER PRIMARY KEY AUTOINCREMENT,
      email          TEXT UNIQUE NOT NULL,
      password_hash  TEXT NOT NULL,
      role           TEXT DEFAULT 'user' CHECK(role IN ('user','manager','admin')),
      display_name   TEXT,
      email_verified INTEGER DEFAULT 0,
      created_at     TEXT DEFAULT (datetime('now')),
      disabled       INTEGER DEFAULT 0
    );

    CREATE TABLE IF NOT EXISTS scooters (
      id           TEXT PRIMARY KEY,
      name         TEXT,
      status       TEXT DEFAULT 'available' CHECK(status IN ('available','in_use','disabled','maintenance')),
      speed_limit  REAL DEFAULT 20.0,
      last_seen    TEXT,
      fw_version   TEXT,
      notes        TEXT
    );

    CREATE TABLE IF NOT EXISTS rides (
      id          TEXT PRIMARY KEY,
      user_id     INTEGER REFERENCES users(id),
      scooter_id  TEXT,
      started_at  TEXT,
      ended_at    TEXT,
      start_lat   REAL,
      start_lon   REAL,
      end_lat     REAL,
      end_lon     REAL,
      distance_m  REAL DEFAULT 0,
      max_speed   REAL DEFAULT 0,
      avg_speed   REAL DEFAULT 0
    );

    CREATE TABLE IF NOT EXISTS geofences (
      id          INTEGER PRIMARY KEY AUTOINCREMENT,
      name        TEXT,
      polygon     TEXT,
      speed_limit REAL,
      active      INTEGER DEFAULT 1
    );

    CREATE TABLE IF NOT EXISTS audit_log (
      id          INTEGER PRIMARY KEY AUTOINCREMENT,
      user_id     INTEGER,
      action      TEXT,
      target      TEXT,
      details     TEXT,
      created_at  TEXT DEFAULT (datetime('now'))
    );
  `);

  console.log('[db] tables créées/vérifiées');

  // ── Créer l'admin par défaut à partir des variables d'environnement ──
  const adminEmail    = process.env.AUTH_USERNAME || 'admin';
  const adminPassword = process.env.AUTH_PASSWORD || '';

  if (adminEmail && adminPassword) {
    const existing = db.prepare('SELECT id FROM users WHERE email = ?').get(adminEmail);
    if (!existing) {
      const hash = bcrypt.hashSync(adminPassword, 12);
      db.prepare(
        'INSERT INTO users (email, password_hash, role, display_name, email_verified) VALUES (?, ?, ?, ?, 1)'
      ).run(adminEmail, hash, 'admin', 'Administrateur');
      console.log(`[db] admin par défaut créé : ${adminEmail}`);
    }
  }

  return db;
}

function getDB() {
  return db;
}

// ─── Utilisateurs ──────────────────────────────────────────────────────────────
function findUserByEmail(email) {
  return db.prepare('SELECT * FROM users WHERE email = ?').get(email);
}

function createUser({ email, password_hash, role = 'user', display_name = null }) {
  const result = db.prepare(
    'INSERT INTO users (email, password_hash, role, display_name) VALUES (?, ?, ?, ?)'
  ).run(email, password_hash, role, display_name);
  return db.prepare('SELECT * FROM users WHERE id = ?').get(result.lastInsertRowid);
}

function getAllUsers() {
  return db.prepare(
    'SELECT id, email, role, display_name, email_verified, created_at, disabled FROM users ORDER BY created_at DESC'
  ).all();
}

function updateUserRole(id, role) {
  db.prepare('UPDATE users SET role = ? WHERE id = ?').run(role, id);
  return db.prepare('SELECT id, email, role, display_name FROM users WHERE id = ?').get(id);
}

function disableUser(id) {
  db.prepare('UPDATE users SET disabled = 1 WHERE id = ?').run(id);
  return db.prepare('SELECT id, email, role, disabled FROM users WHERE id = ?').get(id);
}

// ─── Trottinettes (registre persistant) ────────────────────────────────────────
function registerScooter(mac, name, fwVersion) {
  const existing = db.prepare('SELECT id FROM scooters WHERE id = ?').get(mac);
  if (existing) {
    db.prepare(
      'UPDATE scooters SET last_seen = datetime("now"), fw_version = COALESCE(?, fw_version), name = COALESCE(?, name) WHERE id = ?'
    ).run(fwVersion || null, name || null, mac);
  } else {
    db.prepare(
      'INSERT INTO scooters (id, name, fw_version, last_seen) VALUES (?, ?, ?, datetime("now"))'
    ).run(mac, name || mac, fwVersion || null);
  }
  return db.prepare('SELECT * FROM scooters WHERE id = ?').get(mac);
}

function updateScooterStatus(mac, status) {
  db.prepare('UPDATE scooters SET status = ?, last_seen = datetime("now") WHERE id = ?').run(status, mac);
}

function getScooterRegistry() {
  return db.prepare('SELECT * FROM scooters ORDER BY last_seen DESC').all();
}

// ─── Courses (historique persistant) ───────────────────────────────────────────
function createRide({ id, userId, scooterId, startLat, startLon }) {
  db.prepare(
    'INSERT INTO rides (id, user_id, scooter_id, started_at, start_lat, start_lon) VALUES (?, ?, ?, datetime("now"), ?, ?)'
  ).run(id, userId || null, scooterId, startLat || null, startLon || null);
  return db.prepare('SELECT * FROM rides WHERE id = ?').get(id);
}

function endRide(id, { endLat, endLon, distanceM, maxSpeed, avgSpeed } = {}) {
  db.prepare(
    `UPDATE rides SET ended_at = datetime("now"),
       end_lat = COALESCE(?, end_lat), end_lon = COALESCE(?, end_lon),
       distance_m = COALESCE(?, distance_m), max_speed = COALESCE(?, max_speed),
       avg_speed = COALESCE(?, avg_speed)
     WHERE id = ?`
  ).run(endLat || null, endLon || null, distanceM || null, maxSpeed || null, avgSpeed || null, id);
  return db.prepare('SELECT * FROM rides WHERE id = ?').get(id);
}

function getRides({ userId, scooterId, limit = 50 } = {}) {
  let sql = 'SELECT * FROM rides WHERE 1=1';
  const params = [];
  if (userId) { sql += ' AND user_id = ?'; params.push(userId); }
  if (scooterId) { sql += ' AND scooter_id = ?'; params.push(scooterId); }
  sql += ' ORDER BY started_at DESC LIMIT ?';
  params.push(Math.min(limit, 200));
  return db.prepare(sql).all(...params);
}

function getActiveRidesDB() {
  return db.prepare('SELECT * FROM rides WHERE ended_at IS NULL ORDER BY started_at DESC').all();
}

// ─── Journal d'audit ──────────────────────────────────────────────────────────
function addAuditLog(userId, action, target, details) {
  db.prepare(
    'INSERT INTO audit_log (user_id, action, target, details) VALUES (?, ?, ?, ?)'
  ).run(userId || null, action, target || null, typeof details === 'object' ? JSON.stringify(details) : (details || null));
}

function getAuditLog({ limit = 100 } = {}) {
  return db.prepare(
    'SELECT * FROM audit_log ORDER BY created_at DESC LIMIT ?'
  ).all(Math.min(limit, 500));
}

module.exports = {
  initDB,
  getDB,
  findUserByEmail,
  createUser,
  getAllUsers,
  updateUserRole,
  disableUser,
  registerScooter,
  updateScooterStatus,
  getScooterRegistry,
  createRide,
  endRide,
  getRides,
  getActiveRides: getActiveRidesDB,
  addAuditLog,
  getAuditLog
};
