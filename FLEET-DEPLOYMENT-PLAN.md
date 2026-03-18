# Plan de deploiement automatise de flotte -- Plateforme web

## Vue d'ensemble

Ce plan decrit les modifications necessaires pour transformer le systeme mono-trottinette en plateforme de gestion de flotte libre-service, avec provisionnement web, deverrouillage a distance, et OTA de flotte depuis le dashboard.

---

## 1. Mecanisme de verrouillage/deverrouillage a distance (Lock/Unlock)

### 1.1 Firmware : etat `locked` controle par le serveur

**Fichier : `firmware/src/main.cpp`**

Ajouter un etat global `scooterLocked` qui empeche toute commande moteur :

```cpp
// ── Verrouillage a distance ──
static volatile bool scooterLocked = true;  // VERROUILLE par defaut au boot

// Dans la section cmd 02H du loop() :
// AVANT d'envoyer quoi que ce soit a l'ESC :
if (scooterLocked) {
    currentThrottle = FTESC_THROTTLE_NEUTRAL;
    brakeLightOn = false;
    // Ignorer toutes les commandes IA et manette
}
```

**Pourquoi verrouille par defaut** : securite. Une trottinette qui redemarre (apres crash, perte batterie, OTA) ne doit pas etre utilisable sans autorisation explicite du serveur.

**Traitement du message `unlock`/`lock` dans `onWsProxyEvent()`** :

```cpp
} else if (strcmp(evtype, "lock") == 0) {
    scooterLocked = true;
    currentThrottle = FTESC_THROTTLE_NEUTRAL;  // arret immediat
    brakeLightOn = false;
    wsLog("[lock] trottinette verrouillee");
} else if (strcmp(evtype, "unlock") == 0) {
    scooterLocked = false;
    wsLog("[unlock] trottinette deverrouillee");
}
```

**Ajout de l'etat `locked` dans la telemetrie** (`sendTelemetry()`) :

```cpp
doc["locked"] = scooterLocked;
```

**Ajout de l'etat `locked` dans le message `hello`** :

```cpp
snprintf(hello, sizeof(hello),
    "{\"type\":\"hello\",\"version\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"locked\":true}",
    FW_VERSION, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
```

### 1.2 Proxy : API et gestion de l'etat lock

**Fichier : `proxy.js`**

Ajouter `locked: true` dans le modele scooter :

```js
// Dans esp32Wss 'hello' handler (ligne ~1077) :
scooters.set(scooterId, {
    ws,
    telemetry: { speed: 0, voltage: 0, current: 0, temp: 0, lat: 0, lon: 0 },
    fwVersion: msg.version || null,
    debugMode: false,
    connectedAt: Date.now(),
    name: null,
    locked: true,          // NOUVEAU : verrouille par defaut
    provisionedAt: null,   // NOUVEAU : null = pas encore provisionne
    status: 'new'          // NOUVEAU : 'new' | 'provisioned' | 'active' | 'maintenance'
});
```

**Nouveaux endpoints API** :

```js
// POST /api/scooters/:id/unlock — deverrouiller une trottinette
app.post('/api/scooters/:id/unlock', requireAuth, (req, res) => {
    const scooter = scooters.get(req.params.id);
    if (!scooter) return res.status(404).json({ error: 'Trottinette non trouvee' });
    if (scooter.ws.readyState !== WebSocket.OPEN)
        return res.status(503).json({ error: 'Trottinette non connectee' });

    scooter.ws.send(JSON.stringify({ type: 'unlock' }));
    scooter.locked = false;
    broadcastSSE({ type: 'scooter_list', scooters: getScooterList() });
    res.json({ ok: true, id: req.params.id, locked: false });
});

// POST /api/scooters/:id/lock — verrouiller une trottinette
app.post('/api/scooters/:id/lock', requireAuth, (req, res) => {
    const scooter = scooters.get(req.params.id);
    if (!scooter) return res.status(404).json({ error: 'Trottinette non trouvee' });
    if (scooter.ws.readyState !== WebSocket.OPEN)
        return res.status(503).json({ error: 'Trottinette non connectee' });

    scooter.ws.send(JSON.stringify({ type: 'lock' }));
    scooter.locked = true;
    broadcastSSE({ type: 'scooter_list', scooters: getScooterList() });
    res.json({ ok: true, id: req.params.id, locked: true });
});
```

