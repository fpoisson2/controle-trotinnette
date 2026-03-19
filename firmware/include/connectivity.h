#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  connectivity.h  —  Gestionnaire de connexion WiFi/LTE avec basculement
//
//  Stratégie de connexion :
//    1. Essayer WiFi d'abord (15s timeout)
//    2. Si WiFi échoue et LTE activé → basculer sur LTE
//    3. Vérification périodique (5s) de la connexion active
//    4. Quand sur LTE, sonder WiFi toutes les 30s pour revenir dessus
//
//  Le type de connexion détermine quel Client utiliser pour les WebSockets.
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "modem.h"

// ── Types de connexion ───────────────────────────────────────────────────────
enum ConnType {
    CONN_NONE = 0,    // Aucune connexion
    CONN_WIFI = 1,    // Connecté via WiFi
    CONN_LTE  = 2     // Connecté via LTE cellulaire
};

// ── État interne du gestionnaire ─────────────────────────────────────────────
static ConnType  _connType          = CONN_NONE;
static uint32_t  _connLastCheck     = 0;
static uint32_t  _connLastWifiProbe = 0;
static bool      _connInitialized   = false;

// Intervalle de sondage WiFi quand on est sur LTE (ms)
#define WIFI_PROBE_INTERVAL_MS  30000

