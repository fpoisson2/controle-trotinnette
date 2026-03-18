# Architecture de communication triple-redondante

## Les trois liens

### 1. WiFi (primaire) — déjà implémenté
- Latence : 5-150 ms | Bande passante : 1-50 Mbit/s
- Données : TOUT (audio, télémétrie, commandes, OTA)
- Portée : 30-300 m (point d'accès campus)

### 2. LTE/4G via SIM7670E (secondaire) — à implémenter
- Latence : 50-200 ms | Bande passante : 1-10 Mbit/s
- Données : audio + télémétrie + commandes (pleine capacité)
- Couverture cellulaire quasi illimitée en zone urbaine
- Hot standby recommandé (WebSocket parallèle, heartbeat réduit)

### 3. LoRa 915 MHz via UART (tertiaire) — à implémenter
- Latence : 50-200 ms | Bande passante : 0.3-50 kbit/s
- Données : **données uniquement** (pas de voix) — télémétrie, commandes, heartbeat
- Portée : **2-15 km** en extérieur (longue distance)
- Module : REYAX RYLR896 ou EBYTE E32-915T20D (UART simple, AT commands)
- Connexion : **UART** (TX/RX) vers un second ESP32 ou Raspberry Pi en station de base
- Fréquence : 915 MHz (bande ISM Amérique du Nord)

#### Matériel LoRa
- **Côté trottinette** : module RYLR896 (UART, 3.3V, ~20 mA TX, ~5 mA RX)
  - TX → GPIO libre (ex: GPIO 15)
  - RX → GPIO libre (ex: GPIO 33, libéré du MODEM_RING si non utilisé)
  - Antenne SMA incluse
- **Station de base** (campus) : ESP32 + même module LoRa → WiFi/Ethernet → proxy
  - Reçoit les heartbeats et télémétrie de toutes les trottinettes
  - Relaie les commandes critiques (arrêt, verrouillage)

#### Protocole LoRa (binaire compact, pas JSON)
```
[MAC 6 octets][Type 1 octet][Payload N octets][CRC 2 octets]

Types :
  0x01 = Heartbeat (vide — juste "je suis vivant")
  0x02 = Télémétrie (speed, voltage, lat, lon — 16 octets)
  0x03 = Commande (action + intensity — 3 octets)
  0x04 = Arrêt d'urgence (vide — priorité max)
  0x05 = Verrouillage (locked: 0/1 — 1 octet)
  0x06 = ACK (accusé de réception)
```

Envoi toutes les 10s en mode normal, toutes les 30s en idle.

## Architecture

```
ESP32 (T-A7670E)
  ├── WiFi (radio interne)     → WSS → Proxy (via Cloudflare)
  ├── LTE (SIM7670E UART2)    → WSS → Proxy (direct)
  └── LoRa 915 MHz (UART)     → RF → Station de base → Proxy
```

## Sécurité critique : ARRÊT SI AUCUN SIGNAL RF

### Règle absolue
**Si les 3 liens sont perdus simultanément, la trottinette ne peut plus avancer.**

### Implémentation firmware

```cpp
// Dans loop(), toutes les secondes :
void connectivityWatchdog() {
    bool wifiOK = (activeConn == CONN_WIFI);
    bool lteOK  = (activeConn == CONN_LTE);
    bool loraOK = (millis() - lastLoraAck < LORA_TIMEOUT_MS);  // dernier ACK < 30s

    if (!wifiOK && !lteOK && !loraOK) {
        // AUCUN LIEN — arrêt moteur immédiat
        currentThrottle = FTESC_THROTTLE_NEUTRAL;
        escLocked = true;

        // Afficher "HORS ZONE" sur l'écran I2C
        displayShowOutOfRange();

        // Bip d'alerte continu
        sleepPlayAlertTone();
    }
}
```

### Constantes

```cpp
#define WIFI_LINK_TIMEOUT_MS  10000   // WiFi considéré perdu après 10s sans heartbeat
#define LTE_LINK_TIMEOUT_MS   15000   // LTE considéré perdu après 15s
#define LORA_LINK_TIMEOUT_MS  30000   // LoRa considéré perdu après 30s (heartbeat toutes les 10s)
#define WATCHDOG_GRACE_MS     5000    // Grâce de 5s avant arrêt (éviter les faux positifs)
```

### Affichage écran I2C "HORS ZONE"

Sur l'écran OLED SSD1306 :
```
┌──────────────────────┐
│   ⚠ HORS ZONE ⚠     │
│                      │
│  Aucun signal radio  │
│  Moteur désactivé    │
│                      │
│  Revenez vers le     │
│  campus              │
└──────────────────────┘
```

- Écran clignote (alternance normal/inversé toutes les 500ms)
- Bip d'alerte toutes les 2s
- Quand un lien revient → écran normal, moteur réactivé

## Logique de failover

| WiFi | LTE | LoRa | Action |
|------|-----|------|--------|
| ✅ | standby | ✅ | **Normal** : tout via WiFi, LoRa heartbeat |
| ❌ | ✅ | ✅ | **Bascule LTE** : audio+télémétrie via LTE |
| ❌ | ❌ | ✅ | **Mode dégradé** : télémétrie + commandes via LoRa uniquement, pas de voix |
| ❌ | ❌ | ❌ | **HORS ZONE** : arrêt moteur, écran alerte, bip |

### Données par lien

| Donnée | WiFi | LTE | LoRa |
|--------|------|-----|------|
| Audio PCM16 | ✅ | ✅ | ❌ (bande insuf.) |
| Télémétrie complète | ✅ | ✅ | ❌ |
| Télémétrie réduite | ✅ | ✅ | ✅ (16 octets) |
| Commandes moteur | ✅ | ✅ | ✅ |
| Arrêt d'urgence | ✅ | ✅ | ✅ |
| Heartbeat | ✅ | ✅ | ✅ |
| OTA firmware | ✅ | ✅* | ❌ |

## Impact mémoire

| Composant | RAM estimée |
|-----------|-------------|
| esp_modem (PPP + UART) | ~12 Ko |
| LoRa UART buffer | ~1 Ko |
| LinkManager | ~4 Ko |
| **Total supplémentaire** | **~17 Ko** |

(BLE retiré = ~50 Ko économisés par rapport au plan précédent)

## Avantages du LoRa 915 MHz vs BLE

| Critère | LoRa 915 MHz | BLE (ancien plan) |
|---------|-------------|-------------------|
| Portée | 2-15 km | 10-30 m |
| Dépendance | Station de base fixe | Téléphone utilisateur |
| App mobile requise | Non | Oui |
| Fonctionne sans utilisateur | Oui | Non |
| Coût module | ~10-15 $ | Intégré (gratuit) |
| Détection hors-zone | Oui (perte signal = hors campus) | Non |
| Bande passante | Faible mais suffisante pour les données | Moyen |

**Le LoRa est naturellement un détecteur de zone** : si la trottinette sort de la portée de la station de base du campus, le lien LoRa tombe → combiné avec la perte WiFi campus → "HORS ZONE" automatique.

## Phases d'implémentation

1. **Phase 1 — LTE** (2-3 semaines) : modem SIM7670E, PPP, WebSocket, failover WiFi/LTE
2. **Phase 2 — LoRa** (2 semaines) : module UART, protocole binaire, station de base
3. **Phase 3 — Watchdog** (1 semaine) : arrêt hors-zone, affichage écran, bip alerte
4. **Phase 4 — Robustesse** (1 semaine) : tests terrain, calibration timeouts, faux positifs
