'use strict';

// auth-routes.js — Routes d'authentification multi-utilisateurs avec rôles

const express = require('express');
const bcrypt  = require('bcryptjs');
const jwt     = require('jsonwebtoken');
const router  = express.Router();

const {
  findUserByEmail,
  createUser,
  getAllUsers,
  updateUserRole,
  disableUser,
  addAuditLog
} = require('./database');

const JWT_SECRET = process.env.JWT_SECRET || '';
const JWT_EXPIRY = '12h';

// Domaines courriel autorisés pour l'inscription étudiante/employée
const ALLOWED_DOMAINS = ['cegepl.qc.ca', 'cll.qc.ca'];

// ─── Compatibilité ancienne auth (AUTH_PASSWORD) ──────────────────────────────
const AUTH_USERNAME      = process.env.AUTH_USERNAME || 'admin';
const AUTH_PASSWORD      = process.env.AUTH_PASSWORD || '';
let   AUTH_PASSWORD_HASH = process.env.AUTH_PASSWORD_HASH || '';

if (AUTH_PASSWORD && (!AUTH_PASSWORD_HASH || !AUTH_PASSWORD_HASH.startsWith('$2'))) {
  AUTH_PASSWORD_HASH = bcrypt.hashSync(AUTH_PASSWORD, 12);
}

// ─── Middleware : vérifier le rôle ────────────────────────────────────────────
function requireRole(...roles) {
  return (req, res, next) => {
    if (!req.user) return res.status(401).json({ error: 'Non autorisé' });
    if (!roles.includes(req.user.role)) {
      return res.status(403).json({ error: 'Permission insuffisante' });
    }
    next();
  };
}

// ─── Middleware JWT amélioré — décode l'utilisateur dans req.user ─────────────
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

// ─── Tentatives de login (rate limiting) ──────────────────────────────────────
const loginAttempts = new Map();
const MAX_ATTEMPTS  = 5;
const LOCKOUT_MS    = 15 * 60 * 1000;

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

// ─── POST /api/auth/register — inscription ───────────────────────────────────
router.post('/api/auth/register', async (req, res) => {
  const { email, password, display_name } = req.body || {};

  if (!email || !password) {
    return res.status(400).json({ error: 'Courriel et mot de passe requis' });
  }

  if (password.length < 6) {
    return res.status(400).json({ error: 'Mot de passe trop court (minimum 6 caractères)' });
  }

  // Vérifier le domaine courriel
  const domain = email.split('@')[1]?.toLowerCase();
  if (!domain || !ALLOWED_DOMAINS.includes(domain)) {
    return res.status(400).json({
      error: `Domaine courriel non autorisé. Domaines acceptés : ${ALLOWED_DOMAINS.join(', ')}`
    });
  }

  // Vérifier si l'utilisateur existe déjà
  const existing = findUserByEmail(email.toLowerCase());
  if (existing) {
    return res.status(409).json({ error: 'Ce courriel est déjà utilisé' });
  }

  try {
    const password_hash = await bcrypt.hash(password, 12);
    const user = createUser({
      email: email.toLowerCase(),
      password_hash,
      role: 'user',
      display_name: display_name || email.split('@')[0]
    });

    // Auto-vérification si configuré
    if (process.env.SKIP_EMAIL_VERIFY === 'true') {
      const { getDB } = require('./database');
      getDB().prepare('UPDATE users SET email_verified = 1 WHERE id = ?').run(user.id);
      user.email_verified = 1;
    }

    addAuditLog(user.id, 'register', user.email, { role: 'user' });

    const token = jwt.sign(
      { sub: user.id, email: user.email, role: user.role, display_name: user.display_name },
      JWT_SECRET,
      { expiresIn: JWT_EXPIRY }
    );

    console.log(`[auth] nouvel utilisateur inscrit : ${user.email}`);
    res.status(201).json({ token, user: { id: user.id, email: user.email, role: user.role, display_name: user.display_name } });
  } catch (err) {
    console.error('[auth] erreur inscription :', err.message);
    res.status(500).json({ error: 'Erreur lors de l\'inscription' });
  }
});

