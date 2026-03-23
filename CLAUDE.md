# CLAUDE.md -- Trottinette Intelligente

Trottinette electrique controlee par la voix via ESP32 + OpenAI Realtime API.

## Architecture

```
[Micro I2S MEMS] → ESP32 (firmware C++) → WebSocket/WSS → Proxy Node.js → OpenAI Realtime API
                                                          ↕                ↕
                                                    Dashboard web     Commandes vocales
                                                    (index.html)      (tool calls)
                                                          ↕
ESP32 ← commandes (cmd JSON) ← Proxy ← OpenAI ← reconnaissance vocale
  ↓
[Flipsky ESC via UART] → moteur
```

- **ESP32 firmware** (`firmware/`) : capture audio I2S, manette/frein ADC, controle ESC Flipsky UART, WebSocket client vers proxy
- **Proxy Node.js** (`proxy.js`) : pont WebSocket entre ESP32 et OpenAI Realtime, auth JWT/bcrypt, SSE vers dashboard, OTA distant
- **Dashboard** (`index.html`, `css/dashboard.css`, `js/dashboard.js`) : telemetrie temps reel, controle manuel, carte Leaflet, debug audio
- **Docker** (`docker-compose.yml`, `Dockerfile`) : proxy + tunnel Cloudflare

## Fichiers cles

| Fichier | Role |
|---------|------|
| `firmware/src/main.cpp` | Programme principal ESP32 (setup/loop, WiFi, WebSocket, ESC, ADC, PTT) |
| `firmware/include/config.h` | Pins, seuils ADC, parametres reseau, gains audio |
| `firmware/include/audio.h` | Capture I2S MEMS + lecture DAC via timer hardware (ring buffer) |
| `firmware/include/flipsky.h` | Pilote UART natif pour ESC Flipsky FT85BS (protocole FTESC V1.6) |
| `firmware/include/sleep.h` | Gestion d'energie multi-niveaux (actif, veille legere/profonde, deep sleep) |
| `firmware/include/connectivity.h` | Gestionnaire WiFi/LTE avec basculement automatique |
| `firmware/include/modem.h` | Init/connect/disconnect modem SIM7670E, GPS, signal |
| `firmware/include/WebSocketsNetworkClientSecure.h` | Bridge WiFi/LTE pour lib WebSockets (patch custom) |
| `firmware/lib/TinyGSM/src/TinyGsmClientA76xxSSL.h` | Driver TinyGSM patche pour A76xx SSL (polling, URC, mux) |
| `firmware/platformio.ini` | Environnements PlatformIO (USB, OTA local, OTA proxy) |
| `proxy.js` | Serveur Express + WebSocket (ESP32, OpenAI, debug audio) |
| `index.html` | Structure HTML du dashboard |
| `css/dashboard.css` | Styles du dashboard (themes, responsive, composants) |
| `js/dashboard.js` | Logique client (auth, SSE, telemetrie, carte, controle, touch) |
| `docker-compose.yml` | Services proxy + cloudflared |
| `.env` | Secrets (OPENAI_API_KEY, AUTH_PASSWORD, JWT_SECRET, TUNNEL_TOKEN) -- **ne jamais committer** |

## Commandes

### Firmware (depuis `firmware/`)

```bash
# Flash USB
pio run -e esp32dev --target upload

# Flash OTA via proxy (a distance, tunnel Cloudflare)
AUTH_PASSWORD=monmotdepasse pio run -e esp32dev-proxy-ota --target upload

# Flash OTA local (meme reseau WiFi)
pio run -e esp32dev-ota --target upload

# Moniteur serie
pio run -e esp32dev --target monitor

# Build seul (verification compilation)
pio run -e esp32dev
```

### Proxy (depuis la racine)

**IMPORTANT** : apres toute modification de fichiers applicatifs, copier les fichiers modifies dans le conteneur Docker en cours d'execution (pas de rebuild). Si `proxy.js` est modifie, redemarrer le conteneur apres la copie.

```bash
# Copie rapide des fichiers modifies dans le conteneur (pas de rebuild)
docker cp <fichier> controle-trotinnette-proxy-1:/app/<fichier>
# Exemples :
docker cp index.html controle-trotinnette-proxy-1:/app/index.html
docker cp js/dashboard.js controle-trotinnette-proxy-1:/app/js/dashboard.js
docker cp css/dashboard.css controle-trotinnette-proxy-1:/app/css/dashboard.css
docker cp proxy.js controle-trotinnette-proxy-1:/app/proxy.js

# Redemarrer seulement si proxy.js est modifie (fichiers statiques servis a chaud)
docker compose restart proxy

# Build complet (seulement si les dependances npm changent ou nouveau fichier)
docker compose build --no-cache proxy && docker compose up -d --force-recreate proxy

# Logs
docker compose logs -f proxy

# Dev local (sans Docker)
npm install && node proxy.js
```

## Hardware -- LilyGO T-A7670E

| Fonction | GPIO | Notes |
|----------|------|-------|
| Micro BCLK | 18 | I2S clock |
| Micro LRCLK | 19 | I2S word select |
| Micro DATA | 34 | Input-only, OK |
| Haut-parleur DAC | 25 | DAC1 natif |
| Throttle ADC | 32 | Hall sensor, pull-down interne |
| Frein ADC | 39 | Hall sensor |
| Bouton PTT | 2 | Actif LOW, pull-up interne |
| ESC RX | 14 | UART2 |
| ESC TX | 13 | UART2 |

