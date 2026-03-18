#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  sleep.h  —  Deep sleep et réveil automatique par manette/frein
//
//  Sources de réveil :
//    - GPIO 32 (throttle ADC) : l'utilisateur touche la manette
//    - GPIO 39 (brake ADC)    : l'utilisateur touche le frein
//    - Timer (optionnel)      : réveil périodique pour heartbeat
//
//  Au réveil : jingle DAC GPIO 25, puis reconnexion WiFi + WebSocket
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <driver/dac.h>
#include <driver/adc.h>

// ── Configuration deep sleep ─────────────────────────────────────────────────

// Délai d'inactivité avant mise en veille (ms)
// Aucune activité = pas de throttle, pas de frein, pas de PTT, pas de commande IA
#define SLEEP_INACTIVITY_MS        120000   // 2 minutes

// Seuil ADC au-dessus duquel on considère la manette/frein comme "active"
// Utilisé pour le monitoring d'inactivité (pas pour le wake — le wake utilise ext1)
#define SLEEP_ADC_ACTIVITY_THRESH  200

// Réveil périodique optionnel (0 = désactivé)
// Utile pour envoyer un heartbeat au proxy même en veille profonde
#define SLEEP_TIMER_WAKEUP_US      0  // en microsecondes (ex: 300000000 = 5 min)

// ── Jingle de réveil (DAC GPIO 25) ──────────────────────────────────────────
// Trois notes montantes : Do-Mi-Sol (C5-E5-G5) = ~523-659-784 Hz
// Joué avant toute initialisation lourde (WiFi, I2S, etc.)

static void sleepPlayBootJingle() {
    // Activer le DAC manuellement (pas encore initialisé par audio.h)
    dac_output_enable(DAC_CHANNEL_1);
    dac_output_voltage(DAC_CHANNEL_1, 128);  // silence DC

    // Notes : fréquence (Hz), durée (ms)
    struct Note { float freq; uint32_t dur_ms; };
    const Note melody[] = {
        { 523.0f,  80 },   // Do5
        { 659.0f,  80 },   // Mi5
        { 784.0f, 120 },   // Sol5 (tenu un peu plus)
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

// ── Jingle de mise en veille (notes descendantes) ───────────────────────────
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

// ── Déterminer la cause du réveil ───────────────────────────────────────────
static const char* sleepGetWakeReason() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
        case ESP_SLEEP_WAKEUP_EXT0:     return "ext0 (throttle GPIO32)";
        case ESP_SLEEP_WAKEUP_EXT1:     return "ext1 (throttle/frein)";
        case ESP_SLEEP_WAKEUP_TIMER:    return "timer";
        case ESP_SLEEP_WAKEUP_TOUCHPAD: return "touchpad";
        case ESP_SLEEP_WAKEUP_ULP:      return "ULP";
        default:                        return "power-on / reset";
    }
}

static bool sleepIsWakeFromDeepSleep() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    return (cause != ESP_SLEEP_WAKEUP_UNDEFINED);
}

// ── Configurer les sources de réveil et entrer en deep sleep ────────────────
//
// GPIO 32 (RTC GPIO 9)  = throttle — source principale
// GPIO 39 (RTC GPIO 3)  = frein    — source secondaire
//
// On utilise ext1 avec masque sur les deux GPIOs.
// ext1 supporte le mode ESP_EXT1_WAKEUP_ANY_HIGH : réveil si l'un des GPIOs
// passe à HIGH. Comme le throttle et le frein sont des capteurs Hall qui
// montent en tension quand on les actionne, c'est le mode approprié.
//
// Note : GPIO 32 a un pull-down interne activé dans setup(). En deep sleep,
// les pulls RTC sont configurés séparément via rtc_gpio_pulldown_en().

static void sleepEnterDeepSleep() {
    Serial.println("[sleep] préparation deep sleep...");
    Serial.flush();

    // Jouer le jingle de mise en veille
    sleepPlaySleepJingle();

    // Désactiver WiFi et Bluetooth pour économiser
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(10);

    // Configurer les GPIOs RTC pour le réveil
    // GPIO 32 = RTC_GPIO_NUM_9 (throttle)
    // GPIO 39 = RTC_GPIO_NUM_3 (frein) — input-only, pas de pull interne possible

    // Isoler les GPIOs non utilisés pour minimiser la consommation
    // (les GPIOs I2S, UART ESC, etc. ne sont pas nécessaires en veille)

    // Configurer ext1 : réveil quand GPIO 32 OU GPIO 39 passe à HIGH
    // Masque de bits : bit 32 = (1ULL << 32), bit 39 = (1ULL << 39)
    uint64_t wakeupMask = (1ULL << 32) | (1ULL << 39);
    esp_sleep_enable_ext1_wakeup(wakeupMask, ESP_EXT1_WAKEUP_ANY_HIGH);

    // Pull-down sur GPIO 32 en mode RTC (maintenu pendant le deep sleep)
    // Assure que le GPIO reste LOW quand la manette est au repos
    rtc_gpio_pulldown_en(GPIO_NUM_32);
    rtc_gpio_pullup_dis(GPIO_NUM_32);

    // GPIO 39 : input-only, pas de pull-up/down interne sur ce GPIO
    // Le circuit externe (capteur Hall frein) fournit le niveau de repos

    // Timer de réveil optionnel (heartbeat périodique)
#if SLEEP_TIMER_WAKEUP_US > 0
    esp_sleep_enable_timer_wakeup(SLEEP_TIMER_WAKEUP_US);
#endif

    Serial.println("[sleep] entrée en deep sleep — réveil par manette ou frein");
    Serial.flush();
    delay(50);

    esp_deep_sleep_start();
    // --- Le programme ne continue jamais ici ---
    // Au réveil, setup() est appelé depuis le début
}

// ── Moniteur d'inactivité ───────────────────────────────────────────────────
// Appelé depuis loop() — suit l'activité et déclenche le deep sleep après
// SLEEP_INACTIVITY_MS d'inactivité complète.
//
// Activité = toute action qui remet le compteur à zéro :
//   - Throttle au-dessus du seuil
//   - Frein au-dessus du seuil
//   - Bouton PTT enfoncé
//   - Commande IA reçue
//   - WebSocket proxy connecté et données échangées

static uint32_t sleepLastActivity = 0;

static void sleepResetActivity() {
    sleepLastActivity = millis();
}

// Vérifier si le système est inactif et doit dormir
// throttleVal / brakeVal : lectures ADC brutes actuelles
// pttActive : bouton push-to-talk enfoncé
// aiActive : commande IA récente
// wsConnected : WebSocket proxy connecté
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
        sleepLastActivity = millis();
        return;
    }

    // Vérifier timeout d'inactivité
    if (millis() - sleepLastActivity >= SLEEP_INACTIVITY_MS) {
        Serial.printf("[sleep] inactif depuis %lu ms — mise en veille\n",
                      (unsigned long)(millis() - sleepLastActivity));
        sleepEnterDeepSleep();
    }
}
