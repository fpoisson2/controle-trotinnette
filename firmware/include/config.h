#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  config.h  —  Configuration centralisée du firmware trottinette intelligente
//
//  Tous les paramètres matériels (pins, seuils, gains) et logiciels
//  (timeouts, intervalles, flags) sont regroupés ici.
//  Les valeurs par défaut correspondent au hardware LilyGO T-A7670E.
// ─────────────────────────────────────────────────────────────────────────────

// ── Version firmware ─────────────────────────────────────────────────────────
#define FW_VERSION  "1.5.0"

// ── Flags de développement ───────────────────────────────────────────────────
// FAKE_TELEMETRY : simule vitesse/tension/GPS (pas besoin d'ESC branché)
#define FAKE_TELEMETRY   false
// AUDIO_LOOPBACK : enregistre pendant PTT, rejoue au relâchement (test micro/haut-parleur)
#define AUDIO_LOOPBACK   false

// ─────────────────────────────────────────────────────────────────────────────
//  WiFi
// ─────────────────────────────────────────────────────────────────────────────
#define WIFI_SSID           "TrottiNet"
#define WIFI_PASSWORD       "changeme123"

// Mode WPA2-Enterprise (EAP-PEAP) — réseau Cégep Limoilou
#define WIFI_ENTERPRISE     0   // 0 = WPA2 personnel, 1 = WPA2-Enterprise
#define EAP_IDENTITY        "trottinette@cegepl.ca"
#define EAP_USERNAME        "trottinette@cegepl.ca"
#define EAP_PASSWORD        "eapPassword"

// Timeout de connexion WiFi au boot (ms)
#define WIFI_TIMEOUT_MS     15000

// ─────────────────────────────────────────────────────────────────────────────
//  Proxy WebSocket (serveur distant via Cloudflare tunnel)
// ─────────────────────────────────────────────────────────────────────────────
#define PROXY_HOST          "www.trotilou.ca"
#define PROXY_PORT          443

// ─────────────────────────────────────────────────────────────────────────────
//  Audio — Microphone I2S MEMS (SPH0645 / ICS-43434)
// ─────────────────────────────────────────────────────────────────────────────
#define MIC_BCLK_PIN        18    // I2S bit clock
#define MIC_LRCLK_PIN       19    // I2S word select (L/R clock)
#define MIC_DATA_PIN        34    // I2S data in (GPIO 34 = input-only, OK)

#define MIC_SAMPLE_RATE     16000   // 16 kHz pour reconnaissance vocale
#define SPK_SAMPLE_RATE     8000    // 8 kHz — proxy resample avant envoi pour LTE

#define MIC_CHUNK_SAMPLES   512     // Samples par chunk I2S (~32 ms à 16 kHz)
#define MIC_GAIN            12      // Gain numérique appliqué au PCM brut
#define VOLUME_GAIN         2.5f    // Gain de volume pour la sortie DAC

// ─────────────────────────────────────────────────────────────────────────────
//  Audio — Haut-parleur DAC
// ─────────────────────────────────────────────────────────────────────────────
#define SPK_DAC_PIN         25    // DAC1 natif ESP32 (GPIO 25)

// ─────────────────────────────────────────────────────────────────────────────
//  Manette (throttle) et frein — ADC
// ─────────────────────────────────────────────────────────────────────────────
#define THROTTLE_ADC_PIN    32    // Capteur Hall manette (ADC1_CH4)
#define BRAKE_ADC_PIN       39    // Capteur Hall frein   (ADC1_CH3, input-only)

#define THROTTLE_ADC_MIN    0     // Valeur ADC au repos (recalibrée au boot)
#define THROTTLE_ADC_MAX    4095  // Valeur ADC pleine course
#define BRAKE_ADC_MIN       0     // Valeur ADC frein au repos
#define BRAKE_ADC_MAX       4095  // Valeur ADC frein pleine course

#define THROTTLE_DEADZONE   100   // Zone morte (hysterèse ON > MIN+100, OFF < MIN+50)
#define BRAKE_DEADZONE      100   // Zone morte frein

