# Plan — Configuration vocale/web du firmware + OTA automatique

## Concept

Permettre à un administrateur de modifier les paramètres du firmware (`config.h`) depuis le dashboard web ou par commande vocale, puis de recompiler et flasher automatiquement — sans jamais toucher un IDE.

## Problème actuel

Les paramètres firmware (WiFi SSID, seuils ADC, gains audio, timeouts, pins, etc.) sont des `#define` dans `config.h`. Pour les changer, il faut :
1. Éditer le fichier manuellement
2. Recompiler avec PlatformIO
3. Flasher via USB ou OTA

## Solution : config.h dynamique + build + OTA en un clic

### Architecture

```
Dashboard web / Commande vocale
        │
        ▼
POST /api/config { "WIFI_SSID": "MonReseau", "MIC_GAIN": 16, ... }
        │
        ▼
Proxy : écrire config.h → compiler avec PIO → flasher via OTA WS
        │
        ▼
ESP32 : reçoit le firmware, redémarre avec la nouvelle config
```

## Étape 1 — Endpoint de configuration

### `GET /api/config` — Lire la config actuelle

Le proxy lit `firmware/include/config.h`, parse les `#define` et retourne un JSON :

```json
{
  "FW_VERSION": { "value": "1.5.0", "type": "string", "category": "system" },
  "WIFI_SSID": { "value": "TrottiNet", "type": "string", "category": "wifi" },
  "MIC_GAIN": { "value": 12, "type": "int", "category": "audio" },
  "FAKE_TELEMETRY": { "value": true, "type": "bool", "category": "dev" },
  "BATT_VOLTAGE_MIN": { "value": 33.0, "type": "float", "category": "battery" },
  ...
}
```

Parser regex : `#define\s+(\w+)\s+(.+?)(?:\s*\/\/.*)?$`

### `POST /api/config` — Modifier et flasher

Body :
```json
{
  "changes": {
    "WIFI_SSID": "\"NouveauReseau\"",
    "MIC_GAIN": "16",
    "FAKE_TELEMETRY": "false"
  },
  "flash": true,
  "target": "AA:BB:CC:DD:EE:01"
}
```

Le proxy :
1. Lit `config.h` actuel
2. Applique les modifications (remplacement regex des `#define`)
3. Si `flash: true` :
   a. Compile avec PIO (`pio run -e esp32dev`)
   b. Envoie le `.bin` via OTA WebSocket à la trottinette ciblée
   c. Progression via SSE
4. Sauvegarde le `config.h` modifié

### `POST /api/config/validate` — Valider sans flasher

Compile le firmware et retourne succès/erreur sans flasher.
Utile pour vérifier qu'un changement ne casse pas la compilation.

## Étape 2 — Interface web (panneau Config dans le dashboard)

### Formulaire de configuration

Organisé par catégories (onglets ou accordéon) :

- **WiFi** : SSID, mot de passe, mode Enterprise, EAP creds
- **Audio** : gain micro, gain volume, sample rates
- **Moteur** : deadzones throttle/frein, ADC ranges, pole pairs
- **Écran** : activé/désactivé, refresh rate, auto-return delay
- **Batterie** : tension min/max
- **Veille** : timeouts idle léger/profond/sleep, heartbeat
- **LTE** : activé, APN, timeout
- **LoRa** : activé, intervalle envoi
- **Développement** : FAKE_TELEMETRY, AUDIO_LOOPBACK, debug level

Chaque paramètre affiche :
- Nom du `#define`
- Valeur actuelle (éditable)
- Type (string/int/float/bool)
- Description (extraite du commentaire dans config.h)

Boutons :
- **Valider** → `POST /api/config/validate` (compile uniquement)
- **Appliquer et flasher** → `POST /api/config { flash: true }`
- **Réinitialiser** → recharger les valeurs actuelles

### Progression

Barre de progression SSE pendant le build + flash :
1. "Écriture config.h..." (instant)
2. "Compilation en cours..." (30-60s)
3. "Flash OTA..." (10-20s avec progression %)
4. "Redémarrage ESP32..." (5s)
5. "Vérifié — nouvelle version en ligne"

## Étape 3 — Commande vocale

Ajouter un outil OpenAI Realtime :

```json
{
  "type": "function",
  "name": "modifier_config",
  "description": "Modifier un paramètre du firmware de la trottinette et flasher la mise à jour",
  "parameters": {
    "type": "object",
    "properties": {
      "param": {
        "type": "string",
        "description": "Nom du paramètre (ex: MIC_GAIN, WIFI_SSID, FAKE_TELEMETRY)"
      },
      "value": {
        "type": "string",
        "description": "Nouvelle valeur"
      },
      "flash": {
        "type": "boolean",
        "description": "true = compiler et flasher immédiatement"
      }
    },
    "required": ["param", "value"]
  }
}
```

Exemples de commandes vocales :
- "Augmente le gain du micro à 16" → `modifier_config(MIC_GAIN, 16, true)`
- "Désactive la fausse télémétrie" → `modifier_config(FAKE_TELEMETRY, false, true)`
- "Change le WiFi pour MonReseau" → `modifier_config(WIFI_SSID, "MonReseau", true)`
- "Mets la vitesse max à 15 km/h" → traduction en speed_limit dans la BD, pas firmware

L'IA confirme vocalement : "Gain du micro changé à 16. Compilation en cours... Flashé avec succès."

## Étape 4 — Sécurité

- **Admin uniquement** : `requireRole('admin')` sur tous les endpoints config
- **Audit log** : chaque modification est journalisée dans `audit_log`
- **Validation des valeurs** :
  - Pins GPIO : vérifier qu'ils sont dans les ranges valides ESP32
  - Gains : limiter à des ranges raisonnables (ex: MIC_GAIN 1-50)
  - Strings : échapper les guillemets, limiter la longueur
  - Booléens : accepter uniquement true/false
- **Backup** : copier `config.h` avant chaque modification (max 10 backups)
- **Rollback** : si le flash échoue ou la trottinette ne revient pas en ligne après 2 min, restaurer la config précédente et proposer un re-flash

## Étape 5 — Paramètres runtime (sans recompilation)

Certains paramètres peuvent être changés sans recompilation, envoyés via WebSocket :

```json
{"type": "runtime_config", "param": "TELEMETRY_MS", "value": 1000}
```

Le firmware stocke ces overrides en RAM (perdus au reboot) :

- `TELEMETRY_MS` → intervalle de télémétrie
- `AI_CMD_PRIORITY_MS` → durée priorité IA
- `OLED_REFRESH_MS` → rafraîchissement écran
- `currentGear` → vitesse (déjà implémenté via cmd)

Pour les rendre persistants → NVS (Non-Volatile Storage) ESP32.

## Impact mémoire firmware

| Ajout | RAM | Flash |
|-------|-----|-------|
| NVS runtime config | ~2 Ko | ~4 Ko |
| Total firmware actuel | 103 Ko | 1052 Ko |
| **Marge restante** | **224 Ko** | **914 Ko** |

## Phases d'implémentation

1. **Phase 1** : `GET /api/config` + `POST /api/config` (proxy, pas de UI)
2. **Phase 2** : Panneau Config dans le dashboard (formulaire web)
3. **Phase 3** : Outil vocal `modifier_config` dans OpenAI
4. **Phase 4** : Paramètres runtime via WebSocket + NVS
5. **Phase 5** : Validation, rollback, audit

## Prérequis

- PlatformIO installé dans le conteneur Docker (déjà fait)
- `config.h` monté en volume read-write (pas read-only)
- Connexion WebSocket ESP32 active pour le flash OTA
