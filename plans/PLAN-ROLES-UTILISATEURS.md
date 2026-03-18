# Plan — Rôles utilisateurs et système de courses

## Trois rôles

### Étudiant (user)
- Carte des trottinettes disponibles
- Réservation → déverrouillage → course → verrouillage
- Jauges simplifiées (vitesse + batterie)
- Commande vocale pendant la course
- Historique de ses courses

### Gestionnaire de flotte (manager)
- Carte flotte complète (code couleur par statut)
- Stats d'utilisation, courses en cours
- Désactiver/activer trottinettes, maintenance
- Limites de vitesse, géofencing
- Historique de toutes les courses

### Administrateur (admin)
- Interface actuelle complète
- OTA firmware, debug ESP32
- Gestion des utilisateurs
- Configuration système

## Base de données — SQLite (better-sqlite3)

### Tables principales

**users** : id, email, password_hash, role, display_name, email_verified, created_at, disabled

**scooters** : id (MAC), name, status (available/in_use/disabled/maintenance), speed_limit, last_seen, fw_version

**rides** : id, user_id, scooter_id, started_at, ended_at, start_lat/lon, end_lat/lon, distance_m, max_speed

**geofences** : id, name, polygon (JSON), speed_limit

**audit_log** : id, user_id, action, target, details, created_at

## Verrouillage/déverrouillage à distance

- La trottinette est **verrouillée par défaut** (ESC au neutre, throttle ignoré)
- Le proxy envoie `{"type":"lock","locked":false}` pour déverrouiller
- Le firmware vérifie `locked` avant d'appliquer les commandes throttle
- Fin de course → reverrouillage automatique
- Admin/gestionnaire peut forcer le verrouillage (urgence, maintenance)

## Flux de course (ride session)

1. `POST /api/rides/start { scooter_id }` → vérifie disponibilité, crée la ride, déverrouille
2. Pendant la course : commandes et audio routés vers SA trottinette uniquement
3. `POST /api/rides/end` → reverrouille, écrit les stats, remet status=available
4. Timeout auto : course > 2h → terminée automatiquement

## JWT enrichi

```json
{
  "sub": 42,
  "email": "etudiant@cegepl.qc.ca",
  "role": "user",
  "display_name": "Jean Tremblay"
}
```

## Middleware `requireRole(...roles)`

```javascript
function requireRole(...roles) {
  return (req, res, next) => {
    if (!roles.includes(req.user.role))
      return res.status(403).json({ error: 'Accès refusé' });
    next();
  };
}
```

## Inscription étudiante

1. Email `@cegepl.qc.ca` ou `@cll.qc.ca` requis
2. Lien de vérification par courriel (nodemailer)
3. Login refusé tant que non vérifié
4. Variable `SKIP_EMAIL_VERIFY=true` pour le dev

## Migration depuis le système actuel

1. Ajouter better-sqlite3 + tables
2. Migrer AUTH_PASSWORD → premier admin dans la BD
3. POST /login cherche dans la BD (fallback ancien système temporaire)
4. Ajouter les routes rides et scooters
5. Frontend conditionnel par rôle (`renderForRole(role)`)
6. Retirer le fallback