// ─────────────────────────────────────────────────────────────────────────────
//  _connTryWifi() — Tenter une connexion WiFi (bloquant avec timeout)
// ─────────────────────────────────────────────────────────────────────────────
static bool _connTryWifi(uint32_t timeoutMs) {
    Serial.println("[conn] tentative WiFi...");
    WiFi.disconnect(true);
    delay(50);
    WiFi.mode(WIFI_STA);

#if WIFI_ENTERPRISE
    // WPA2-Enterprise (EAP-PEAP)
    esp_wifi_sta_wpa2_ent_set_identity(
        (const uint8_t *)EAP_IDENTITY, strlen(EAP_IDENTITY));
    esp_wifi_sta_wpa2_ent_set_username(
        (const uint8_t *)EAP_USERNAME, strlen(EAP_USERNAME));
    esp_wifi_sta_wpa2_ent_set_password(
        (const uint8_t *)EAP_PASSWORD, strlen(EAP_PASSWORD));
    esp_wifi_sta_wpa2_ent_enable();
    WiFi.begin(WIFI_SSID);
#else
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
#endif

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeoutMs) {
            Serial.println("[conn] WiFi timeout");
            WiFi.disconnect(true);
            return false;
        }
        delay(250);
    }

    Serial.printf("[conn] WiFi connecté : %s\n", WiFi.localIP().toString().c_str());
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  _connTryLte() — Tenter une connexion LTE (bloquant avec timeout)
// ─────────────────────────────────────────────────────────────────────────────
#if LTE_ENABLED
static bool _connTryLte() {
    Serial.println("[conn] tentative LTE...");

    // Initialiser le modem si pas encore fait
    if (!modemInit()) {
        Serial.println("[conn] LTE : échec init modem");
        return false;
    }

    // Connecter au réseau cellulaire
    if (!modemConnect()) {
        Serial.println("[conn] LTE : échec connexion");
        return false;
    }

    Serial.println("[conn] LTE connecté");
    return true;
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  connInit() — Initialiser la connectivité (WiFi d'abord, puis LTE)
//
//  Appelé une seule fois dans setup().
//  Retourne le type de connexion obtenu.
// ─────────────────────────────────────────────────────────────────────────────
static ConnType connInit() {
    _connInitialized = true;
    _connLastCheck   = millis();

    // Essayer WiFi en premier (timeout configurable)
    if (_connTryWifi(WIFI_TIMEOUT_MS)) {
        _connType = CONN_WIFI;
        return _connType;
    }

    // WiFi échoué → essayer LTE si activé
#if LTE_ENABLED
    Serial.println("[conn] WiFi échoué, basculement vers LTE...");
    if (_connTryLte()) {
        _connType = CONN_LTE;
        return _connType;
    }
#endif

    // Aucune connexion possible
    Serial.println("[conn] AUCUNE connexion disponible");
    _connType = CONN_NONE;
    return _connType;
}

// ─────────────────────────────────────────────────────────────────────────────
//  connCheck() — Vérification périodique de la connectivité
//
//  Appelé dans loop(). Vérifie toutes les CONN_CHECK_INTERVAL_MS :
//    - Si connecté WiFi : vérifier que WiFi est toujours actif
//    - Si connecté LTE  : vérifier que GPRS est toujours actif + sonder WiFi
//    - Si déconnecté    : tenter reconnexion WiFi puis LTE
//
//  Retourne le type de connexion actuel après vérification.
// ─────────────────────────────────────────────────────────────────────────────
static ConnType connCheck() {
    if (!_connInitialized) return CONN_NONE;

    uint32_t now = millis();
    if (now - _connLastCheck < CONN_CHECK_INTERVAL_MS) return _connType;
    _connLastCheck = now;

    switch (_connType) {
        case CONN_WIFI:
            // Vérifier que le WiFi est toujours connecté
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[conn] WiFi perdu !");
                _connType = CONN_NONE;

                // Tenter reconnexion rapide (5s)
                if (_connTryWifi(5000)) {
                    _connType = CONN_WIFI;
                }
#if LTE_ENABLED
                else if (_connTryLte()) {
                    _connType = CONN_LTE;
                }
#endif
            }
            break;

#if LTE_ENABLED
        case CONN_LTE:
            // Vérifier que le LTE est toujours connecté
            if (!modemIsConnected()) {
                Serial.println("[conn] LTE perdu !");
                _connType = CONN_NONE;

                // Tenter WiFi d'abord (préféré)
                if (_connTryWifi(WIFI_TIMEOUT_MS)) {
                    _connType = CONN_WIFI;
                    modemPowerOff();  // Éteindre le modem si WiFi retrouvé
                } else if (modemConnect()) {
                    _connType = CONN_LTE;
                }
            } else {
                // Sonder WiFi périodiquement pour revenir dessus (moins énergivore)
                if (now - _connLastWifiProbe > WIFI_PROBE_INTERVAL_MS) {
                    _connLastWifiProbe = now;
                    Serial.println("[conn] sondage WiFi depuis LTE...");

                    // Scan rapide non-bloquant : vérifier si le SSID est visible
                    int n = WiFi.scanNetworks(false, false, false, 300);
                    bool ssidFound = false;
                    for (int i = 0; i < n; i++) {
                        if (WiFi.SSID(i) == WIFI_SSID) {
                            ssidFound = true;
                            break;
                        }
                    }
                    WiFi.scanDelete();

                    if (ssidFound) {
                        Serial.println("[conn] WiFi détecté ! Basculement...");
                        if (_connTryWifi(WIFI_TIMEOUT_MS)) {
                            _connType = CONN_WIFI;
                            modemDisconnect();
                            modemPowerOff();
                            Serial.println("[conn] retour sur WiFi, modem éteint");
                        }
                    }
                }
            }
            break;
#endif

        case CONN_NONE:
            // Tenter reconnexion
            if (_connTryWifi(WIFI_TIMEOUT_MS)) {
                _connType = CONN_WIFI;
            }
#if LTE_ENABLED
            else if (_connTryLte()) {
                _connType = CONN_LTE;
            }
#endif
            break;
    }

    return _connType;
}

// ─────────────────────────────────────────────────────────────────────────────
//  connGetType() — Retourner le type de connexion actuel
// ─────────────────────────────────────────────────────────────────────────────
static ConnType connGetType() {
    return _connType;
}

// Setter pour main.cpp (quand connInit/connCheck ne sont pas utilisés)
static void connSetType(ConnType t) {
    _connType = t;
}

// ─────────────────────────────────────────────────────────────────────────────
//  connGetRSSI() — Force du signal pour la connexion active
//
//  WiFi : RSSI en dBm (typiquement -30 à -90)
//  LTE  : qualité signal 0-31 convertie en pseudo-dBm
// ─────────────────────────────────────────────────────────────────────────────
// Cache RSSI LTE (mis à jour par Core 1 via connUpdateLteRSSI)
static volatile int _lteRssiCache = -999;

static int connGetRSSI() {
    switch (_connType) {
        case CONN_WIFI:
            return WiFi.RSSI();
#if LTE_ENABLED
        case CONN_LTE:
            // Pas d'appel AT ici ! Retourner le cache mis à jour par Core 1
            return _lteRssiCache;
#endif
        default:
            return -999;
    }
}

// Appelé depuis Core 1 uniquement pour mettre à jour le RSSI LTE
static void connUpdateLteRSSI() {
#if LTE_ENABLED
    if (_connType == CONN_LTE) {
        int sq = modemSignalQuality();
        _lteRssiCache = -113 + (sq * 2);
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
//  connGetTypeName() — Nom lisible du type de connexion
// ─────────────────────────────────────────────────────────────────────────────
static const char* connGetTypeName() {
    switch (_connType) {
        case CONN_WIFI: return "WiFi";
        case CONN_LTE:  return "LTE";
        default:        return "Aucun";
    }
}
