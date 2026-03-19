# Trotilou — Trottinettes intelligentes pour le Cegep Limoilou

Trottinettes electriques en libre-service controlees par la voix, deployees sur le campus du Cegep Limoilou a Quebec.

## Architecture

```
                          ┌─────────────────┐
                          │  OpenAI Realtime │
                          │  (voix → commande)│
                          └────────┬────────┘
                                   │ WSS
┌──────────┐    WSS     ┌─────────┴─────────┐     SSE      ┌──────────────┐
│  ESP32   │◄──────────►│   Proxy Node.js   │◄────────────►│  Dashboard   │
│ T-A7670E │            │   (Express + WS)  │              │  (index.html)│
└──────────┘            │                   │              └──────────────┘
  │ I2S micro           │  SQLite (users,   │              ┌──────────────┐
  │ DAC speaker         │   rides, fleet)   │              │ Landing page │
  │ OLED SSD1306        │                   │              │(landing.html)│
  │ ESC Flipsky UART    └───────────────────┘              └──────────────┘
  │ Throttle/Frein ADC        │
  │ LTE SIM7670E              │ Docker + Cloudflare Tunnel
  │ LoRa 915 MHz              │
  └───────────────────────────┘
```

## Demarrage rapide

```bash
# 1. Cloner et configurer
git clone https://github.com/fpoisson2/controle-trotinnette.git
cd controle-trotinnette
cp .env.example .env
# Editer .env : OPENAI_API_KEY, AUTH_PASSWORD, JWT_SECRET

# 2. Demarrer avec Docker
./start.sh          # ou : docker compose up -d --build

# 3. Ouvrir dans le navigateur
# Landing page : http://localhost:3000
# Dashboard :    http://localhost:3000/dashboard
```

Avec tunnel Cloudflare (optionnel) :
```bash
# Ajouter TUNNEL_TOKEN dans .env, puis :
./start.sh    # detecte automatiquement le token et demarre cloudflared
```

## Fonctionnalites

### Controle vocal
- Commandes naturelles en francais : "avance doucement", "freine", "a quelle vitesse on roule ?"
- OpenAI Realtime API (cloud) ou pipeline local Whisper + Ollama + Piper
- Push-to-talk physique (GPIO 2) ou depuis le dashboard

### Libre-service
- Trottinettes verrouillees par defaut au boot
- Courses (rides) : demarrer → rouler → terminer → reverrouillage auto
- Inscription etudiante avec courriel @cegepl.qc.ca
- 3 roles : etudiant, gestionnaire de flotte, administrateur

### Securite triple redondance
- **WiFi** (primaire) → **LTE 4G** (fallback) → **LoRa 915 MHz** (tertiaire)
- Si tous les liens sont perdus : arret moteur immediat + alerte HORS ZONE
- Watchdog avec periode de grace configurable

### Ecran OLED
- SSD1306 128x64 via I2C (GPIO 21/22)
- 5 pages : tableau de bord, trajet, systeme, alertes, musique
- Page speciale HORS ZONE clignotante
- 2 boutons physiques : verrouillage (GPIO 15) + navigation (GPIO 0)

### Lecteur de musique
- 6 melodies RTTTL (Mario, Tetris, Star Wars, Fur Elise, Nokia, Jingle Bells)
- Coexistence avec l'audio IA (pause auto quand l'IA parle)
- Controle par boutons physiques ou commande WebSocket

### Gestion de flotte
- Auto-enregistrement des trottinettes a la connexion WebSocket
- Registre persistant (`fleet.json`)
- Tests QC a distance
- OTA firmware en lot (flash-all)
- Script de provisionnement batch (`tools/provision.py`)

### Economie d'energie
- Machine d'etats : actif → idle leger (2 min) → idle profond (10 min) → deep sleep (30 min)
- Reveil par manette, frein ou timer (heartbeat 5 min)
- Jingle de boot au reveil

## Structure du projet

```
controle-trotinnette/
├── firmware/                    # Code ESP32 (C++, PlatformIO)
│   ├── src/main.cpp             # Programme principal (setup/loop, FreeRTOS)
│   ├── include/
│   │   ├── config.h             # Configuration centralisee (pins, seuils, flags)
│   │   ├── config.h.fleet       # Template config pour deploiement campus
│   │   ├── audio.h              # Capture I2S MEMS + lecture DAC (ring buffer)
│   │   ├── flipsky.h            # Pilote ESC Flipsky UART V1.6
│   │   ├── display.h            # Ecran OLED SSD1306 (U8g2, 5 pages)
│   │   ├── music.h              # Lecteur melodies RTTTL via DAC
│   │   ├── sleep.h              # Deep sleep + machine d'etats energie
│   │   ├── modem.h              # LTE SIM7670E (TinyGSM)
│   │   ├── connectivity.h       # Failover WiFi/LTE
│   │   └── lora.h               # LoRa 915 MHz (protocole binaire)
│   └── platformio.ini           # Environnements PlatformIO (USB, OTA, proxy)
│
├── proxy.js                     # Serveur Express + WebSocket (backend principal)
├── database.js                  # SQLite : users, rides, scooters, geofences, audit
├── auth-routes.js               # Authentification multi-utilisateurs + roles
├── fleet.js                     # Gestion de flotte + persistance fleet.json
│
├── index.html                   # Dashboard SPA (telemetrie, carte, controle vocal)
├── landing.html                 # Page publique Trotilou (hero, tarifs, carte)
│
├── Dockerfile                   # Node.js + PlatformIO + better-sqlite3
├── docker-compose.yml           # Proxy + Cloudflare tunnel (optionnel)
├── start.sh                     # Script de demarrage intelligent
├── .env.example                 # Variables d'environnement
│
├── tools/
│   └── provision.py             # Flash batch USB + QR codes + rapport CSV
│
└── plans/                       # Plans d'architecture (markdown)
    ├── PLAN-LTE.md
    ├── PLAN-TRIPLE-REDONDANCE.md
    ├── PLAN-ECRAN-I2C.md
    ├── PLAN-DEEP-SLEEP.md
    ├── PLAN-MUSIQUE-OLED.md
    ├── PLAN-ROLES-UTILISATEURS.md
    ├── PLAN-DEPLOIEMENT-FLOTTE.md
    ├── PLAN-REFACTOR-FRONTEND-BACKEND.md
    ├── PLAN-UI-REDESIGN.md
    ├── PLAN-LANDING-PAGE.md
    └── PLAN-CONFIG-VOCALE-OTA.md
```

