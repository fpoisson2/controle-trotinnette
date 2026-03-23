#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  sleep.h  —  Gestion d'énergie multi-niveaux et deep sleep
//
//  Machine à états de puissance :
//    POWER_ACTIVE     → fonctionnement normal
//    POWER_IDLE_LIGHT → après 2 min d'inactivité (télémétrie réduite, I2S off)
//    POWER_IDLE_DEEP  → après 10 min (télémétrie 30s, OpenAI déconnecté)
//    POWER_SLEEP      → après 30 min (deep sleep ESP32, réveil timer/manette)
//
//  Sources de réveil deep sleep :
//    - ext0 : GPIO 2 (bouton PTT)
//    - ext1 : GPIO 32 (throttle) OU GPIO 39 (frein) — ANY_HIGH
//    - Timer : toutes les 5 minutes (heartbeat)
//
//  Au réveil : jingle DAC GPIO 25, puis reconnexion WiFi + WebSocket
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <driver/dac.h>
#include <driver/adc.h>
#include <esp_wifi.h>
#include "config.h"

// ── États de puissance ───────────────────────────────────────────────────────
enum PowerState {
    POWER_ACTIVE     = 0,   // Fonctionnement normal
    POWER_IDLE_LIGHT = 1,   // Veille légère (2 min)
    POWER_IDLE_DEEP  = 2,   // Veille profonde partielle (10 min)
    POWER_SLEEP      = 3    // Deep sleep ESP32 (30 min)
};

// ── État interne ─────────────────────────────────────────────────────────────
static PowerState _sleepPowerState    = POWER_ACTIVE;
static uint32_t   _sleepLastActivity  = 0;
static bool       _sleepI2SDisabled   = false;
static bool       _sleepAIDisconnected = false;

// ── Seuil ADC pour la détection d'activité ──────────────────────────────────
#define SLEEP_ADC_ACTIVITY_THRESH  200

// ── Intervalles de télémétrie par état ───────────────────────────────────────
#define TELEMETRY_ACTIVE_MS        10000   // Normal : 10 s (LTE-friendly)
#define TELEMETRY_IDLE_LIGHT_MS    30000   // Veille légère : 30 s
#define TELEMETRY_IDLE_DEEP_MS     60000   // Veille profonde : 60 s

// ─────────────────────────────────────────────────────────────────────────────
//  sleepGetPowerState() — Retourner l'état de puissance actuel
// ─────────────────────────────────────────────────────────────────────────────
static PowerState sleepGetPowerState() {
    return _sleepPowerState;
}

