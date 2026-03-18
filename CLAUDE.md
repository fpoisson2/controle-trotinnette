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
- **Dashboard** (`index.html`) : telemetrie temps reel, controle manuel, carte Leaflet, debug audio
- **Docker** (`docker-compose.yml`, `Dockerfile`) : proxy + tunnel Cloudflare

## Fichiers cles

| Fichier | Role |
|---------|------|
| `firmware/src/main.cpp` | Programme principal ESP32 (setup/loop, WiFi, WebSocket, ESC, ADC, PTT) |
| `firmware/include/config.h` | Pins, seuils ADC, parametres reseau, gains audio |
| `firmware/include/audio.h` | Capture I2S MEMS + lecture DAC via timer hardware (ring buffer) |
| `firmware/include/flipsky.h` | Pilote UART natif pour ESC Flipsky FT85BS (protocole FTESC V1.6) |
| `firmware/platformio.ini` | Environnements PlatformIO (USB, OTA local, OTA proxy) |
| `proxy.js` | Serveur Express + WebSocket (ESP32, OpenAI, debug audio) |
| `index.html` | SPA dashboard (telemetrie, carte, controle vocal/manuel) |
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

```bash
# Build et deploiement
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
- Binaire : audio PCM16 16kHz (chunks de 4x512 samples = 128ms)
- Texte JSON : `{"type":"telemetry", "speed":..., "voltage":..., ...}`

**Proxy vers ESP32** :
- `{"type":"audio", "data":"<base64 PCM16 24kHz>"}` -- reponse vocale IA
- `{"type":"cmd", "action":"avancer|freiner|arreter|vitesse_*", "intensity":0.0-1.0}` -- commande moteur
- `{"type":"ota_begin", "size":N}` + chunks binaires + `{"type":"ota_end"}` -- mise a jour firmware

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
