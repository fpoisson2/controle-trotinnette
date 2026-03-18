# Plan d'implémentation LTE — SIM7670E sur T-A7670E

## 1. Contexte matériel et mapping des pins

Le LilyGO T-A7670E a un modem SIM7670E connecté en interne :

| GPIO | Fonction modem | Utilisation firmware actuelle |
|------|----------------|-------------------------------|
| 4 | MODEM_PWRKEY | Libre (nécessaire pour allumer le modem) |
| 25 | MODEM_DTR | Utilisé comme DAC speaker (DTR non activé, coexistence OK) |
| 26 | MODEM_TX (Serial vers modem RX) | Libre (I2S utilise GPIO 19 en prod) |
| 27 | MODEM_RX (Serial depuis modem TX) | Libre (I2S utilise GPIO 18 en prod) |
| 33 | MODEM_RING | Libre (I2S utilise GPIO 34 en prod) |

**Conclusion** : GPIO 26/27 sont libres en production. Pas de conflit avec I2S (18/19/34).

## 2. Étapes d'implémentation

### 2.1 Bibliothèques PlatformIO

```ini
lib_deps =
    links2004/WebSockets @ ^2.4.0
    bblanchon/ArduinoJson @ ^7.0.0
    vshymanskyy/TinyGSM @ ^0.12.0
    govorox/SSLClient @ ^1.3.0

build_flags =
    -DCORE_DEBUG_LEVEL=3
    -DTINY_GSM_MODEM_SIM7600
```

### 2.2 Ajouts config.h

```cpp
// ── LTE Modem (SIM7670E intégré au T-A7670E) ──
#define MODEM_UART       Serial1
#define MODEM_BAUD       115200
#define MODEM_TX_PIN     26
#define MODEM_RX_PIN     27
#define MODEM_PWRKEY_PIN 4
#define MODEM_DTR_PIN    -1    // GPIO25 = DAC speaker
#define MODEM_RING_PIN   33

// ── LTE / APN ──
#define LTE_ENABLED      true
#define LTE_APN          "internet"
#define LTE_APN_USER     ""
#define LTE_APN_PASS     ""
#define LTE_SIM_PIN      ""

// ── Stratégie de connexion ──
#define WIFI_TIMEOUT_MS       15000
#define LTE_CONNECT_TIMEOUT_MS 30000
#define CONN_CHECK_INTERVAL_MS 5000
```

### 2.3 Nouveau header `modem.h`

Encapsule : allumage via PWRKEY, init AT, connexion GPRS/LTE, déconnexion, signal quality, power off.

### 2.4 Gestionnaire de connectivité `connectivity.h`

- État : `CONN_NONE`, `CONN_WIFI`, `CONN_LTE`
- Boot : WiFi d'abord (15s timeout) → fallback LTE
- Vérification périodique (5s) : si WiFi perdu → LTE, si LTE actif → probe WiFi toutes les 30s
- Client SSL unifié : WiFiClientSecure (WiFi) ou TinyGsmClientSecure (LTE)

### 2.5 Approche WebSocket

**Recommandation** : PPPoS (Point-to-Point Protocol over Serial) — le modem crée une interface réseau transparente. Le code WebSocket existant fonctionne sans modification.

Alternative : remplacer `links2004/WebSockets` par `gilmaimon/ArduinoWebsockets` qui accepte un `Client*` générique.

### 2.6 Télémétrie enrichie

```cpp
doc["conn"] = (activeConn == CONN_WIFI) ? "wifi" : (activeConn == CONN_LTE) ? "lte" : "none";
doc["rssi"] = (activeConn == CONN_WIFI) ? WiFi.RSSI() : modemSignalQuality();
```

### 2.7 Gestion d'énergie du modem

- **Lazy init** : modem éteint si WiFi OK au boot
- **PSM** : `AT+CPSMS=1` après connexion
- **Power off** : `modemPowerOff()` quand WiFi revient
- Consommation : 150-300mA actif, 10-20mA sleep

## 3. Impact mémoire

| Composant | RAM estimée |
|-----------|-------------|
| TinyGSM | ~10 Ko |
| SSL (hardware modem) | 0 (offloadé au SIM7670E) |
| UART buffer | 2 Ko |
| **Total** | ~12 Ko |

## 4. Séquence d'implémentation

1. **Phase 1** : Bring-up modem (AT commands, GPRS)
2. **Phase 2** : PPP ou abstraction Client (WebSocket sur LTE)
3. **Phase 3** : Connectivity manager (fallback automatique)
4. **Phase 4** : Télémétrie + dashboard (affichage type connexion)
5. **Phase 5** : Power management (PSM, lazy init)

## 5. Risques

| Risque | Mitigation |
|--------|------------|
| GPIO 26/27 conflit I2S | Confirmer que prod utilise 18/19 |
| WebSocketsClient pas compatible Client custom | Utiliser PPPoS ou changer de lib |
| Mémoire avec SSL + TinyGSM | Utiliser SSL hardware du SIM7670E |
| Latence LTE pour la voix | WiFi prioritaire, LTE = fallback |