// ─── POST /api/auth/login — connexion ────────────────────────────────────────
router.post('/api/auth/login', async (req, res) => {
  const ip = req.headers['x-forwarded-for']?.split(',')[0].trim() || req.socket.remoteAddress;
  const { email, password, username } = req.body || {};

  const { blocked, remaining, rec } = checkRateLimit(ip);
  if (blocked) {
    return res.status(429).json({ error: `Trop de tentatives. Réessayer dans ${remaining} min.` });
  }

  // Identifiant : email ou username (rétro-compatibilité)
  const loginId = (email || username || '').toLowerCase();
  if (!loginId || !password) {
    return res.status(400).json({ error: 'Identifiants requis' });
  }

  // 1) Vérifier dans la base de données
  const dbUser = findUserByEmail(loginId);
  if (dbUser) {
    if (dbUser.disabled) {
      return res.status(403).json({ error: 'Compte désactivé' });
    }

    const validPass = await bcrypt.compare(password, dbUser.password_hash);
    if (validPass) {
      if (!dbUser.email_verified && process.env.SKIP_EMAIL_VERIFY !== 'true') {
        return res.status(403).json({ error: 'Courriel non vérifié' });
      }

      clearAttempts(ip);
      addAuditLog(dbUser.id, 'login', dbUser.email, { ip });

      const token = jwt.sign(
        { sub: dbUser.id, email: dbUser.email, role: dbUser.role, display_name: dbUser.display_name },
        JWT_SECRET,
        { expiresIn: JWT_EXPIRY }
      );

      console.log(`[auth] connexion DB : ${dbUser.email} (${dbUser.role})`);
      return res.json({ token, user: { id: dbUser.id, email: dbUser.email, role: dbUser.role, display_name: dbUser.display_name } });
    }
  }

  // 2) Rétro-compatibilité : ancien système AUTH_PASSWORD
  if (loginId === AUTH_USERNAME && AUTH_PASSWORD_HASH) {
    const validLegacy = await bcrypt.compare(password, AUTH_PASSWORD_HASH);
    if (validLegacy) {
      clearAttempts(ip);

      const token = jwt.sign(
        { sub: 0, email: AUTH_USERNAME, role: 'admin', display_name: 'Administrateur' },
        JWT_SECRET,
        { expiresIn: JWT_EXPIRY }
      );

      console.log(`[auth] connexion legacy : ${AUTH_USERNAME}`);
      return res.json({ token, user: { id: 0, email: AUTH_USERNAME, role: 'admin', display_name: 'Administrateur' } });
    }
  }

  // Échec
  recordFailedAttempt(ip);
  const attemptsLeft = MAX_ATTEMPTS - ((rec?.count || 0) + 1);
  res.status(401).json({
    error: attemptsLeft > 0
      ? `Identifiants incorrects (${attemptsLeft} tentative${attemptsLeft > 1 ? 's' : ''} restante${attemptsLeft > 1 ? 's' : ''})`
      : 'Compte verrouillé 15 minutes'
  });
});

// ─── GET /api/auth/me — info utilisateur courant ─────────────────────────────
router.get('/api/auth/me', requireAuth, (req, res) => {
  res.json({
    id: req.user.sub,
    email: req.user.email,
    role: req.user.role,
    display_name: req.user.display_name
  });
});

// ─── GET /api/users — liste des utilisateurs (admin) ────────────────────────
router.get('/api/users', requireAuth, requireRole('admin'), (req, res) => {
  res.json(getAllUsers());
});

// ─── POST /api/users/:id/role — changer le rôle (admin) ─────────────────────
router.post('/api/users/:id/role', requireAuth, requireRole('admin'), (req, res) => {
  const { role } = req.body || {};
  const validRoles = ['user', 'manager', 'admin'];
  if (!role || !validRoles.includes(role)) {
    return res.status(400).json({ error: `Rôle invalide. Valeurs acceptées : ${validRoles.join(', ')}` });
  }

  const user = updateUserRole(parseInt(req.params.id), role);
  if (!user) return res.status(404).json({ error: 'Utilisateur non trouvé' });

  addAuditLog(req.user.sub, 'change_role', user.email, { newRole: role });
  console.log(`[auth] rôle changé : ${user.email} → ${role} (par ${req.user.email})`);
  res.json(user);
});

// ─── POST /api/users/:id/disable — désactiver un compte (admin) ─────────────
router.post('/api/users/:id/disable', requireAuth, requireRole('admin'), (req, res) => {
  const user = disableUser(parseInt(req.params.id));
  if (!user) return res.status(404).json({ error: 'Utilisateur non trouvé' });

  addAuditLog(req.user.sub, 'disable_user', user.email, null);
  console.log(`[auth] utilisateur désactivé : ${user.email} (par ${req.user.email})`);
  res.json(user);
});

module.exports = router;
module.exports.requireAuth = requireAuth;
module.exports.requireRole = requireRole;