**Mettre a jour `getScooterList()`** pour inclure les nouveaux champs :

```js
function getScooterList() {
    return Array.from(scooters.entries()).map(([id, s]) => ({
        id,
        name: s.name || id.split(':').slice(-2).join(':'),
        fwVersion: s.fwVersion,
        connected: s.ws.readyState === WebSocket.OPEN,
        telemetry: s.telemetry,
        selected: id === selectedScooterId,
        connectedAt: s.connectedAt,
        locked: s.locked,            // NOUVEAU
        status: s.status,            // NOUVEAU
        provisionedAt: s.provisionedAt  // NOUVEAU
    }));
}
```

### 1.3 Integration du verrouillage dans le controle IA

Modifier `sendScooterCmd()` pour refuser les commandes si la trottinette est verrouillee :

```js
function sendScooterCmd(action, intensity) {
    const selected = getSelectedScooter();
    if (selected && selected.locked) {
        console.log(`[cmd] refusee — trottinette verrouillee`);
        return; // ne pas envoyer la commande
    }
    const cmd = { type: 'cmd', action };
    if (intensity !== undefined) cmd.intensity = intensity;
    broadcastSSE({ type: 'cmd', action, intensity: intensity ?? null });
}
```

---

## 2. Provisionnement web (flow admin)

### 2.1 Flow complet

```
1. Admin branche ESP32 en USB
2. Flash initial (firmware generique avec PROXY_HOST configure)
   → pio run -e esp32dev --target upload
3. ESP32 se connecte au WiFi puis au proxy via WebSocket
4. Message "hello" avec MAC + version → proxy enregistre avec status='new'
5. Dashboard affiche la nouvelle unite dans la liste (badge "NOUVEAU")
6. Admin clique "Provisionner" → assigne un nom, configure
7. Status passe a 'provisioned' → prete pour le service
8. Admin clique "Deverrouiller" quand necessaire
```

### 2.2 Endpoint de provisionnement

**Fichier : `proxy.js`**

```js
// POST /api/scooters/:id/provision — provisionner une nouvelle unite
app.post('/api/scooters/:id/provision', requireAuth, (req, res) => {
    const { name } = req.body;
    const scooter = scooters.get(req.params.id);
    if (!scooter) return res.status(404).json({ error: 'Trottinette non trouvee' });

    scooter.name = name || req.params.id;
    scooter.status = 'provisioned';
    scooter.provisionedAt = Date.now();

    console.log(`[fleet] provisionne: ${req.params.id} → "${scooter.name}"`);
    broadcastSSE({ type: 'scooter_list', scooters: getScooterList() });
    res.json({ ok: true, id: req.params.id, name: scooter.name });
});

// POST /api/scooters/:id/status — changer le statut (active, maintenance, etc.)
app.post('/api/scooters/:id/status', requireAuth, (req, res) => {
    const { status } = req.body;  // 'active' | 'provisioned' | 'maintenance'
    const scooter = scooters.get(req.params.id);
    if (!scooter) return res.status(404).json({ error: 'Trottinette non trouvee' });
    if (!['new', 'provisioned', 'active', 'maintenance'].includes(status))
        return res.status(400).json({ error: 'Statut invalide' });

    scooter.status = status;
    broadcastSSE({ type: 'scooter_list', scooters: getScooterList() });
    res.json({ ok: true });
});
```

### 2.3 Persistance de la flotte

Actuellement les scooters sont en memoire (`Map`). Pour une flotte, il faut persister les metadonnees (nom, status, date de provisionnement) pour survivre aux redemarrages du proxy.

**Fichier : `proxy.js`** — ajouter un fichier JSON simple :

