# Firmware ESP32 — Trottinette Intelligente

## Câblage

### Microphone I2S MEMS (Adafruit SPH0645 / ICS-43434)

| Broche micro | GPIO ESP32 | Description       |
|--------------|-----------|-------------------|
| 3V           | 3.3V      | Alimentation      |
| GND          | GND       | Masse             |
| BCLK         | GPIO 27   | Bit clock         |
| LRCLK        | GPIO 26   | Word select       |
| DOUT         | GPIO 33   | Données audio     |
| SEL          | GND       | Canal gauche      |

### Haut-parleur (Adafruit STEMMA Speaker)

| Broche speaker | GPIO ESP32 | Description              |
|----------------|-----------|--------------------------|
| VCC            | 5V ou 3.3V| Alimentation             |
| GND            | GND       | Masse                    |
| Signal         | GPIO 25   | Audio DAC natif (DAC1)   |

### Bouton vocal

| Fonction      | GPIO ESP32 | Notes                                      |
|---------------|-----------|---------------------------------------------|
| Toggle micro  | GPIO 34   | Input-only — pull-up externe 10kΩ requis    |

> GPIO34 n'a pas de pull-up interne. Brancher une résistance 10kΩ entre GPIO34 et 3.3V.
> Câblage : 3.3V → 10kΩ → GPIO34 ; bouton entre GPIO34 et GND.

---

## Schéma de principe

```
ESP32 DevKit
┌─────────────────────────────────┐
│  3.3V ──────────────── VCC  [MIC]│
│  GND  ──────────────── GND  [MIC]│
│  GPIO27 ────────────── BCLK [MIC]│
│  GPIO26 ────────────── LRCLK[MIC]│
│  GPIO33 ────────────── DOUT [MIC]│
│  GND  ──────────────── SEL  [MIC]│
│                                 │
│  5V/3.3V ────────────── VCC [SPK]│
│  GND  ──────────────── GND  [SPK]│
│  GPIO25 (DAC1) ──────── SIG [SPK]│
│                                 │
│  GPIO0 (BOOT btn) ── toggle micro│
└─────────────────────────────────┘
```

---

## Configuration

Copier `include/config.h.example` vers `include/config.h` et remplir :

```c
// Réseau personnel (maison / hotspot)
#define WIFI_ENTERPRISE  false
#define WIFI_SSID        "MonReseau"
#define WIFI_PASSWORD    "monmotdepasse"

// Réseau entreprise / cégep (EAP-PEAP)
#define WIFI_ENTERPRISE  true
#define WIFI_SSID        "NomDuReseau"
#define EAP_USERNAME     "mon_identifiant"
#define EAP_PASSWORD     "mon_mot_de_passe"
```

Le proxy est accessible à `trottinette.ve2fpd.com` (HTTPS, port 443).

---

## Compilation et flash

```bash
cd firmware
pio run --target upload
```

Monitor série :

```bash
pio device monitor --baud 115200
```

---

## Fonctionnement

1. Au démarrage, l'ESP32 se connecte au WiFi et s'enregistre sous `trottinette.local`
2. **Bouton BOOT (GPIO0)** : appui court = toggle microphone ON/OFF
3. Quand le micro est actif : l'audio est streamé en continu vers `/audio` sur le proxy
4. Le modèle IA détecte automatiquement les tours de parole (`semantic_vad`)
5. L'audio de réponse arrive via SSE `/stream` et est joué sur le haut-parleur (DAC GPIO25)
6. Les commandes envoyées par le proxy arrivent via WebSocket sur le port 8080

Dans le `.env` du proxy, définir :
```
SCOOTER_WS=ws://trottinette.local:8080
```
