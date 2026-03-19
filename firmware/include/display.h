#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  display.h  —  Écran OLED SSD1306 128x64 avec U8g2
//
//  Pages d'affichage :
//    Page 0 : Tableau de bord (vitesse, batterie, icônes état)
//    Page 1 : Trajet (durée, vitesse moyenne, courant)
//    Page 2 : Système (version FW, IP, MAC, uptime, heap)
//    Page 3 : Alertes (auto si température > 80°C ou tension < 34V)
//    Page 4 : Musique (lecteur RTTTL, contrôles, progression)
//    Page spéciale : HORS ZONE (clignotant, tous liens RF perdus)
//
//  Utilise U8g2 en mode hardware I2C (Wire).
//  Gardé derrière #if OLED_ENABLED.
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include "config.h"

#if OLED_ENABLED

#include <U8g2lib.h>
#include <Wire.h>

// ── Structure de données pour l'affichage ────────────────────────────────────
struct DisplayData {
    // Télémétrie ESC
    float speed;           // km/h
    float voltage;         // V
    float current;         // A (courant moteur)
    float tempFet;         // °C (température contrôleur)
    float tempMotor;       // °C (température moteur)
    float rpm;             // tours/min

    // Batterie
    float battPercent;     // 0-100%

    // GPS (simulé ou réel)
    float lat;
    float lon;

    // État connexion
    bool wifiConnected;
    bool wsConnected;
    bool micActive;
    bool locked;

    // Type de connexion (WiFi / LTE / Aucun)
    uint8_t connType;      // ConnType enum : 0=NONE, 1=WIFI, 2=LTE
    int     rssi;          // Force du signal

    // Trajet
    uint32_t tripStartMs;  // millis() au début du trajet
    float    tripAvgSpeed; // Vitesse moyenne (km/h)

    // État énergie
    uint8_t powerState;    // 0=ACTIVE, 1=IDLE_LIGHT, 2=IDLE_DEEP, 3=SLEEP

    // LoRa
    bool loraConnected;
};

// ── Instance U8g2 — SSD1306 128x64 I2C hardware ─────────────────────────────
// U8G2_SSD1306_128X64_NONAME_F_HW_I2C : full buffer, hardware I2C
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C _u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE,
                                                    /* clock=*/ OLED_SCL_PIN,
                                                    /* data=*/  OLED_SDA_PIN);

// ── État de l'écran ──────────────────────────────────────────────────────────
static uint8_t  _displayPage       = 0;
static uint8_t  _displayPageCount  = 5;  // 0-3 + page 4 musique
static bool     _displayOutOfRange = false;
static uint32_t _displayLastBlink  = 0;
static bool     _displayBlinkState = false;
static bool     _displayPowerOn    = true;