```js
const FLEET_DB = path.join(__dirname, 'fleet.json');

function loadFleetDB() {
    if (!fs.existsSync(FLEET_DB)) return {};
    try { return JSON.parse(fs.readFileSync(FLEET_DB, 'utf8')); }
    catch (_) { return {}; }
}

function saveFleetDB() {
    const db = {};
    for (const [id, s] of scooters) {
        db[id] = { name: s.name, status: s.status, provisionedAt: s.provisionedAt };
    }
    fs.writeFileSync(FLEET_DB, JSON.stringify(db, null, 2));
}

// Au chargement du proxy :
const fleetDB = loadFleetDB();

// Dans le handler 'hello' — restaurer les metadonnees persistees :
if (fleetDB[scooterId]) {
    const saved = fleetDB[scooterId];
    entry.name = saved.name;
    entry.status = saved.status || 'provisioned';
    entry.provisionedAt = saved.provisionedAt;
}
```

Appeler `saveFleetDB()` apres chaque modification de nom/status/provision.

**Note** : pour une flotte > 50 unites, migrer vers SQLite (`better-sqlite3`). Pour le moment, un fichier JSON suffit.

---

## 3. OTA de flotte depuis le dashboard web

### 3.1 Flash groupé (toutes les trottinettes ou selection)

**Fichier : `proxy.js`**

```js
// POST /api/ota/fleet — flasher toutes les trottinettes connectees (ou une liste)
app.post('/api/ota/fleet', requireAuth, async (req, res) => {
    const { version, targets } = req.body;
    // targets : tableau d'IDs. Si absent, toutes les connectees.

    if (otaInProgress) return res.status(409).json({ error: 'OTA deja en cours' });

    let targetVersion = version;
    if (!targetVersion) {
        const latest = await fetchLatestRelease();
        if (!latest) return res.status(404).json({ error: 'Aucune release trouvee' });
        targetVersion = latest.version;
    }

    let build = getBuild(targetVersion);
    if (!build) {
        return res.status(404).json({ error: `Build v${targetVersion} pas disponible. Compilez d'abord.` });
    }

    const firmware = fs.readFileSync(path.join(BUILDS_DIR, `${targetVersion}.bin`));

    // Determiner les cibles
    const targetIds = targets || Array.from(scooters.keys());
    const connected = targetIds.filter(id => {
        const s = scooters.get(id);
        return s && s.ws.readyState === WebSocket.OPEN;
    });

    if (connected.length === 0) {
        return res.status(503).json({ error: 'Aucune trottinette connectee dans la selection' });
    }

    res.json({
        ok: true,
        message: `Flash v${targetVersion} lance pour ${connected.length} unite(s)`,
        targets: connected
    });

    // Flash sequentiel (une a la fois pour eviter surcharge reseau)
    (async () => {
        for (const id of connected) {
            try {
                console.log(`[ota-fleet] flash ${id} → v${targetVersion}`);
                broadcastSSE({ type: 'ota_fleet_progress', scooterId: id, status: 'flashing' });
                await flashFirmware(firmware, id);
                broadcastSSE({ type: 'ota_fleet_progress', scooterId: id, status: 'success' });
            } catch (err) {
                console.error(`[ota-fleet] echec ${id}: ${err.message}`);
                broadcastSSE({ type: 'ota_fleet_progress', scooterId: id, status: 'error', error: err.message });
            }
            // Attendre 5s entre chaque flash (reconnexion ESP32)
            await new Promise(r => setTimeout(r, 5000));
        }
        broadcastSSE({ type: 'ota_fleet_progress', status: 'complete' });
    })();
});
```

### 3.2 Verification de version de la flotte

```js
// GET /api/fleet/status — vue d'ensemble de la flotte avec versions
app.get('/api/fleet/status', requireAuth, (req, res) => {
    const list = getScooterList();
    const builds = getLocalBuilds();
    const latestBuild = builds.length > 0 ? builds[0].version : null;

    res.json({
        scooters: list.map(s => ({
            ...s,
            needsUpdate: latestBuild && s.fwVersion && s.fwVersion !== latestBuild
        })),
        latestBuild,
        availableBuilds: builds.map(b => b.version)
    });
});
```

---

## 4. Dashboard web — modifications UI

### 4.1 Onglet/section "Flotte" dans `index.html`

Ajouter un nouvel onglet dans la navigation existante avec :

**Liste des trottinettes** (tableau) :
| Colonne | Source |
|---------|--------|
| Nom | `scooter.name` |
| MAC | `scooter.id` |
| Statut | Badge : NOUVEAU / PROVISIONNE / ACTIF / MAINTENANCE |
| Firmware | `scooter.fwVersion` (rouge si != derniere build) |
| Connectee | Indicateur vert/rouge |
| Verrouillee | Icone cadenas + toggle |
| Batterie | `scooter.telemetry.voltage` |
| Vitesse | `scooter.telemetry.speed` |
| Actions | Boutons : Provisionner / Renommer / Lock-Unlock / Flash OTA |

**Panneau de provisionnement** (modal ou inline) :
- Apparait quand on clique "Provisionner" sur une unite `status=new`
- Champs : Nom de l'unite
- Bouton : "Provisionner et verrouiller"

**Panneau OTA flotte** :
- Dropdown version disponible
- Checkboxes pour selectionner les unites cibles
- Bouton "Mettre a jour la selection" / "Mettre a jour toute la flotte"
- Barre de progression par unite

### 4.2 Bouton Lock/Unlock dans le header

Pour la trottinette selectionnee, ajouter un bouton cadenas dans le header existant, a cote des indicateurs :

```html
<button id="lock-toggle-btn" onclick="toggleLock()" title="Verrouiller/Deverrouiller"
    style="background:none;border:none;cursor:pointer;font-size:18px;padding:4px">
    🔒
