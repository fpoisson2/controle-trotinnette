# Plan — Économie de batterie et deep sleep ESP32

## Machine d'états

```
ACTIF → IDLE LÉGER (2 min) → IDLE PROFOND (10 min) → DEEP SLEEP (30 min)
  ↑                                                           │
  └───────────── Réveil (throttle, frein, PTT, timer) ────────┘
```

## Niveaux de veille

### ACTIF (état normal)
- Télémétrie : 500ms
- Audio I2S : actif
- WiFi : actif
- WebSocket : actif

### IDLE LÉGER (après 2 min d'inactivité)
- Télémétrie : toutes les 5s
- Audio I2S : désactivé
- WiFi : power save mode
- WebSocket : heartbeat 30s

### IDLE PROFOND (après 10 min)
- Télémétrie : toutes les 30s
- I2S : désactivé
- OpenAI : déconnecté
- WiFi : actif mais réduit

### DEEP SLEEP (après 30 min)
- ESP32 en deep sleep
- Réveil par timer toutes les 5 min (heartbeat)
- Modem LTE en sleep (AT+CSCLK)
- Consommation : ~10 µA (ESP32) + ~10 mA (modem sleep)

## Sources de réveil

### GPIO RTC (deep sleep wake sources)
- **GPIO 32 (throttle ADC)** : `esp_sleep_enable_ext0_wakeup(GPIO_NUM_32, LOW)`
- **GPIO 39 (frein ADC)** : `esp_sleep_enable_ext1_wakeup(BIT(39), ESP_EXT1_WAKEUP_ANY_HIGH)`
- **GPIO 2 (bouton PTT)** : `esp_sleep_enable_ext0_wakeup(GPIO_NUM_2, LOW)`
- **Timer** : `esp_sleep_enable_timer_wakeup(5 * 60 * 1000000)` (5 min)

### Son de boot au réveil
- Jouer un jingle via DAC GPIO 25 **immédiatement** (avant WiFi)
- Double bip court (do-mi) pour indiquer l'activation
- Implémenté dans `sleepPlayBootJingle()` (déjà dans le code)

## Détection d'inactivité

```cpp
// Réinitialiser le compteur sur :
// - Changement ADC throttle > seuil
// - Changement ADC frein > seuil
// - Réception commande WebSocket
// - Bouton PTT pressé
// - Mouvement GPS (changement lat/lon)
void sleepResetActivity();
```

## Séquence de reconnexion après réveil

1. `sleepPlayBootJingle()` — feedback immédiat
2. `WiFi.begin()` — reconnexion (~2-3s)
3. `wsProxy.begin()` — WebSocket vers proxy
4. Message `hello` avec MAC + version
5. Proxy reçoit → remet la trottinette "en ligne" dans le dashboard

## État "en veille" vs "déconnecté" sur le dashboard

- **En veille** : dernier heartbeat < 10 min → icône lune/zzz
- **Déconnecté** : dernier heartbeat > 10 min → icône rouge

## Ajouts config.h

```cpp
#define IDLE_LIGHT_TIMEOUT_MS   120000    // 2 min → idle léger
#define IDLE_DEEP_TIMEOUT_MS    600000    // 10 min → idle profond
#define SLEEP_TIMEOUT_MS        1800000   // 30 min → deep sleep
#define SLEEP_HEARTBEAT_SEC     300       // 5 min entre les réveils
#define THROTTLE_WAKE_THRESHOLD 50        // variation ADC pour réveiller
#define BOOT_JINGLE_FREQ_1      523       // Do5 (Hz)
#define BOOT_JINGLE_FREQ_2      659       // Mi5 (Hz)
#define BOOT_JINGLE_DURATION_MS 100       // durée par note
```