// ─────────────────────────────────────────────────────────────────────────────
//  displayInit() — Initialiser l'écran OLED et afficher le logo de démarrage
// ─────────────────────────────────────────────────────────────────────────────
static void displayInit() {
    Serial.println("[oled] initialisation SSD1306 128x64...");

    _u8g2.begin();
    _u8g2.setContrast(180);

    // Logo de démarrage "Trotilou"
    _u8g2.clearBuffer();
    _u8g2.setFont(u8g2_font_helvB14_tr);

    // Centrer "Trotilou"
    const char* title = "Trotilou";
    int titleW = _u8g2.getStrWidth(title);
    _u8g2.drawStr((OLED_WIDTH - titleW) / 2, 28, title);

    // Version en petit
    _u8g2.setFont(u8g2_font_5x7_tr);
    char verBuf[32];
    snprintf(verBuf, sizeof(verBuf), "FW %s", FW_VERSION);
    int verW = _u8g2.getStrWidth(verBuf);
    _u8g2.drawStr((OLED_WIDTH - verW) / 2, 45, verBuf);

    // Ligne décorative
    _u8g2.drawHLine(20, 50, OLED_WIDTH - 40);

    _u8g2.sendBuffer();
    Serial.println("[oled] logo affiché");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Fonctions utilitaires de dessin
// ─────────────────────────────────────────────────────────────────────────────

// Dessiner une barre de batterie (x, y, largeur, hauteur, pourcentage 0-100)
static void _drawBatteryBar(int x, int y, int w, int h, float percent) {
    // Contour de la batterie
    _u8g2.drawFrame(x, y, w, h);
    // Borne positive
    _u8g2.drawBox(x + w, y + h / 4, 2, h / 2);
    // Remplissage proportionnel
    int fillW = (int)((w - 2) * constrain(percent, 0.0f, 100.0f) / 100.0f);
    if (fillW > 0) {
        _u8g2.drawBox(x + 1, y + 1, fillW, h - 2);
    }
}

// Dessiner une icône WiFi simplifiée (x, y) — petit symbole 8x8
static void _drawWifiIcon(int x, int y, bool connected) {
    if (connected) {
        // Arcs concentriques simplifiés
        _u8g2.drawCircle(x + 4, y + 6, 6, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
        _u8g2.drawCircle(x + 4, y + 6, 3, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
        _u8g2.drawPixel(x + 4, y + 5);
    } else {
        // X pour déconnecté
        _u8g2.drawLine(x, y, x + 8, y + 8);
        _u8g2.drawLine(x + 8, y, x, y + 8);
    }
}

// Dessiner une icône cadenas (x, y)
static void _drawLockIcon(int x, int y, bool locked) {
    if (locked) {
        // Anse du cadenas
        _u8g2.drawFrame(x + 2, y, 5, 4);
        // Corps
        _u8g2.drawBox(x, y + 4, 9, 6);
    } else {
        // Anse ouverte
        _u8g2.drawLine(x + 2, y + 3, x + 2, y);
        _u8g2.drawHLine(x + 2, y, 5);
        _u8g2.drawLine(x + 6, y, x + 6, y + 1);
        // Corps
        _u8g2.drawFrame(x, y + 4, 9, 6);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  _displayDrawPage0() — Tableau de bord principal
//
//  ┌────────────────────────┐
//  │ 25.3 km/h     🔒 📶  │
//  │                       │
//  │ [████████░░] 78%      │
//  │ 38.5V  12.3A  45°C   │
//  └────────────────────────┘
// ─────────────────────────────────────────────────────────────────────────────
static void _displayDrawPage0(const DisplayData* d) {
    // Vitesse en gros
    _u8g2.setFont(u8g2_font_helvB24_tr);
    char speedBuf[16];
    snprintf(speedBuf, sizeof(speedBuf), "%.0f", d->speed);
    _u8g2.drawStr(0, 28, speedBuf);

    // Unité km/h en petit
    _u8g2.setFont(u8g2_font_5x7_tr);
    int speedW = _u8g2.getStrWidth(speedBuf);
    // Utiliser la police helvB24 pour mesurer la largeur exacte
    // Approximation : chaque chiffre ~16px
    _u8g2.drawStr(speedW + 5, 28, "km/h");

    // Icônes d'état en haut à droite
    _drawLockIcon(OLED_WIDTH - 12, 0, d->locked);
    _drawWifiIcon(OLED_WIDTH - 24, 0, d->wifiConnected);

    // Icône micro (petit point clignotant)
    if (d->micActive) {
        _u8g2.drawDisc(OLED_WIDTH - 32, 4, 2);
    }

    // Barre de batterie
    _drawBatteryBar(0, 38, 80, 10, d->battPercent);

    // Pourcentage batterie
    char battBuf[8];
    snprintf(battBuf, sizeof(battBuf), "%.0f%%", d->battPercent);
    _u8g2.setFont(u8g2_font_5x7_tr);
    _u8g2.drawStr(84, 46, battBuf);

    // Ligne du bas : tension, courant, température
    char infoBuf[40];
    snprintf(infoBuf, sizeof(infoBuf), "%.1fV  %.1fA  %.0f%cC",
             d->voltage, d->current, d->tempFet, 0xB0);
    _u8g2.drawStr(0, 62, infoBuf);
}

// ─────────────────────────────────────────────────────────────────────────────
//  _displayDrawPage1() — Informations du trajet
// ─────────────────────────────────────────────────────────────────────────────
static void _displayDrawPage1(const DisplayData* d) {
    _u8g2.setFont(u8g2_font_6x10_tr);
    _u8g2.drawStr(0, 10, "-- TRAJET --");
    _u8g2.drawHLine(0, 12, OLED_WIDTH);

    // Durée du trajet
    uint32_t elapsed = 0;
    if (d->tripStartMs > 0) {
        elapsed = (millis() - d->tripStartMs) / 1000;
    }
    uint32_t minutes = elapsed / 60;
    uint32_t seconds = elapsed % 60;
    char durBuf[32];
    snprintf(durBuf, sizeof(durBuf), "Duree: %lu:%02lu", (unsigned long)minutes, (unsigned long)seconds);
    _u8g2.drawStr(0, 26, durBuf);

    // Vitesse moyenne
    char avgBuf[32];
    snprintf(avgBuf, sizeof(avgBuf), "Moy: %.1f km/h", d->tripAvgSpeed);
    _u8g2.drawStr(0, 40, avgBuf);

    // Courant actuel
    char curBuf[32];
    snprintf(curBuf, sizeof(curBuf), "Courant: %.2f A", d->current);
    _u8g2.drawStr(0, 54, curBuf);
}

// ─────────────────────────────────────────────────────────────────────────────
//  _displayDrawPage2() — Informations système
// ─────────────────────────────────────────────────────────────────────────────
static void _displayDrawPage2(const DisplayData* d) {
    _u8g2.setFont(u8g2_font_5x7_tr);
    _u8g2.drawStr(0, 8, "-- SYSTEME --");
    _u8g2.drawHLine(0, 10, OLED_WIDTH);

    // Version firmware
    char fwBuf[32];
    snprintf(fwBuf, sizeof(fwBuf), "FW: %s", FW_VERSION);
    _u8g2.drawStr(0, 20, fwBuf);

    // Adresse IP
    char ipBuf[32];
    if (d->connType == 1) {  // CONN_WIFI
        snprintf(ipBuf, sizeof(ipBuf), "IP: %s", WiFi.localIP().toString().c_str());
    } else {
        snprintf(ipBuf, sizeof(ipBuf), "IP: (pas WiFi)");
    }
    _u8g2.drawStr(0, 30, ipBuf);

    // Adresse MAC
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macBuf[32];
    snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    _u8g2.drawStr(0, 40, macBuf);

    // Uptime
    uint32_t upSec = millis() / 1000;
    uint32_t upMin = upSec / 60;
    uint32_t upHr  = upMin / 60;
    char uptBuf[32];
    snprintf(uptBuf, sizeof(uptBuf), "Up: %luh%02lum%02lus",
             (unsigned long)upHr, (unsigned long)(upMin % 60), (unsigned long)(upSec % 60));
    _u8g2.drawStr(0, 50, uptBuf);

    // Heap libre
    char heapBuf[32];
    snprintf(heapBuf, sizeof(heapBuf), "Heap: %u o", (unsigned)esp_get_free_heap_size());
    _u8g2.drawStr(0, 60, heapBuf);
}

// ─────────────────────────────────────────────────────────────────────────────
//  _displayDrawPage3() — Alertes automatiques
//
//  Affichée automatiquement si :
//    - Température FET > 80°C
//    - Tension batterie < 34V
// ─────────────────────────────────────────────────────────────────────────────
static void _displayDrawPage3(const DisplayData* d) {
    _u8g2.setFont(u8g2_font_6x10_tr);
    _u8g2.drawStr(0, 10, "!! ALERTES !!");
    _u8g2.drawHLine(0, 12, OLED_WIDTH);

    int y = 26;
    bool hasAlert = false;

    // Alerte température
    if (d->tempFet > 80.0f) {
        char tempBuf[40];
        snprintf(tempBuf, sizeof(tempBuf), "TEMP HAUTE: %.0f C", d->tempFet);
        _u8g2.drawStr(0, y, tempBuf);
        y += 14;
        hasAlert = true;
    }

    // Alerte tension basse
    if (d->voltage < 34.0f && d->voltage > 0.1f) {
        char voltBuf[40];
        snprintf(voltBuf, sizeof(voltBuf), "BATT FAIBLE: %.1fV", d->voltage);
        _u8g2.drawStr(0, y, voltBuf);
        y += 14;
        hasAlert = true;
    }

    // Alerte aucune connexion
    if (d->connType == 0 && !d->loraConnected) {
        _u8g2.drawStr(0, y, "AUCUNE CONNEXION");
        y += 14;
        hasAlert = true;
    }

    if (!hasAlert) {
        _u8g2.setFont(u8g2_font_6x10_tr);
        _u8g2.drawStr(10, 40, "Aucune alerte");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  _displayDrawPage4() — Lecteur de musique
//
//  ┌──────────────────────────┐
//  │ ♪ MUSIQUE         03/06 │
//  │                          │
//  │   ♫ Super Mario ♫        │
//  │                          │
//  │   ████████░░░░░  0:12    │
//  │  LOCK:▶  PTT:◄◄  PG:►► │
//  └──────────────────────────┘
// ─────────────────────────────────────────────────────────────────────────────
static void _displayDrawPage4(const DisplayData* d) {
    (void)d; // Non utilisé directement — les données viennent de music.h

    // Inclure les fonctions de music.h (déclarées avant display.h dans main.cpp)
    _u8g2.setFont(u8g2_font_5x7_tr);

    // En-tête : icône note + titre + numéro piste
    _u8g2.drawStr(0, 8, "\x0E MUSIQUE");  // ♪ symbole
    char idxBuf[12];
    snprintf(idxBuf, sizeof(idxBuf), "%02d/%02d",
             musicGetTrackIndex() + 1, musicGetTrackCount());
    int idxW = _u8g2.getStrWidth(idxBuf);
    _u8g2.drawStr(OLED_WIDTH - idxW, 8, idxBuf);
    _u8g2.drawHLine(0, 10, OLED_WIDTH);

    // Nom de la piste (centré, police moyenne)
    _u8g2.setFont(u8g2_font_6x10_tr);
    const char* name = musicGetTrackName();
    int nameW = _u8g2.getStrWidth(name);
    _u8g2.drawStr((OLED_WIDTH - nameW) / 2, 28, name);

    // Icône play/pause
    if (musicIsPlaying()) {
        // Triangle play ▶
        _u8g2.drawTriangle(
            OLED_WIDTH / 2 - 20, 32,
            OLED_WIDTH / 2 - 20, 40,
            OLED_WIDTH / 2 - 14, 36);
    } else if (musicIsPaused()) {
        // Deux barres pause ❚❚
        _u8g2.drawBox(OLED_WIDTH / 2 - 20, 32, 3, 8);
        _u8g2.drawBox(OLED_WIDTH / 2 - 15, 32, 3, 8);
    } else {
        // Carré stop ■
        _u8g2.drawBox(OLED_WIDTH / 2 - 20, 32, 8, 8);
    }

    // Barre de progression
    float progress = musicGetProgress();
    int barX = 4;
    int barY = 45;
    int barW = OLED_WIDTH - 40;
    int barH = 6;
    _u8g2.drawFrame(barX, barY, barW, barH);
    int fillW = (int)(barW * progress);
    if (fillW > 0) _u8g2.drawBox(barX, barY, fillW, barH);

    // Temps écoulé
    uint32_t elapsed = musicGetElapsedSec();
    uint32_t total   = musicGetTotalSec();
    char timeBuf[16];
    snprintf(timeBuf, sizeof(timeBuf), "%lu:%02lu",
             (unsigned long)(elapsed / 60), (unsigned long)(elapsed % 60));
    _u8g2.setFont(u8g2_font_5x7_tr);
    _u8g2.drawStr(barX + barW + 3, barY + 6, timeBuf);

    // Légende des boutons en bas
    _u8g2.setFont(u8g2_font_5x7_tr);
    _u8g2.drawStr(0, 62, "LCK:\x10\x11 PTT:\x1E PG:\x1F");
}

// ─────────────────────────────────────────────────────────────────────────────
//  _displayDrawOutOfRange() — Écran spécial HORS ZONE (clignotant)
//
//  Affiché quand tous les liens RF sont perdus (WiFi + LTE + LoRa).
//  Clignote toutes les 500ms pour attirer l'attention.
// ─────────────────────────────────────────────────────────────────────────────
static void _displayDrawOutOfRange() {
    uint32_t now = millis();
    if (now - _displayLastBlink > 500) {
        _displayLastBlink = now;
        _displayBlinkState = !_displayBlinkState;
    }

    if (_displayBlinkState) {
        // Cadre d'alerte
        _u8g2.drawFrame(0, 0, OLED_WIDTH, OLED_HEIGHT);
        _u8g2.drawFrame(2, 2, OLED_WIDTH - 4, OLED_HEIGHT - 4);

        // Texte HORS ZONE centré et en gros
        _u8g2.setFont(u8g2_font_helvB14_tr);
        const char* msg = "HORS ZONE";
        int msgW = _u8g2.getStrWidth(msg);
        _u8g2.drawStr((OLED_WIDTH - msgW) / 2, 30, msg);

        // Sous-texte
        _u8g2.setFont(u8g2_font_5x7_tr);
        const char* sub = "Liens RF perdus";
        int subW = _u8g2.getStrWidth(sub);
        _u8g2.drawStr((OLED_WIDTH - subW) / 2, 48, sub);

        const char* sub2 = "NEUTRE FORCE";
        int sub2W = _u8g2.getStrWidth(sub2);
        _u8g2.drawStr((OLED_WIDTH - sub2W) / 2, 60, sub2);
    }
    // Sinon : écran vide (clignotement)
}

// ─────────────────────────────────────────────────────────────────────────────
//  displayUpdate() — Rafraîchir l'écran avec les données actuelles
//
//  Appelé depuis loop() à intervalle OLED_REFRESH_MS.
//  Choisit la page à afficher selon l'état.
// ─────────────────────────────────────────────────────────────────────────────
static void displayUpdate(const DisplayData* d) {
    if (!_displayPowerOn) return;

    _u8g2.clearBuffer();

    // Page spéciale HORS ZONE (priorité maximale)
    if (_displayOutOfRange) {
        _displayDrawOutOfRange();
        _u8g2.sendBuffer();
        return;
    }

    // Alerte automatique : basculer sur page 3 si condition critique
    bool autoAlert = (d->tempFet > 80.0f) ||
                     (d->voltage < 34.0f && d->voltage > 0.1f);
    uint8_t pageToShow = autoAlert ? 3 : _displayPage;

    // Dessiner la page sélectionnée
    switch (pageToShow) {
        case 0: _displayDrawPage0(d); break;
        case 1: _displayDrawPage1(d); break;
        case 2: _displayDrawPage2(d); break;
        case 3: _displayDrawPage3(d); break;
        case 4: _displayDrawPage4(d); break;  // Musique
        default: _displayDrawPage0(d); break;
    }

    // Indicateur de page en bas à droite (petits points)
    for (uint8_t i = 0; i < _displayPageCount; i++) {
        int dotX = OLED_WIDTH - (_displayPageCount - i) * 6;
        if (i == pageToShow) {
            _u8g2.drawDisc(dotX, 63, 2);
        } else {
            _u8g2.drawCircle(dotX, 63, 2);
        }
    }

    _u8g2.sendBuffer();
}

// ─────────────────────────────────────────────────────────────────────────────
//  displaySetPage() — Changer la page affichée
// ─────────────────────────────────────────────────────────────────────────────
static void displaySetPage(uint8_t page) {
    if (page < _displayPageCount) {
        _displayPage = page;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  displayNextPage() — Passer à la page suivante (cyclique)
// ─────────────────────────────────────────────────────────────────────────────
static void displayNextPage() {
    _displayPage = (_displayPage + 1) % _displayPageCount;
}

// ─────────────────────────────────────────────────────────────────────────────
//  displayShowOutOfRange() — Activer/désactiver l'alerte HORS ZONE
// ─────────────────────────────────────────────────────────────────────────────
static void displayShowOutOfRange(bool show) {
    _displayOutOfRange = show;
}

// ─────────────────────────────────────────────────────────────────────────────
//  displayOff() / displayOn() — Éteindre/allumer l'écran (économie d'énergie)
// ─────────────────────────────────────────────────────────────────────────────
static void displayOff() {
    _u8g2.setPowerSave(1);
    _displayPowerOn = false;
    Serial.println("[oled] écran éteint (économie)");
}

static void displayOn() {
    _u8g2.setPowerSave(0);
    _displayPowerOn = true;
    Serial.println("[oled] écran allumé");
}

#else  // OLED_ENABLED == false

// ── Stubs vides quand l'écran est désactivé ──────────────────────────────────
struct DisplayData {
    float speed, voltage, current, tempFet, tempMotor, rpm;
    float battPercent, lat, lon;
    bool wifiConnected, wsConnected, micActive, locked;
    uint8_t connType; int rssi;
    uint32_t tripStartMs; float tripAvgSpeed;
    uint8_t powerState;
    bool loraConnected;
};

static void displayInit()                        {}
static void displayUpdate(const DisplayData*)    {}
static void displaySetPage(uint8_t)              {}
static void displayNextPage()                    {}
static void displayShowOutOfRange(bool)          {}
static void displayOff()                         {}
static void displayOn()                          {}

#endif  // OLED_ENABLED
