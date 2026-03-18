# Plan — Écran OLED I2C + 2 boutons physiques

## Matériel recommandé

### Écran : SSD1306 128x64, 0.96", I2C
- Adresse : `0x3C`
- Alimentation : 3.3V
- Consommation : ~20 mA allumé, ~0.01 mA éteint
- Câblage : VCC→3.3V, GND→GND, SDA→GPIO 21, SCL→GPIO 22

### 2 boutons physiques GPIO (pas I2C — plus simple)

| Bouton | GPIO | Fonction |
|--------|------|----------|
| BTN_LOCK | **15** | Verrouillage/déverrouillage trottinette |
| BTN_PAGE | **0** | Cycle pages écran (bouton BOOT de la carte) |

Câblage : bouton entre GPIO et GND, pull-up interne activé.

**Alternative I2C** : PCF8574 (adresse `0x20`) si on veut rester 100% I2C.

## GPIO 21/22 : libres et confirmés

GPIO 21 (SDA) et 22 (SCL) ne sont pas utilisés par le firmware ni réservés par le modem. Le code actuel fait `Wire.end()` au boot — il suffira de le remplacer par `Wire.begin(21, 22)`.

## Bibliothèque : U8g2

- Mode full buffer (`U8G2_SSD1306_128X64_NONAME_F_HW_I2C`) — 1 KB RAM
- Polices intégrées, support UTF-8 (accents français)
- Dessins vectoriels, barres, icônes XBM
- PlatformIO : `olikraus/U8g2 @ ^2.35.0`

## Pages d'affichage

### Page 1 — Tableau de bord (défaut)
```
┌──────────────────────────┐
│ [WiFi] [WS] [MIC] 15km/h│  icônes + vitesse grande police
│                          │
│ ████████████░░░░  38.2V  │  barre batterie + tension
│ Gear: HIGH    25.3°C     │  vitesse + température
└──────────────────────────┘
```
Si verrouillé : cadenas au centre.

### Page 2 — Trajet en cours
- Durée, vitesse moyenne, courant instantané

### Page 3 — Système
- FW version, IP, MAC, uptime

### Page 4 — Alertes (auto si surchauffe ou batterie faible)
- ⚠ Temp ESC > 80°C
- ⚠ Batterie < 34V

### Page spéciale — HORS ZONE
```
┌──────────────────────────┐
│   ⚠ HORS ZONE ⚠         │
│                          │
│  Aucun signal radio      │
│  Moteur désactivé        │
│                          │
│  Revenez vers le campus  │
└──────────────────────────┘
```
Affiché quand les 3 liens RF sont perdus (WiFi + LTE + LoRa). Écran clignote.

## Gestion des boutons

### BTN_LOCK (GPIO 15)
- Appui court (<500ms) : basculer lock/unlock
- Verrouillé → throttle forcé au neutre, manette ignorée
- Anti-rebond : 200ms

### BTN_PAGE (GPIO 0)
- Appui court : page suivante (1→2→3→4→1)
- Appui long (>1s) : retour page 1
- Auto-retour page 1 après 10s sans appui

## Architecture FreeRTOS

- Écran géré sur **Core 0** (même core que `loop()`)
- Toutes les données de télémétrie déjà disponibles sur Core 0
- Pas de mutex nécessaire (même core)
- Refresh : 500ms (synchrone avec la télémétrie)
- Coût CPU : ~2-5ms par refresh, négligeable

## Intégration deep sleep

- Avant sleep : `u8g2.setPowerSave(1)` → écran éteint (0.01 mA)
- Au réveil : `displayInit()` → logo boot "LimoTrott" pendant 2s
- Progression OTA affichée sur l'écran pendant le flash

## config.h

```cpp
// ── Écran OLED I2C ──
#define OLED_ENABLED        true
#define OLED_SDA_PIN        21
#define OLED_SCL_PIN        22
#define OLED_I2C_ADDR       0x3C
#define OLED_WIDTH          128
#define OLED_HEIGHT         64
#define OLED_REFRESH_MS     500

// ── Boutons ──
#define BTN_LOCK_PIN        15
#define BTN_PAGE_PIN        0
#define BTN_DEBOUNCE_MS     200
#define BTN_LONG_PRESS_MS   1000
#define DISPLAY_AUTO_RETURN_MS  10000

// ── Batterie (barre de progression) ──
#define BATT_VOLTAGE_MIN    33.0f    // 0% (10S LiPo)
#define BATT_VOLTAGE_MAX    42.0f    // 100%
```

## Impact mémoire

| Élément | RAM | Flash |
|---------|-----|-------|
| Buffer écran U8g2 | 1 024 octets | — |
| DisplayData struct | ~80 octets | — |
| Bibliothèque U8g2 | — | ~20-35 KB |
| Polices (2-3 tailles) | — | ~5-10 KB |
| Logo boot XBM | — | ~1 KB |
| **Total** | **~1.1 KB** | **~30-45 KB** |

## Ordre d'implémentation

1. Créer `firmware/include/display.h` (init + page 1)
2. Ajouter defines dans `config.h.example`
3. Modifier `main.cpp` : `Wire.begin()`, `displayInit()`, `displayUpdate()` dans loop
4. Ajouter U8g2 dans `platformio.ini`
5. Ajouter gestion boutons dans loop (debounce, lock, page)
6. Implémenter pages 2, 3, 4 + page HORS ZONE
7. Intégrer avec `sleep.h` (displayOff/displayInit)
8. Logo boot "LimoTrott" en XBM