// ─────────────────────────────────────────────────────────────────────────────
//  Bouton vocal / PTT (Push-To-Talk)
// ─────────────────────────────────────────────────────────────────────────────
#define VOICE_BTN_PIN       2     // Bouton PTT principal (actif LOW, pull-up interne)
#define GEAR_R_PIN          2     // Alias pour le bouton PTT (même GPIO)

// ─────────────────────────────────────────────────────────────────────────────
//  ESC Flipsky — UART
// ─────────────────────────────────────────────────────────────────────────────
#define ESC_RX_PIN          14    // UART2 RX (ESP32 reçoit de l'ESC)
#define ESC_TX_PIN          13    // UART2 TX (ESP32 envoie vers l'ESC)
#define ESC_BAUD            115200
#define ESC_POLE_PAIRS      15    // Paires de pôles du moteur (conversion RPM→vitesse)

// Les defines FTESC_THROTTLE_* et FTESC_GEAR_* sont dans flipsky.h

// ─────────────────────────────────────────────────────────────────────────────
//  WebSocket serveur local (télémétrie directe sur le réseau WiFi)
// ─────────────────────────────────────────────────────────────────────────────
#define WS_SERVER_PORT      8080

// ─────────────────────────────────────────────────────────────────────────────
//  Télémétrie et commandes IA
// ─────────────────────────────────────────────────────────────────────────────
#define TELEMETRY_MS        500     // Intervalle d'envoi télémétrie (ms)
#define AI_CMD_PRIORITY_MS  8000    // Durée priorité IA sur manette physique (ms)

// ─────────────────────────────────────────────────────────────────────────────
//  OTA (mise à jour sans fil)
// ─────────────────────────────────────────────────────────────────────────────
#define OTA_HOSTNAME        "trottinette"
#define OTA_PASSWORD        "ota1234"

// ─────────────────────────────────────────────────────────────────────────────
//  Modem LTE — SIM7670E (LilyGO T-A7670E intégré)
// ─────────────────────────────────────────────────────────────────────────────
#define LTE_ENABLED         true    // Activer le modem cellulaire

#define MODEM_UART          Serial1
#define MODEM_BAUD          460800   // Cible (upgrade depuis 115200 dans modemInit)
#define MODEM_TX_PIN        26      // ESP32 TX → modem RX
#define MODEM_RX_PIN        27      // ESP32 RX ← modem TX
#define MODEM_PWRKEY_PIN    4       // Pin PWRKEY pour allumer/éteindre le modem
#define MODEM_DTR_PIN       (-1)    // Pin DTR (non utilisé — GPIO 25 réservé au DAC)
#define MODEM_RING_PIN      33      // Pin RING (notification appel/SMS)

// ── Pins spécifiques au board LilyGO T-A7670E ─────────────────────────────
#define BOARD_POWERON_PIN   12      // Alimentation DC-DC du modem (OBLIGATOIRE)
#define MODEM_POWERON_PULSE_WIDTH_MS  200  // Durée du pulse PWRKEY (ms)

// ── APN cellulaire ───────────────────────────────────────────────────────────
#define LTE_APN             "internet.keepgo.com"

// ── Timeouts de connexion ────────────────────────────────────────────────────
#define LTE_CONNECT_TIMEOUT_MS   30000   // Timeout connexion cellulaire (ms)
#define CONN_CHECK_INTERVAL_MS   5000    // Vérification connectivité périodique (ms)

// ─────────────────────────────────────────────────────────────────────────────
//  Écran OLED — SSD1306 128x64 I2C
// ─────────────────────────────────────────────────────────────────────────────
#define OLED_ENABLED        true

#define OLED_SDA_PIN        21      // I2C SDA
#define OLED_SCL_PIN        22      // I2C SCL
#define OLED_I2C_ADDR       0x3C    // Adresse I2C de l'écran
#define OLED_WIDTH          128     // Largeur en pixels
#define OLED_HEIGHT         64      // Hauteur en pixels
#define OLED_REFRESH_MS     500     // Intervalle de rafraîchissement (ms)