</button>
```

```js
async function toggleLock() {
    if (!selectedScooterIdLocal) return;
    const scooter = currentScooterList.find(s => s.id === selectedScooterIdLocal);
    if (!scooter) return;
    const action = scooter.locked ? 'unlock' : 'lock';
    await authFetch(`/api/scooters/${encodeURIComponent(selectedScooterIdLocal)}/${action}`, {
        method: 'POST'
    });
}
```

### 4.3 Indication visuelle du verrouillage

- Quand la trottinette est verrouillee : barre de titre rouge, icone cadenas ferme, commandes moteur grises
- Quand deverrouillee : barre de titre normale, icone cadenas ouvert

---

## 5. Securite

### 5.1 Auth sur les endpoints sensibles

Tous les nouveaux endpoints utilisent `requireAuth` (JWT existant). Points critiques :

- `POST /api/scooters/:id/unlock` — **requiert auth** (sinon n'importe qui deverrouille)
- `POST /api/scooters/:id/lock` — **requiert auth**
- `POST /api/scooters/:id/provision` — **requiert auth**
- `POST /api/ota/fleet` — **requiert auth**

### 5.2 Auth sur le WebSocket ESP32

Actuellement `/ws-esp32` n'a pas d'authentification. Pour la flotte, ajouter un token de pre-provisionnement :

```js
// Dans le handler 'hello' :
if (msg.type === 'hello') {
    const token = msg.token || '';
    const FLEET_TOKEN = process.env.FLEET_TOKEN || '';
    if (FLEET_TOKEN && token !== FLEET_TOKEN) {
        console.log(`[esp32-ws] token invalide — rejet`);
        ws.close(4001, 'Token invalide');
        return;
    }
    // ... enregistrement normal
}
```

Ajouter `FLEET_TOKEN` dans `config.h` du firmware :

```cpp
#define FLEET_TOKEN "mon_token_secret"
```

Et l'envoyer dans le message `hello` :

```cpp
snprintf(hello, sizeof(hello),
    "{\"type\":\"hello\",\"version\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"locked\":true,\"token\":\"%s\"}",
    FW_VERSION, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], FLEET_TOKEN);