## Hardware

### LilyGO T-A7670E

| Fonction | GPIO | Notes |
|----------|------|-------|
| Micro I2S BCLK | 18 | Horloge I2S |
| Micro I2S LRCLK | 19 | Word select |
| Micro I2S DATA | 34 | Input-only |
| Haut-parleur DAC | 25 | DAC1 natif (musique + voix IA) |
| Throttle (manette) | 32 | ADC, capteur Hall |
| Frein | 39 | ADC, capteur Hall |
| Bouton PTT | 2 | Push-to-talk, actif LOW |
| ESC Flipsky RX | 14 | UART2 |
| ESC Flipsky TX | 13 | UART2 |
| OLED SDA | 21 | I2C |
| OLED SCL | 22 | I2C |
| Bouton verrouillage | 15 | Actif LOW |
| Bouton page ecran | 0 | Bouton BOOT |
| Modem LTE TX | 26 | SIM7670E interne |
| Modem LTE RX | 27 | SIM7670E interne |
| Modem PWRKEY | 4 | Allumage modem |

## Commandes

### Firmware

```bash
cd firmware

# Compiler (verification)
pio run -e esp32dev

# Flash USB
pio run -e esp32dev --target upload

# Flash OTA local (meme reseau WiFi)
pio run -e esp32dev-ota --target upload

# Flash OTA distant (via tunnel Cloudflare)
AUTH_PASSWORD=motdepasse pio run -e esp32dev-proxy-ota --target upload

# Moniteur serie
pio run -e esp32dev --target monitor
```

### Proxy (Docker)

```bash
# Build et demarrage
docker compose up -d --build

# Avec tunnel Cloudflare
docker compose --profile tunnel up -d --build

# Logs
docker compose logs -f proxy

# Dev local (sans Docker)
npm install && node proxy.js
```

### Provisionnement flotte

```bash
# Flash toutes les trottinettes branchees en USB
python tools/provision.py

# Un seul port
python tools/provision.py --port COM3

# Sans recompiler
python tools/provision.py --skip-compile
```

## API

### Publique

| Methode | Route | Description |
|---------|-------|-------------|
| GET | `/` | Landing page |
| GET | `/dashboard` | Dashboard (login requis) |
| GET | `/stream` | SSE telemetrie temps reel |
| POST | `/login` | Authentification legacy |
| POST | `/api/auth/register` | Inscription (@cegepl.qc.ca) |
| POST | `/api/auth/login` | Login multi-utilisateurs |

### Authentifiee (JWT)

| Methode | Route | Description |
|---------|-------|-------------|
| POST | `/cmd` | Commande moteur (avancer, freiner, arreter) |
| GET | `/api/scooters` | Liste trottinettes connectees |
| POST | `/api/scooters/lock` | Verrouiller/deverrouiller |
| POST | `/api/rides/start` | Demarrer une course |
| POST | `/api/rides/end` | Terminer une course |
| GET | `/api/fleet` | Registre complet de la flotte |
| POST | `/api/ota/flash` | Flash OTA une trottinette |
| POST | `/api/ota/flash-all` | Flash OTA toute la flotte |

## Variables d'environnement

| Variable | Requis | Description |
|----------|--------|-------------|
| `OPENAI_API_KEY` | Oui | Cle API OpenAI Realtime |
| `AUTH_PASSWORD` | Oui | Mot de passe admin |
| `JWT_SECRET` | Oui | Secret pour signer les JWT |
| `PROXY_PORT` | Non | Port du serveur (defaut: 3000) |
| `TUNNEL_TOKEN` | Non | Token Cloudflare (active le tunnel) |
| `USE_LOCAL_AI` | Non | `true` = pipeline Whisper+Ollama+Piper |
| `SKIP_EMAIL_VERIFY` | Non | `true` = pas de verification courriel |

## Compilation firmware

Derniere compilation :
```
RAM:   [===       ]  31.6% (103 524 / 327 680 bytes)
Flash: [=====     ]  53.5% (1 052 425 / 1 966 080 bytes)
```

## Licence

MIT

---

Fait avec passion a Quebec — Propulse par ESP32 + OpenAI
