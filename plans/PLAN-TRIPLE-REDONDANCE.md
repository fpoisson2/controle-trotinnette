# Architecture de communication triple-redondante

## Les trois liens

### 1. WiFi (primaire) — déjà implémenté
- Latence : 5-150 ms | Bande passante : 1-50 Mbit/s
- Données : TOUT (audio, télémétrie, commandes, OTA)
- Portée : 30-300 m (point d'accès)

### 2. LTE/4G via SIM7670E (secondaire) — à implémenter
- Latence : 50-200 ms | Bande passante : 1-10 Mbit/s
- Données : audio + télémétrie + commandes (pleine capacité)
- Couverture cellulaire quasi illimitée en zone urbaine
- Hot standby recommandé (WebSocket parallèle, heartbeat réduit)

### 3. BLE via téléphone relais (tertiaire) — à implémenter
- Latence : 10-50 ms (local) | Bande passante : ~200 kbit/s
- Données : commandes critiques uniquement (arrêt d'urgence, télémétrie minimale)
- **Arrêt d'urgence LOCAL sans internet en <50ms**
- Nécessite une app mobile (Flutter ou React Native)

## Architecture

```
ESP32 (T-A7670E)
  ├── WiFi (radio interne)     → WSS → Proxy
  ├── LTE (SIM7670E UART2)    → WSS → Proxy
  └── BLE (radio interne)     → App mobile → HTTP → Proxy
                                     └── Arrêt urgence LOCAL (sans internet)
```

## Logique de failover

| WiFi | LTE | BLE | Action |
|------|-----|-----|--------|
| ACTIF | STANDBY | ACTIF | Normal : tout via WiFi |
| INACTIF | ACTIF | ACTIF | Bascule LTE : audio+télémétrie via LTE |
| INACTIF | INACTIF | ACTIF | Mode urgence : arrêt progressif via BLE |
| INACTIF | INACTIF | INACTIF | Sécurité : arrêt moteur immédiat |

## Arrêt d'urgence (cas critique)

1. **BLE local** : commande directe sans internet (<50 ms)
2. **Commande proxy** (WiFi ou LTE) : `{"type":"cmd","action":"arreter"}`
3. **Watchdog timeout** : aucun lien pendant 15s → arrêt automatique
4. **Frein physique ADC** : toujours prioritaire, indépendant

## Impact mémoire FreeRTOS

| Composant | RAM estimée |
|-----------|-------------|
| esp_modem (PPP + UART) | ~12 Ko |
| NimBLE (pile BLE) | ~50 Ko |
| Second WebSocket (LTE) | ~8 Ko |
| LinkManager | ~4 Ko |
| **Total supplémentaire** | **~74 Ko** |

Marge restante : ~160 Ko libres sur ~320 Ko.

## Options évaluées et non retenues

- **LoRa** : excellente portée mais nécessite matériel + passerelle supplémentaire
- **ESP-NOW** : latence ultra-faible mais nécessite un second ESP32 fixe
- **WiFi Direct / Hotspot** : fausse redondance (même mécanisme WiFi)

## Phases d'implémentation

1. **LTE** (2-3 semaines) : modem, PPP, WebSocket, LinkManager
2. **BLE arrêt d'urgence** (2-3 semaines) : NimBLE, GATT, app mobile
3. **Robustesse** (1-2 semaines) : hot standby, déduplication, PSRAM
