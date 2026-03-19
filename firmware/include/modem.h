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

// Définir le modèle de modem AVANT d'inclure TinyGSM
// SIM7670E est compatible avec le driver SIM7600
#define TINY_GSM_MODEM_SIM7600
#include <TinyGsmClient.h>

// ── Instance globale du modem ────────────────────────────────────────────────
static TinyGsm       _modem(MODEM_UART);
static TinyGsmClient _modemClient(_modem);

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

    // Configurer les pins
    if (MODEM_PWRKEY_PIN >= 0) {
        pinMode(MODEM_PWRKEY_PIN, OUTPUT);
        digitalWrite(MODEM_PWRKEY_PIN, LOW);
    }
    if (MODEM_DTR_PIN >= 0) {
        pinMode(MODEM_DTR_PIN, OUTPUT);
        digitalWrite(MODEM_DTR_PIN, LOW);  // Mode actif (pas en veille)
    }

    // Initialiser l'UART du modem
    MODEM_UART.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    delay(100);

    // Pulse PWRKEY pour allumer le modem
    if (MODEM_PWRKEY_PIN >= 0) {
        Serial.println("[modem] pulse PWRKEY (allumage)...");
        digitalWrite(MODEM_PWRKEY_PIN, LOW);
        delay(100);
        digitalWrite(MODEM_PWRKEY_PIN, HIGH);
        delay(1000);
        digitalWrite(MODEM_PWRKEY_PIN, LOW);
    }

    // Attendre le boot du modem (5 secondes max)
    Serial.println("[modem] attente boot modem (5s)...");
    delay(5000);

    // Tester la communication AT
    Serial.println("[modem] test AT...");
    if (!_modem.testAT(5000)) {
        Serial.println("[modem] ERREUR : pas de réponse AT");
        return false;
    }

    // Initialiser le modem (reset + config de base)
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

    // Attendre l'enregistrement sur le réseau (timeout 60s)
    Serial.println("[modem] attente enregistrement réseau...");
    if (!_modem.waitForNetwork(60000)) {
        Serial.println("[modem] ERREUR : pas de réseau");
        return false;
    }
    Serial.println("[modem] réseau trouvé");

    // Connexion GPRS avec l'APN
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
