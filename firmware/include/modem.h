#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  modem.h  —  Gestion du modem LTE SIM7670E via TinyGSM
//
//  Le modem SIM7670E est intégré au LilyGO T-A7670E.
//  Communication via UART (Serial1) avec commandes AT.
//  Gardé derrière #if LTE_ENABLED pour ne pas compiler si inutilisé.
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include "config.h"

#if LTE_ENABLED

// TINY_GSM_MODEM_A76XXSSL défini via build_flags → active TinyGsmClientSecure
#include <TinyGsmClient.h>

// ── Instances globales du modem ─────────────────────────────────────────────
// Déclarées ici, définies UNE SEULE FOIS grâce à la garde MODEM_DEFINE_GLOBALS
// (posée dans main.cpp avant l'include)
#ifdef MODEM_DEFINE_GLOBALS
TinyGsm              _modem(MODEM_UART);
TinyGsmClient        _modemClient(_modem, 1);
TinyGsmClientSecure  _modemClientSSL(_modem, 0);
#else
extern TinyGsm              _modem;
extern TinyGsmClient        _modemClient;
extern TinyGsmClientSecure  _modemClientSSL;
#endif

// ── Drapeaux d'état ──────────────────────────────────────────────────────────
static bool _modemInitialized = false;
static bool _modemConnected   = false;