// ─────────────────────────────────────────────────────────────────────────────
//  sleepGetTelemetryInterval() — Intervalle de télémétrie selon l'état actuel
// ─────────────────────────────────────────────────────────────────────────────
static uint32_t sleepGetTelemetryInterval() {
    switch (_sleepPowerState) {
        case POWER_IDLE_LIGHT: return TELEMETRY_IDLE_LIGHT_MS;
        case POWER_IDLE_DEEP:  return TELEMETRY_IDLE_DEEP_MS;
        default:               return TELEMETRY_ACTIVE_MS;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  sleepGetPowerStateName() — Nom lisible de l'état de puissance
// ─────────────────────────────────────────────────────────────────────────────
static const char* sleepGetPowerStateName() {
    switch (_sleepPowerState) {
        case POWER_ACTIVE:     return "ACTIF";
        case POWER_IDLE_LIGHT: return "VEILLE_LEGERE";
        case POWER_IDLE_DEEP:  return "VEILLE_PROFONDE";
        case POWER_SLEEP:      return "DEEP_SLEEP";
        default:               return "INCONNU";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Jingle de réveil (DAC GPIO 25)
//  Trois notes montantes : Do-Mi-Sol (C5-E5-G5)
//  Joué avant toute initialisation lourde (WiFi, I2S, etc.)
// ─────────────────────────────────────────────────────────────────────────────
static void sleepPlayBootJingle() {
    // Activer le DAC manuellement (pas encore initialisé par audio.h)
    dac_output_enable(DAC_CHANNEL_1);
    dac_output_voltage(DAC_CHANNEL_1, 128);  // silence DC

    struct Note { float freq; uint32_t dur_ms; };
    const Note melody[] = {
        { (float)BOOT_JINGLE_FREQ_1,  BOOT_JINGLE_DURATION_MS },       // Do5
        { (float)BOOT_JINGLE_FREQ_2,  BOOT_JINGLE_DURATION_MS },       // Mi5
        { 784.0f,                      BOOT_JINGLE_DURATION_MS + 20 },  // Sol5 (tenu)
    };

    for (const auto &n : melody) {
        uint32_t period_us = (uint32_t)(1000000.0f / n.freq);
        uint32_t end_ms = millis() + n.dur_ms;
        while (millis() < end_ms) {
            dac_output_voltage(DAC_CHANNEL_1, 200);
            delayMicroseconds(period_us / 2);
            dac_output_voltage(DAC_CHANNEL_1, 56);
            delayMicroseconds(period_us / 2);
        }
        // Pause inter-notes
        dac_output_voltage(DAC_CHANNEL_1, 128);
        delay(30);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Jingle de mise en veille (notes descendantes)
// ─────────────────────────────────────────────────────────────────────────────
static void sleepPlaySleepJingle() {
    dac_output_enable(DAC_CHANNEL_1);
    dac_output_voltage(DAC_CHANNEL_1, 128);

    struct Note { float freq; uint32_t dur_ms; };
    const Note melody[] = {
        { 784.0f,  80 },   // Sol5
        { 523.0f,  80 },   // Do5
        { 330.0f, 120 },   // Mi4 (descend)
    };

    for (const auto &n : melody) {
        uint32_t period_us = (uint32_t)(1000000.0f / n.freq);
        uint32_t end_ms = millis() + n.dur_ms;
        while (millis() < end_ms) {
            dac_output_voltage(DAC_CHANNEL_1, 180);
            delayMicroseconds(period_us / 2);
            dac_output_voltage(DAC_CHANNEL_1, 76);
            delayMicroseconds(period_us / 2);
        }
        dac_output_voltage(DAC_CHANNEL_1, 128);
        delay(30);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  sleepPlayAlertTone() — Tonalité d'alerte pour hors zone / urgence
//
//  Son strident alternant deux fréquences (style alarme)
// ─────────────────────────────────────────────────────────────────────────────
static void sleepPlayAlertTone() {
    dac_output_enable(DAC_CHANNEL_1);

    // 3 cycles de bip-bip strident
    for (int cycle = 0; cycle < 3; cycle++) {
        // Fréquence haute (1000 Hz)
        uint32_t period1 = 1000;  // 1000 µs = 1000 Hz
        uint32_t end1 = millis() + 80;
        while (millis() < end1) {
            dac_output_voltage(DAC_CHANNEL_1, 220);
            delayMicroseconds(period1 / 2);
            dac_output_voltage(DAC_CHANNEL_1, 36);
            delayMicroseconds(period1 / 2);
        }

        // Fréquence basse (600 Hz)
        uint32_t period2 = 1667;  // ~600 Hz
        uint32_t end2 = millis() + 80;
        while (millis() < end2) {
            dac_output_voltage(DAC_CHANNEL_1, 220);
            delayMicroseconds(period2 / 2);
            dac_output_voltage(DAC_CHANNEL_1, 36);
            delayMicroseconds(period2 / 2);
        }

        dac_output_voltage(DAC_CHANNEL_1, 128);
        delay(40);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Déterminer la cause du réveil
// ─────────────────────────────────────────────────────────────────────────────
static const char* sleepGetWakeReason() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
        case ESP_SLEEP_WAKEUP_EXT0:     return "ext0 (bouton PTT GPIO2)";
        case ESP_SLEEP_WAKEUP_EXT1:     return "ext1 (throttle/frein)";
        case ESP_SLEEP_WAKEUP_TIMER:    return "timer (heartbeat)";
        case ESP_SLEEP_WAKEUP_TOUCHPAD: return "touchpad";
        case ESP_SLEEP_WAKEUP_ULP:      return "ULP";
        default:                        return "power-on / reset";
    }
}

static bool sleepIsWakeFromDeepSleep() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    return (cause != ESP_SLEEP_WAKEUP_UNDEFINED);
}

// ─────────────────────────────────────────────────────────────────────────────
//  sleepEnterDeepSleep() — Configurer les sources de réveil et dormir
//
//  Sources de réveil :
//    - ext0 : GPIO 2 (bouton PTT) — actif LOW
//    - ext1 : GPIO 32 | GPIO 39 — ANY_HIGH (manette/frein)
//    - Timer : SLEEP_HEARTBEAT_SEC secondes
// ─────────────────────────────────────────────────────────────────────────────
static void sleepEnterDeepSleep() {
    Serial.println("[sleep] préparation deep sleep...");
    Serial.flush();

    // Jouer le jingle de mise en veille
    sleepPlaySleepJingle();

    // Désactiver WiFi et Bluetooth pour économiser
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(10);

    // Source de réveil ext0 : bouton PTT (GPIO 2, actif LOW)
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_2, 0);  // 0 = réveil quand LOW

    // Source de réveil ext1 : manette (GPIO 32) OU frein (GPIO 39)
    uint64_t wakeupMask = (1ULL << 32) | (1ULL << 39);
    esp_sleep_enable_ext1_wakeup(wakeupMask, ESP_EXT1_WAKEUP_ANY_HIGH);

    // Pull-down sur GPIO 32 en mode RTC (maintenu pendant le deep sleep)
    rtc_gpio_pulldown_en(GPIO_NUM_32);
    rtc_gpio_pullup_dis(GPIO_NUM_32);

    // Source de réveil timer : heartbeat périodique
    if (SLEEP_HEARTBEAT_SEC > 0) {
        uint64_t sleepUs = (uint64_t)SLEEP_HEARTBEAT_SEC * 1000000ULL;
        esp_sleep_enable_timer_wakeup(sleepUs);
        Serial.printf("[sleep] timer wakeup : %d secondes\n", SLEEP_HEARTBEAT_SEC);
    }

    Serial.println("[sleep] entrée en deep sleep — réveil par PTT, manette, frein ou timer");
    Serial.flush();
    delay(50);

    esp_deep_sleep_start();
    // --- Le programme ne continue jamais ici ---
}

// ─────────────────────────────────────────────────────────────────────────────
//  sleepResetActivity() — Remettre le compteur d'inactivité à zéro
//
//  Appelé lors de toute activité :
//    - Manette/frein actionné
//    - Bouton PTT enfoncé
//    - Commande IA reçue
//    - Échange WebSocket
// ─────────────────────────────────────────────────────────────────────────────
static void sleepResetActivity() {
    _sleepLastActivity = millis();

    // Si on était en veille légère ou profonde, revenir en mode actif
    if (_sleepPowerState != POWER_ACTIVE) {
        Serial.printf("[sleep] activité détectée — retour ACTIF (était %s)\n",
                      sleepGetPowerStateName());

        // Réactiver I2S si désactivé par la veille
        if (_sleepI2SDisabled) {
            _sleepI2SDisabled = false;
            audioStartI2S();  // Reprendre le driver existant (pas réinstaller)
            Serial.println("[sleep] I2S réactivé (start)");
        }

        // Désactiver WiFi power save (seulement si WiFi actif)
        if (!_wsUseLte) {
            esp_wifi_set_ps(WIFI_PS_NONE);
        }

        _sleepPowerState = POWER_ACTIVE;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  sleepIsI2SDisabled() — Vérifier si l'I2S a été désactivé par le mode veille
// ─────────────────────────────────────────────────────────────────────────────
static bool sleepIsI2SDisabled() {
    return _sleepI2SDisabled;
}

// ─────────────────────────────────────────────────────────────────────────────
//  sleepIsAIDisconnected() — Vérifier si l'IA a été déconnectée par le mode veille
// ─────────────────────────────────────────────────────────────────────────────
static bool sleepIsAIDisconnected() {
    return _sleepAIDisconnected;
}

// ─────────────────────────────────────────────────────────────────────────────
//  sleepCheckInactivity() — Machine à états de puissance
//
//  Appelé depuis loop() toutes les secondes.
//  Gère les transitions entre les états de puissance.
//
//  ACTIVE → IDLE_LIGHT (2 min) :
//    - Réduire télémétrie à 5s
//    - Désactiver I2S (micro)
//    - WiFi power save
//
//  IDLE_LIGHT → IDLE_DEEP (10 min) :
//    - Télémétrie à 30s
//    - Déconnecter OpenAI (proxy reste pour heartbeat)
//
//  IDLE_DEEP → DEEP_SLEEP (30 min) :
//    - Entrer en deep sleep avec timer wakeup
// ─────────────────────────────────────────────────────────────────────────────
static void sleepCheckInactivity(int throttleVal, int brakeVal,
                                  bool pttActive, bool aiActive,
                                  bool wsConnected, int throttleMin, int brakeMin) {
    // Détecter activité
    bool active = false;
    if (throttleVal > throttleMin + SLEEP_ADC_ACTIVITY_THRESH) active = true;
    if (brakeVal    > brakeMin    + SLEEP_ADC_ACTIVITY_THRESH) active = true;
    if (pttActive)  active = true;
    if (aiActive)   active = true;

    if (active) {
        sleepResetActivity();
        return;
    }

    // Calculer le temps d'inactivité
    uint32_t inactiveMs = millis() - _sleepLastActivity;

    // Transitions de la machine à états
    switch (_sleepPowerState) {
        case POWER_ACTIVE:
            if (inactiveMs >= IDLE_LIGHT_TIMEOUT_MS) {
                Serial.println("[sleep] transition → VEILLE LÉGÈRE (2 min inactif)");
                _sleepPowerState = POWER_IDLE_LIGHT;

                // Réduire la consommation : WiFi power save (seulement si WiFi actif)
                if (!_wsUseLte) {
                    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
                    Serial.println("[sleep] WiFi power save activé");
                }

                // Arrêter I2S (micro) pour économiser du courant
                audioStopI2S();
                _sleepI2SDisabled = true;
                Serial.println("[sleep] I2S arrêté");
            }
            break;

        case POWER_IDLE_LIGHT:
            if (inactiveMs >= IDLE_DEEP_TIMEOUT_MS) {
                Serial.println("[sleep] transition → VEILLE PROFONDE (10 min inactif)");
                _sleepPowerState = POWER_IDLE_DEEP;

                // Marquer l'IA comme devant être déconnectée
                _sleepAIDisconnected = true;
                Serial.println("[sleep] OpenAI sera déconnecté par le code appelant");
            }
            break;

        case POWER_IDLE_DEEP:
            if (inactiveMs >= SLEEP_TIMEOUT_MS) {
                Serial.printf("[sleep] inactif depuis %lu ms → DEEP SLEEP\n",
                              (unsigned long)inactiveMs);
                _sleepPowerState = POWER_SLEEP;
                sleepEnterDeepSleep();
                // Ne revient jamais ici
            }
            break;

        case POWER_SLEEP:
            // Ne devrait jamais arriver (on est en deep sleep)
            break;
    }
}
