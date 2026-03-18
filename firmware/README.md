# Firmware ESP32 — Trottinette Intelligente

## Pinout complet — LilyGO T-A7670E

> Configuration active dans `include/config.h`.

### Tableau de câblage

#### ESC Flipsky FT85BS (UART2)

| Signal | GPIO | Fil ESC | Notes |
|--------|------|---------|-------|
| TX     | 13   | RX ESC  | UART2 |
| RX     | 14   | TX ESC  | UART2 |
| GND    | GND  | GND ESC | Masse commune |

> Vitesse : 115200 baud, 8N1. Mode ESC : **Input Signal Type = UART**.

---

#### Gâchette d'accélérateur (hall sensor)

| Signal | GPIO | Fil capteur | Notes |
|--------|------|-------------|-------|
| Signal | **32** | Vert | ADC1_CH4 — pull-down interne activé |
| 3.3V   | 3.3V   | Rouge | Alimentation capteur |
| GND    | GND    | Noir  | Masse |

> Plage ADC : repos ≈ 930–960, plein gaz ≈ 2800. Auto-calibration au démarrage.
> ⚠️ Ne pas brancher sur GPIO 36 (SOLAR_IN) ni GPIO 35 (VBAT) — tension parasite.

---

#### Levier de frein (hall sensor)

| Signal | GPIO | Fil capteur | Notes |
|--------|------|-------------|-------|
| Signal | **39** | Vert | ADC1_CH3 — input-only |
| 3.3V   | 3.3V   | Rouge | Alimentation capteur |
| GND    | GND    | Noir  | Masse |

> Plage ADC : repos ≈ 850–900, frein à fond ≈ 3500. Auto-calibration au démarrage.

---

#### Boutons (actif LOW, pull-up interne)

| Bouton         | GPIO | Fonction                                  |
|----------------|------|-------------------------------------------|
| Gear-F (PTT)   | **22** | Push-to-talk assistant vocal (tenir = ON) |
| Gear-L         | **21** | Vitesse lente (limite 33 %)               |
| Gear-H         | **23** | Vitesse haute (plein régime)              |

> Câblage : bouton entre GPIO et GND. Pull-up interne activé.
> Gear-M supprimé — GPIO 22 réaffecté au PTT.

---

#### Microphone I2S MEMS (Adafruit SPH0645 / ICS-43434)

| Broche micro | GPIO | Description |
|-------------|------|-------------|
| 3V          | 3.3V | Alimentation |
| GND         | GND  | Masse |
| BCLK        | **18** | Bit clock |
| LRCLK / WS  | **19** | Word select |
| DOUT        | **34** | Données audio (input-only) |
| SEL         | GND  | Canal gauche |

---

#### Haut-parleur (DAC natif)

| Broche speaker | GPIO | Description |
|----------------|------|-------------|
| VCC            | 3.3V ou 5V | Alimentation |
| GND            | GND        | Masse |
| Signal         | **25** (DAC1) | Audio analogique |

---

### Pins réservés / à éviter sur T-A7670E

| GPIO | Fonction board | Utilisation firmware |
|------|---------------|----------------------|
| 4    | MODEM_PWRKEY  | — |
| 16   | PSRAM (WROVER) | inaccessible |
| 17   | PSRAM (WROVER) | inaccessible |
| 25   | MODEM_DTR     | DAC haut-parleur (DTR non activé = OK) |
| 26   | MODEM_TX      | — |
| 27   | MODEM_RX      | — |
| 33   | MODEM_RING    | — |
| 35   | VBAT monitor  | ne pas utiliser pour ADC |
| 36   | SOLAR_IN      | ne pas utiliser pour ADC |

---

## Configuration (`include/config.h`)

```c
// WiFi personnel (maison / hotspot)
#define WIFI_ENTERPRISE  false
#define WIFI_SSID        "MonReseau"
#define WIFI_PASSWORD    "monmotdepasse"

// WiFi entreprise / cégep (EAP-PEAP/MSCHAPv2)
#define WIFI_ENTERPRISE  true
#define WIFI_SSID        "NomDuReseau"
#define EAP_USERNAME     "identifiant"
#define EAP_PASSWORD     "motdepasse"

// OTA — changer le mot de passe avant déploiement
#define OTA_PASSWORD     "ota1234"
```

---

## Compilation et flash

### Via USB (premier flash ou si OTA indisponible)

```bash
cd firmware
pio run -e esp32dev --target upload
```

### Via WiFi (OTA — l'ESP32 doit être sur le réseau)

```bash
pio run -e esp32dev-ota --target upload
```

> L'ESP32 doit avoir été flashé au moins une fois via USB pour activer l'OTA.
> Mot de passe OTA défini dans `config.h` (`OTA_PASSWORD`).

### Monitor série

```bash
pio device monitor
```

---

## Fonctionnement

1. Au démarrage, l'ESP32 se connecte au WiFi et s'enregistre sous `trottinette.local`
2. L'ADC est auto-calibré sur 100 échantillons (1 s) — throttle et frein au repos
3. **Bouton Gear-F (GPIO 22)** : tenir = microphone actif, relâcher = micro inactif
4. **Boutons Gear-L / Gear-H** : sélection de la limite de vitesse (33 % / 100 %)
5. L'audio est streamé vers le proxy (`wss://trottinette.ve2fpd.com/ws-esp32`)
6. L'IA (OpenAI Realtime) traite la voix et envoie des commandes (`avancer`, `freiner`, etc.)
7. Les commandes IA ont priorité sur la manette pendant 3 secondes (`AI_CMD_PRIORITY_MS`)
8. La télémétrie ESC est envoyée toutes les 500 ms vers l'interface web

### Commandes vocales disponibles

| Ce que tu dis           | Action             |
|-------------------------|--------------------|
| "avance" / "accélère"   | `avancer`          |
| "à fond"                | `avancer` (100 %)  |
| "doucement"             | `avancer` (30 %)   |
| "freine"                | `freiner`          |
| "stop" / "arrête"       | `arreter`          |
| "vitesse lente"         | `vitesse_lente`    |
| "vitesse moyenne"       | `vitesse_moyenne`  |
| "vitesse haute"         | `vitesse_haute`    |
| "marche arrière"        | `marche_arriere`   |
| "quelle est ta vitesse" | lecture télémétrie |

---

## Optionnel — Écran SPI (non implémenté)

Un petit écran SPI (ex. ST7735, ILI9341, SSD1306 SPI) peut être ajouté pour afficher
vitesse, tension batterie, gear actif et état du micro sans avoir besoin de l'interface web.

Broches VSPI disponibles (18/19/23 occupés — utiliser HSPI ou SPI software) :

| Signal | GPIO disponible | Notes |
|--------|----------------|-------|
| MOSI   | libre          | |
| MISO   | libre          | |
| SCK    | libre          | |
| CS     | libre          | |
| DC     | libre          | Data/Command |
| RST    | libre          | Reset |

---

## Proxy

Le proxy Node.js tourne sur `trottinette.ve2fpd.com` (Cloudflare, port 443).

Variables `.env` requises :

```
OPENAI_API_KEY=sk-...
JWT_SECRET=...
AUTH_USERNAME=admin
AUTH_PASSWORD_HASH=...   # bcrypt hash
PROXY_PORT=3000
```