// ─────────────────────────────────────────────────────────────────────────────
//  modemInit() — Allumer le modem via PWRKEY et initialiser les commandes AT
//
//  Séquence PWRKEY du SIM7670E :
//    1. LOW pendant 100ms (état initial)
//    2. HIGH pendant 1000ms (pulse d'allumage)
//    3. LOW (relâcher)
//    4. Attendre ~5s pour le boot du modem
// ─────────────────────────────────────────────────────────────────────────────
static bool modemInit() {
    Serial.println("[modem] initialisation SIM7670E...");

    // Alimenter le modem via BOARD_POWERON_PIN (GPIO 12)
#ifdef BOARD_POWERON_PIN
    pinMode(BOARD_POWERON_PIN, OUTPUT);
    digitalWrite(BOARD_POWERON_PIN, HIGH);
#endif

    // DTR LOW = mode actif (pas en veille)
#if MODEM_DTR_PIN >= 0
    pinMode(MODEM_DTR_PIN, OUTPUT);
    digitalWrite(MODEM_DTR_PIN, LOW);
#endif

    // Initialiser l'UART du modem
    MODEM_UART.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    delay(100);

    // ── Vérifier si le modem répond déjà (évite un cycle PWRKEY inutile) ────
    Serial.println("[modem] test AT rapide...");
    if (_modem.testAT(3000)) {
        Serial.println("[modem] modem déjà actif — pas de PWRKEY");
    } else {
        // Le modem ne répond pas → séquence d'allumage PWRKEY
        Serial.println("[modem] pas de réponse — pulse PWRKEY...");
        pinMode(MODEM_PWRKEY_PIN, OUTPUT);
        digitalWrite(MODEM_PWRKEY_PIN, LOW);
        delay(100);
        digitalWrite(MODEM_PWRKEY_PIN, HIGH);
        delay(MODEM_POWERON_PULSE_WIDTH_MS);
        digitalWrite(MODEM_PWRKEY_PIN, LOW);

        // Attendre AT OK avec retry (re-pulse PWRKEY si nécessaire)
        Serial.println("[modem] attente réponse AT...");
        uint32_t atStart = millis();
        int retry = 0;
        while (!_modem.testAT(1000)) {
            Serial.print(".");
            if (retry++ > 30) {
                Serial.println("\n[modem] re-pulse PWRKEY...");
                digitalWrite(MODEM_PWRKEY_PIN, LOW);
                delay(100);
                digitalWrite(MODEM_PWRKEY_PIN, HIGH);
                delay(MODEM_POWERON_PULSE_WIDTH_MS);
                digitalWrite(MODEM_PWRKEY_PIN, LOW);
                retry = 0;
            }
            if (millis() - atStart > 60000) {
                Serial.println("\n[modem] ERREUR : pas de réponse AT après 60s");
                return false;
            }
        }
        Serial.printf("\n[modem] AT OK après %lu ms\n", millis() - atStart);
    }

    // Initialiser le modem (config de base)
    Serial.println("[modem] init()...");
    if (!_modem.init()) {
        Serial.println("[modem] ERREUR : init() échoué");
        return false;
    }

    // Afficher les informations du modem
    String modemInfo = _modem.getModemInfo();
    Serial.printf("[modem] info : %s\n", modemInfo.c_str());

    String imei = _modem.getIMEI();
    Serial.printf("[modem] IMEI : %s\n", imei.c_str());

    // Attendre que la SIM soit prête (peut prendre quelques secondes après le boot)
    Serial.println("[modem] attente SIM prête...");
    for (int i = 0; i < 10; i++) {
        SimStatus simSt = _modem.getSimStatus();
        if (simSt == SIM_READY) {
            Serial.println("[modem] SIM prête");
            break;
        }
        Serial.printf("[modem] SIM status=%d, retry %d/10...\n", (int)simSt, i + 1);
        delay(2000);
    }

    _modemInitialized = true;
    Serial.println("[modem] initialisé avec succès");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  modemConnect() — Connexion au réseau cellulaire avec l'APN configuré
// ─────────────────────────────────────────────────────────────────────────────
static bool modemConnect() {
    if (!_modemInitialized) {
        Serial.println("[modem] ERREUR : modem non initialisé");
        return false;
    }

    Serial.printf("[modem] connexion au réseau (APN: %s)...\n", LTE_APN);

    // ── Vérifier si déjà connecté au réseau (après un soft reset ESP32) ─────
    if (_modem.isNetworkConnected()) {
        Serial.println("[modem] réseau déjà enregistré");
        if (_modem.isGprsConnected()) {
            Serial.println("[modem] GPRS déjà actif — reconnexion rapide");
            _modemConnected = true;
            String ip = _modem.getLocalIP();
            Serial.printf("[modem] connecté — IP : %s\n", ip.c_str());
            return true;
        }
        Serial.println("[modem] réseau OK mais GPRS inactif — activation...");
    } else {
        // Forcer le mode complet (radio ON) et configurer l'APN
        _modem.sendAT("+CFUN=1");
        _modem.waitResponse(3000);
        {
            char cgdcont[128];
            snprintf(cgdcont, sizeof(cgdcont), "+CGDCONT=1,\"IP\",\"%s\"", LTE_APN);
            _modem.sendAT(cgdcont);
            _modem.waitResponse();
        }
        // Recherche automatique d'opérateur
        _modem.sendAT("+COPS=0");
        _modem.waitResponse(5000);
        // Attendre un peu que la radio trouve le réseau
        delay(5000);

        // Attendre l'enregistrement sur le réseau (timeout 120s)
        Serial.println("[modem] attente enregistrement réseau...");
        if (!_modem.waitForNetwork(120000)) {
            Serial.println("[modem] ERREUR : pas de réseau");
            return false;
        }
    }
    Serial.println("[modem] réseau trouvé");

    // Debug : afficher opérateur et signal avant connexion
    String op = _modem.getOperator();
    Serial.printf("[modem] opérateur : %s\n", op.c_str());
    int16_t sq = _modem.getSignalQuality();
    Serial.printf("[modem] signal : %d/31\n", sq);

    // Connexion GPRS avec l'APN
    Serial.printf("[modem] gprsConnect APN='%s'...\n", LTE_APN);
    if (!_modem.gprsConnect(LTE_APN, "", "")) {
        Serial.println("[modem] ERREUR : connexion GPRS échouée");
        return false;
    }

    // Vérifier la connexion
    if (!_modem.isGprsConnected()) {
        Serial.println("[modem] ERREUR : GPRS non connecté après gprsConnect");
        return false;
    }

    _modemConnected = true;
    String ip = _modem.getLocalIP();
    Serial.printf("[modem] connecté — IP : %s\n", ip.c_str());
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  modemDisconnect() — Déconnecter le GPRS
// ─────────────────────────────────────────────────────────────────────────────
static void modemDisconnect() {
    if (_modemConnected) {
        Serial.println("[modem] déconnexion GPRS...");
        _modem.gprsDisconnect();
        _modemConnected = false;
        Serial.println("[modem] GPRS déconnecté");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  modemPowerOff() — Éteindre le modem (économie d'énergie)
// ─────────────────────────────────────────────────────────────────────────────
static void modemPowerOff() {
    Serial.println("[modem] extinction...");
    modemDisconnect();

    // Commande AT d'extinction propre
    _modem.poweroff();

    _modemInitialized = false;
    Serial.println("[modem] éteint");
}

// ─────────────────────────────────────────────────────────────────────────────
//  modemSignalQuality() — Qualité du signal (0-31, 99=inconnu)
//  Valeurs typiques : 0-9 marginal, 10-14 OK, 15-19 bon, 20-31 excellent
// ─────────────────────────────────────────────────────────────────────────────
static int modemSignalQuality() {
    if (!_modemInitialized) return 0;
    int16_t sq = _modem.getSignalQuality();
    if (sq == 99) return 0;  // 99 = inconnu → traiter comme 0
    return (int)sq;
}

// ─────────────────────────────────────────────────────────────────────────────
//  modemIsConnected() — Vérifier si le modem est connecté au réseau
// ─────────────────────────────────────────────────────────────────────────────
static bool modemIsConnected() {
    if (!_modemInitialized) return false;
    _modemConnected = _modem.isGprsConnected();
    return _modemConnected;
}

// ─────────────────────────────────────────────────────────────────────────────
//  modemGetClient() — Obtenir le client TCP/IP pour les connexions réseau
// ─────────────────────────────────────────────────────────────────────────────
static TinyGsmClient& modemGetClient() {
    return _modemClient;
}

#else  // LTE_ENABLED == false

// ── Stubs vides quand LTE est désactivé ─────────────────────────────────────
static bool modemInit()          { return false; }
static bool modemConnect()       { return false; }
static void modemDisconnect()    {}
static void modemPowerOff()      {}
static int  modemSignalQuality() { return 0; }
static bool modemIsConnected()   { return false; }

#endif  // LTE_ENABLED