GPIO 4, 16, 17, 35, 36 sont reserves par le modem/PSRAM du T-A7670E.

## Architecture firmware (FreeRTOS)

- **Core 0** (`loop()`) : WiFi, OTA, WebSocket serveur (telemetrie locale port 8080), lecture ADC manette/frein, commandes ESC a 20 Hz
- **Core 1** (`taskCapture`) : WebSocket client proxy (`wsProxy.loop()`), capture audio I2S, envoi PCM16 binaire, reception reponses IA + audio

La telemetrie est ecrite par Core 0 dans un buffer partage et envoyee par Core 1 via `wsProxy`.

## Protocole WebSocket ESP32 <-> Proxy

**ESP32 vers proxy** (`/ws-esp32`) :
- Binaire : audio PCM16 16kHz (chunks de 4x512 samples = 128ms) -- mode WiFi streaming
- Texte JSON : `{"type":"voice_blob", "size":N}` + chunks binaires 1KB -- mode LTE chained
- Texte JSON : `{"type":"telemetry", "speed":..., "voltage":..., "locked":bool, ...}`
- Texte JSON : `{"type":"lock_ack", "locked":bool}` -- confirmation verrouillage

**Proxy vers ESP32** :
- Binaire : chunks PCM8 8kHz unsigned 1KB -- reponse vocale IA (mode LTE chained)
- `{"type":"audio", "data":"<base64 PCM16 24kHz>"}` -- reponse vocale IA (mode WiFi streaming)
- `{"type":"cmd", "action":"avancer|freiner|arreter|vitesse_*", "intensity":0.0-1.0}` -- commande moteur (bloquee si locked)
- `{"type":"lock", "locked":true|false}` -- verrouiller/deverrouiller la trottinette
- `{"type":"ota_begin", "size":N}` + chunks binaires + `{"type":"ota_end"}` -- mise a jour firmware

## Pipeline vocal LTE (mode chained)

En LTE, le streaming Realtime API n'est pas viable (debit AT trop lent). Le mode "chained" utilise l'API Chat Completions avec audio :

1. **PTT enfonce** : ESP32 accumule le PCM16 16kHz dans un buffer (alloue au boot, ~160KB)
2. **PTT relache** : ESP32 envoie `voice_blob` JSON + chunks binaires 1KB/75ms au proxy
3. **Proxy** : reassemble les chunks, ajoute header WAV, appelle `gpt-audio-1.5` (Chat Completions)
4. **Reponse** : proxy recoit PCM16 24kHz, resample en PCM8 8kHz, envoie en chunks binaires 1KB/5ms
5. **ESP32** : recoit les chunks binaires (WStype_BIN), pre-buffer 0.5s, joue via DAC

**Contraintes debit AT** : chaque chunk = 1 commande AT+CCHSEND/CCHRECV (~100-500ms). Upload ~56KB en ~10s, download ~13KB en ~6.5s. Le buffer RX TinyGSM est 4096 bytes, polling sslAvailable toutes les 50ms.

**Reconnexion LTE** : silence timeout 60s, reset SSL (CCHSTOP+CCHSTART) toutes les 10s si deconnecte, watchdog GPRS toutes les 30s, reboot apres 10 min d'echecs.

## Verrouillage et courses (libre-service)

- La trottinette est **verrouillee par defaut** au boot et a la connexion
- Quand `locked=true` : throttle force au neutre (manette + IA + commandes manuelles bloques)
- Le deverrouillage se fait via `POST /api/scooters/lock` ou `POST /api/rides/start`
- Une **course** (ride session) represente une utilisation : debut → rouler → fin
- `POST /api/rides/start` : deverrouille + cree la course
- `POST /api/rides/end` : termine la course + reverrouille automatiquement
- `POST /api/scooters/lock` : verrouillage/deverrouillage manuel (admin/urgence)
- Securite multi-couche : proxy bloque les cmds + firmware force neutre si locked
- Deconnexion ESP32 termine automatiquement toute course active

## Variables d'environnement (.env)

```
OPENAI_API_KEY=sk-...
AUTH_USERNAME=admin
AUTH_PASSWORD=...
AUTH_PASSWORD_HASH=...    # genere automatiquement si absent
JWT_SECRET=...
TUNNEL_TOKEN=...          # token tunnel Cloudflare
PROXY_PORT=3000
USE_LOCAL_AI=false        # true = pipeline Whisper+Ollama+Piper au lieu d'OpenAI
```

## Conventions

- Code et commentaires en francais
- Protocole ESC : Flipsky FTESC UART V1.6 (pas VESC standard)
- Commande ESC via `ftesc_control()` (cmd 02H) : throttle 0-1023, 512=neutre
- Priorite IA sur manette physique pendant 8 secondes apres une commande vocale (`AI_CMD_PRIORITY_MS`)
- Anti-rebond PTT : 300ms pour filtrer les pulses USB-serie
- Calibration throttle au boot : mediane de 200 echantillons ADC
