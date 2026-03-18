# Plan de déploiement et provisionnement de la flotte

## Flash USB en batch — `tools/provision.py`

### Config partagée
`config.h` est **100% identique** pour toutes les unités. L'identification se fait par MAC (message `hello`).

### Flux du script
1. Détecter les ports USB (CP2102/CH340)
2. Compiler le firmware une seule fois (`pio run -e esp32dev`)
3. Flash parallèle via `esptool.py` (bootloader + partitions + firmware)
4. Moniteur série : attendre `[wifi] connecté` + `[ws-proxy] connecté`
5. Générer QR code par unité (contenu : URL + MAC)
6. Rapport CSV : MAC, port, statut, timestamp

### Template flotte
`firmware/include/config.h.fleet` : WiFi Enterprise (campus), proxy Cloudflare, `FAKE_TELEMETRY false`

## Auto-enregistrement serveur

### Persistance `fleet.json`
```json
{
  "AA:BB:CC:DD:EE:01": {
    "name": "Trottinette-01",
    "firstSeen": "2026-03-18T10:00:00Z",
    "qcStatus": "passed",
    "notes": "Bâtiment A"
  }
}
```

Chargé au démarrage, sauvegardé sur modification (debounce 5s), volume Docker.

### Nouveaux endpoints
- `GET /api/fleet` — registre complet (connectées + hors-ligne)
- `POST /api/fleet/:mac/rename` — renommer
- `POST /api/fleet/:mac/notes` — annoter
- `DELETE /api/fleet/:mac` — retirer du registre

## OTA de flotte — `POST /api/ota/flash-all`

### Refactoring pré-requis
Remplacer le verrou global `otaInProgress` par un verrou **par trottinette** dans la Map scooters.

### Comportement
1. Accepte `{ version }`, récupère le `.bin` pré-compilé
2. Flash par lots de 5 (`OTA_MAX_CONCURRENT = 5`)
3. Progression via SSE : `{ type: "fleet_ota_progress", scooterId, percent, status }`
4. Échecs marqués individuellement, re-flash possible

### Vérification post-OTA
Après reboot, comparer `hello.version` avec la version flashée.

## Tests QC à distance

### Endpoint `POST /api/fleet/:mac/qc`
1. Envoie `{ type: "qc_request" }` via WebSocket
2. ESP32 exécute : ADC throttle/frein, audio I2S RMS, ESC UART, heap libre, uptime
3. Réponse `{ type: "qc_result", tests: {...} }`
4. Résultat stocké dans `fleet.json`

### Dashboard QC
Tableau avec : MAC, nom, FW version, statut connexion, dernier QC, résultat, actions.

## Flux complet de provisionnement

```
1. Brancher ESP32 USB
2. python tools/provision.py
   → Compile → Flash → Vérifie → Imprime MAC + QR
3. Coller le QR sur la trottinette
4. Dashboard web : renommer, tester, valider
5. Trottinette prête pour le service
```

## Flux de mise à jour flotte

```
1. Push sur main (modifiant firmware/)
2. GitHub Actions crée une release
3. Proxy détecte et compile le .bin
4. Admin → Dashboard → "Mettre à jour toute la flotte"
5. Flash par lots de 5, progression temps réel
6. Échecs re-flashés individuellement
```