// ─────────────────────────────────────────────────────────────────────────────
//  Boutons physiques (écran + verrouillage)
// ─────────────────────────────────────────────────────────────────────────────
#define BTN_LOCK_PIN        15      // Bouton verrouillage/déverrouillage
#define BTN_PAGE_PIN        0       // Bouton changement de page OLED
#define BTN_DEBOUNCE_MS     200     // Anti-rebond (ms)
#define BTN_LONG_PRESS_MS   1000    // Seuil appui long (ms)
#define DISPLAY_AUTO_RETURN_MS  10000  // Retour auto à la page principale (ms)

// ─────────────────────────────────────────────────────────────────────────────
//  Batterie
// ─────────────────────────────────────────────────────────────────────────────
#define BATT_VOLTAGE_MIN    33.0f   // Tension minimale batterie (V) — alerte
#define BATT_VOLTAGE_MAX    42.0f   // Tension maximale batterie (V) — 100%

// ─────────────────────────────────────────────────────────────────────────────
//  Deep sleep et gestion d'énergie multi-niveaux
// ─────────────────────────────────────────────────────────────────────────────
// Transitions : ACTIVE → IDLE_LIGHT (2min) → IDLE_DEEP (10min) → SLEEP (30min)
#define IDLE_LIGHT_TIMEOUT_MS   120000    // 2 min → mode veille légère
#define IDLE_DEEP_TIMEOUT_MS    600000    // 10 min → mode veille profonde partielle
#define SLEEP_TIMEOUT_MS        1800000   // 30 min → deep sleep ESP32

// Heartbeat en deep sleep (réveil timer périodique)
#define SLEEP_HEARTBEAT_SEC     300       // 5 minutes entre chaque réveil

// Seuil ADC pour réveil par manette
#define THROTTLE_WAKE_THRESHOLD 50

// Jingle de démarrage (fréquences Hz)
#define BOOT_JINGLE_FREQ_1      523       // Do5
#define BOOT_JINGLE_FREQ_2      659       // Mi5
#define BOOT_JINGLE_DURATION_MS 100       // Durée de chaque note (ms)

// ─────────────────────────────────────────────────────────────────────────────
//  LoRa 915 MHz — Module RYLR896 via UART (commandes AT)
// ─────────────────────────────────────────────────────────────────────────────
#define LORA_ENABLED        false   // Activer la communication LoRa

#define LORA_TX_PIN         15      // ESP32 TX → module LoRa RX
#define LORA_RX_PIN         33      // ESP32 RX ← module LoRa TX
#define LORA_BAUD           115200  // Débit UART du module LoRa
#define LORA_SEND_INTERVAL_MS   10000   // Intervalle d'envoi télémétrie LoRa (ms)

// ─────────────────────────────────────────────────────────────────────────────
//  Triple redondance — Watchdog connectivité
// ─────────────────────────────────────────────────────────────────────────────
// Délai sans réponse avant de considérer un lien comme perdu
#define WIFI_LINK_TIMEOUT_MS    10000   // WiFi : 10 s
#define LTE_LINK_TIMEOUT_MS     15000   // LTE  : 15 s
#define LORA_LINK_TIMEOUT_MS    30000   // LoRa : 30 s

// Grâce supplémentaire avant de déclarer HORS ZONE (tous liens perdus)
#define WATCHDOG_GRACE_MS       5000    // 5 s de grâce après perte du dernier lien

// ─────────────────────────────────────────────────────────────────────────────
//  Musique (lecteur de mélodies RTTTL via DAC)
// ─────────────────────────────────────────────────────────────────────────────
#define MUSIC_ENABLED           true
#define MUSIC_MAX_NOTES         200     // Notes max par mélodie parsée
#define MUSIC_DEFAULT_OCTAVE    6
#define MUSIC_DEFAULT_DURATION  4       // Noire par défaut
#define MUSIC_DEFAULT_BPM       120
#define MUSIC_PAGE              4       // Index de la page musique sur l'OLED