```

### 5.3 Protection anti-vol firmware

Le verrouillage est en RAM. Si quelqu'un reboot l'ESP32, il revient a `locked=true`. Mais si quelqu'un re-flashe le firmware avec `locked=false` par defaut, le verrou est contourne. Pour contrer :

- Stocker l'etat de lock dans NVS (flash ESP32) avec un HMAC signe par le serveur
- Le firmware verifie le HMAC avant d'accepter un unlock
- Implementation optionnelle (phase 2)

---

## 6. Modifications firmware detaillees

### 6.1 Resume des changements dans `main.cpp`

1. **Variable globale** : `static volatile bool scooterLocked = true;`

2. **Message hello** : ajouter `"locked":true` et `"token":"..."`

3. **Handler WebSocket** (`onWsProxyEvent`) : ajouter handlers pour `lock` et `unlock`

4. **Telemetrie** (`sendTelemetry`) : ajouter `doc["locked"] = scooterLocked;`

5. **Boucle ESC** (`loop()`, section cmd 02H) : bloquer throttle si `scooterLocked == true`

6. **Handler commandes IA** (`onWsProxyEvent`, section `cmd`) : ignorer si `scooterLocked`

Position exacte dans le code existant — le guard de verrouillage se place dans `loop()` a la ligne 599, AVANT le calcul de `targetThrottle` :

```cpp
// ligne ~608, apres "bool aiActive = ..."
if (scooterLocked) {
    currentThrottle = FTESC_THROTTLE_NEUTRAL;
    brakeLightOn = false;
    // skip tout le calcul manette/IA
} else if (!aiActive) {
    // ... code existant manette physique ...
}
```

### 6.2 Resume des changements dans `config.h.example`

```cpp
// ── Flotte ───────────────────────────────────────────────────────────────────
#define FLEET_TOKEN      ""          // token d'authentification vers le proxy
// FW_VERSION est defini automatiquement par le build system
```

---

## 7. Resume des modifications proxy.js

### Nouveaux endpoints

| Methode | Route | Auth | Description |
|---------|-------|------|-------------|
| `POST` | `/api/scooters/:id/lock` | JWT | Verrouiller une trottinette |
| `POST` | `/api/scooters/:id/unlock` | JWT | Deverrouiller une trottinette |
| `POST` | `/api/scooters/:id/provision` | JWT | Provisionner une nouvelle unite |
| `POST` | `/api/scooters/:id/status` | JWT | Changer le statut |
| `POST` | `/api/ota/fleet` | JWT | Flash OTA groupé |
| `GET` | `/api/fleet/status` | JWT | Vue d'ensemble flotte + versions |

### Modifications existantes

| Element | Modification |
|---------|-------------|
| Modele `scooters.set()` | Ajouter `locked`, `status`, `provisionedAt` |
| `getScooterList()` | Inclure `locked`, `status`, `provisionedAt` |
| `sendScooterCmd()` | Bloquer si `selected.locked` |
| Handler `hello` | Restaurer metadonnees depuis `fleet.json` |
| Nouveau fichier `fleet.json` | Persistance nom/status/provisionnement |

---

## 8. Ordre d'implementation recommande

### Phase 1 — Lock/Unlock (priorite haute)
1. Firmware : ajouter `scooterLocked`, handlers `lock`/`unlock`, guard dans `loop()`
2. Proxy : endpoints `/api/scooters/:id/lock` et `/unlock`
3. Dashboard : bouton lock/unlock dans le header + indication visuelle
4. Telemetrie : inclure `locked` dans les donnees envoyees

### Phase 2 — Provisionnement web
5. Proxy : endpoint `/api/scooters/:id/provision`, persistance `fleet.json`
6. Dashboard : tableau de flotte, modal de provisionnement
7. Proxy : restauration des metadonnees au `hello`

### Phase 3 — OTA de flotte
8. Proxy : endpoint `/api/ota/fleet` (flash sequentiel)
9. Dashboard : UI de selection + progression par unite
10. Proxy : `/api/fleet/status` pour vue d'ensemble

### Phase 4 — Securite avancee (optionnel)
11. Token d'authentification ESP32 (`FLEET_TOKEN`)
12. NVS + HMAC pour anti-contournement du lock
13. Migration `fleet.json` → SQLite si > 50 unites

---

## 9. Impact sur les fichiers existants

| Fichier | Type de modification |
|---------|---------------------|
| `firmware/src/main.cpp` | Ajouter ~40 lignes (variable lock, handlers, guards) |
| `firmware/include/config.h.example` | Ajouter 2 defines (FLEET_TOKEN) |
| `proxy.js` | Ajouter ~120 lignes (6 endpoints, persistance, modele enrichi) |
| `index.html` | Ajouter ~200 lignes (onglet flotte, bouton lock, UI OTA fleet) |
| `fleet.json` (NOUVEAU) | Fichier de persistance auto-genere |
| `.gitignore` | Ajouter `fleet.json` |
| `.env` | Ajouter `FLEET_TOKEN` (optionnel) |
