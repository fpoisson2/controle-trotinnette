# Firmware ESP32 — Trottinette Intelligente

## Pinout complet

> Deux variantes supportées. Sélectionner les bons GPIOs dans `include/config.h`.

### Variante A — ESP32 DevKit standard

### Variante B — LilyGO T-A7670E (LTE intégré)
GPIO 26/27/33 occupés par le modem A7670E, GPIO 4/13/14/15 occupés par SD card.
Remapping automatique dans `config.h` en décommentant les lignes T-A7670E.



```
ESP32 DevKit v1
                        ┌──────────┐
              3.3V ─────┤ 3V3  GND ├───── GND
              EN   ─────┤ EN   D23 ├
              VP   ─────┤ VP   D22 ├
 [THROTTLE ADC] ── GPIO36┤ D36  TX0 ├
 [BRAKE ADC]    ── GPIO39┤ D39  RX0 ├
              GPIO34 ───┤ D34  D21 ├
              GPIO35 ───┤ D35  D19 ├
 [MIC BCLK]  ── GPIO32 ─┤ D32  D18 ├
 [MIC DATA]  ── GPIO33 ─┤ D33   V5 ├────  5V
 [GEAR-R PTT]── GPIO4  ─┤ D25  D17 ├─── GPIO17 [ESC TX]
 [GEAR-L]    ── GPIO13 ─┤ D26  D16 ├─── GPIO16 [ESC RX]
              GPIO12 ───┤ D27   D4 ├
 [MIC LRCLK] ── GPIO14 ─┤ D14   D0 ├
 [GEAR-M]    ── GPIO27 ─┤ D12   D2 ├
 [GEAR-H]    ── GPIO26 ─┤ D13  D15 ├
              GND  ─────┤ GND  GND ├───── GND
              VIN  ─────┤ VIN  3V3 ├────  3.3V
                        └──────────┘
              [SPK DAC] ── GPIO25 (DAC1)
              [MIC BCLK]  ── GPIO27
              [MIC LRCLK] ── GPIO26
              [MIC DATA]  ── GPIO33
```

---

## Tableau de câblage

### ESC Flipsky FT85BS (UART2)

| Signal   | GPIO ESP32 | Fil ESC  | Notes               |
|----------|-----------|----------|---------------------|
| TX       | GPIO 16   | RX ESC   | UART2               |
| RX       | GPIO 17   | TX ESC   | UART2               |
| GND      | GND       | GND ESC  | Masse commune       |

> Vitesse : 115200 baud, 8N1. Mode ESC : **Input Signal Type = UART**.

---

### Manette physique — Gâchette d'accélérateur (hall sensor)

| Signal | GPIO ESP32 | Fil capteur | Notes                              |
|--------|-----------|-------------|------------------------------------|
| Signal | GPIO 36   | Vert        | ADC1 input-only (pas de pull-up)   |
| 3.3V   | 3.3V      | Rouge       | Alimentation capteur               |
| GND    | GND       | Noir        | Masse                              |

> Plage ADC : repos ≈ 930–945, plein gaz ≈ 2800.

---

### Manette physique — Levier de frein (hall sensor)

| Signal | GPIO ESP32 | Fil capteur | Notes                              |
|--------|-----------|-------------|------------------------------------|
| Signal | GPIO 39   | Vert        | ADC1 input-only (pas de pull-up)   |
| 3.3V   | 3.3V      | Rouge       | Alimentation capteur               |
| GND    | GND       | Noir        | Masse                              |

> Plage ADC : repos ≈ 850–900, frein à fond ≈ 3500.

---

### Boutons de vitesse + push-to-talk (actif LOW, pull-up interne)

| Bouton        | GPIO ESP32 | Fonction                                  |
|---------------|-----------|-------------------------------------------|
| Gear-R (rouge)| GPIO 4    | Push-to-talk assistant vocal (tenir = ON) |
| Gear-L        | GPIO 13   | Vitesse lente (limite 33 %)               |
| Gear-M        | GPIO 14   | Vitesse moyenne (limite 66 %)             |
| Gear-H        | GPIO 15   | Vitesse haute (plein régime)              |

> Câblage : bouton entre GPIO et GND. Pull-up interne activé.

---

### Microphone I2S MEMS (Adafruit SPH0645 / ICS-43434)

| Broche micro | GPIO ESP32 | Description   |
|-------------|-----------|---------------|
| 3V          | 3.3V      | Alimentation  |
| GND         | GND       | Masse         |
| BCLK        | GPIO 27   | Bit clock     |
| LRCLK / WS  | GPIO 26   | Word select   |
| DOUT        | GPIO 33   | Données audio |
| SEL         | GND       | Canal gauche  |

---

### Haut-parleur (Adafruit STEMMA Speaker — DAC natif)

| Broche speaker | GPIO ESP32     | Description          |
|----------------|---------------|----------------------|
| VCC            | 3.3V ou 5V    | Alimentation         |
| GND            | GND           | Masse                |
| Signal         | GPIO 25 (DAC1)| Audio analogique     |

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
2. **Bouton Gear-R (GPIO4)** : tenir = microphone actif, relâcher = micro inactif
3. **Boutons Gear-L/M/H** : sélection de la limite de vitesse (33 / 66 / 100 %)
4. L'audio est streamé vers le proxy (`wss://trottinette.ve2fpd.com/ws-esp32`)
5. L'IA (OpenAI Realtime) traite la voix et envoie des commandes (`avancer`, `freiner`, etc.)
6. Les commandes IA ont priorité sur la manette pendant 3 secondes (`AI_CMD_PRIORITY_MS`)
7. La télémétrie ESC est envoyée toutes les 500 ms vers l'interface web

### Commandes vocales disponibles

| Ce que tu dis          | Action              |
|------------------------|---------------------|
| "avance" / "accélère"  | `avancer`           |
| "à fond"               | `avancer` (100 %)   |
| "doucement"            | `avancer` (30 %)    |
| "freine"               | `freiner`           |
| "stop" / "arrête"      | `arreter`           |
| "vitesse lente"        | `vitesse_lente`     |
| "vitesse moyenne"      | `vitesse_moyenne`   |
| "vitesse haute"        | `vitesse_haute`     |
| "marche arrière"       | `marche_arriere`    |
| "quelle est ta vitesse"| lecture télémétrie  |

---

## Optionnel — Écran SPI (non implémenté)

Un petit écran SPI (ex. ST7735, ILI9341, SSD1306 SPI) peut être ajouté pour afficher
vitesse, tension batterie, gear actif et état du micro sans avoir besoin de l'interface web.

Broches SPI disponibles sur l'ESP32 DevKit (HSPI) :

| Signal | GPIO ESP32 | Description        |
|--------|-----------|-------------------|
| MOSI   | GPIO 13   | *conflit Gear-L*  |
| MISO   | GPIO 12   | (lecture écran)   |
| SCK    | GPIO 14   | *conflit Gear-M*  |
| CS     | GPIO 15   | *conflit Gear-H*  |
| DC     | libre     | Data/Command       |
| RST    | libre     | Reset              |

> Les broches HSPI (13/14/15) sont utilisées par les boutons Gear. Si un écran est ajouté,
> il faudra déplacer les boutons sur d'autres GPIO ou utiliser le bus VSPI (GPIO 18/19/23).

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
