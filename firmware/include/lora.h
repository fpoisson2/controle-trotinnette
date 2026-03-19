#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  lora.h  —  Communication LoRa 915 MHz via module RYLR896 (commandes AT)
//
//  Le RYLR896 est un module LoRa simple piloté par UART avec commandes AT.
//  Utilisé comme lien de secours quand WiFi et LTE sont indisponibles.
//
//  Protocole binaire compact :
//    [MAC 6 octets][Type 1][Payload N][CRC16 2]
//
//  Types de messages :
//    0x01 = Heartbeat (pas de payload)
//    0x02 = Télémétrie (vitesse, tension, GPS)
//    0x03 = Commande (du serveur vers la trottinette)
//    0x04 = Arrêt d'urgence
//    0x05 = Verrouillage/déverrouillage
//    0x06 = Accusé de réception (ACK)
//
//  Gardé derrière #if LORA_ENABLED.
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include "config.h"

#if LORA_ENABLED

// ── Types de messages LoRa ───────────────────────────────────────────────────
#define LORA_MSG_HEARTBEAT      0x01
#define LORA_MSG_TELEMETRY      0x02
#define LORA_MSG_COMMAND         0x03
#define LORA_MSG_EMERGENCY       0x04
#define LORA_MSG_LOCK            0x05
#define LORA_MSG_ACK             0x06

// Taille maximale d'un message LoRa (limite RYLR896 = 240 octets)
#define LORA_MAX_PAYLOAD    64

// Adresse LoRa du module (configurable)
#define LORA_ADDRESS        1
#define LORA_NETWORK_ID     6

// ── UART pour le module LoRa ─────────────────────────────────────────────────
// On utilise Serial2 si disponible, sinon SoftwareSerial
// Note : Serial2 est aussi utilisé par l'ESC (pins 13/14).
// Si les pins LoRa sont différentes de l'ESC, on peut utiliser SoftwareSerial.
#include <SoftwareSerial.h>
static SoftwareSerial _loraSerial(LORA_RX_PIN, LORA_TX_PIN);

// ── État interne LoRa ────────────────────────────────────────────────────────
static bool     _loraInitialized = false;
static uint32_t _loraLastAck     = 0;
static uint32_t _loraLastSend    = 0;

// ── Buffer de réception ──────────────────────────────────────────────────────
#define LORA_RX_BUF_SIZE    128
static char     _loraRxBuf[LORA_RX_BUF_SIZE];
static uint8_t  _loraRxLen = 0;

// ─────────────────────────────────────────────────────────────────────────────
//  CRC16 simplifié pour le protocole LoRa (même algo que Flipsky pour cohérence)
// ─────────────────────────────────────────────────────────────────────────────
static uint16_t _loraCrc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

// ─────────────────────────────────────────────────────────────────────────────
//  _loraSendAT() — Envoyer une commande AT et attendre la réponse
// ─────────────────────────────────────────────────────────────────────────────
static bool _loraSendAT(const char* cmd, const char* expected, uint32_t timeoutMs = 2000) {
    _loraSerial.print(cmd);
    _loraSerial.print("\r\n");

    uint32_t start = millis();
    String response = "";
    while (millis() - start < timeoutMs) {
        if (_loraSerial.available()) {
            char c = _loraSerial.read();
            response += c;
            if (response.indexOf(expected) >= 0) {
                return true;
            }
        }
        delay(1);
    }
    Serial.printf("[lora] AT timeout : %s → %s\n", cmd, response.c_str());
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  loraInit() — Initialiser l'UART et configurer le module RYLR896
//
//  Configuration :
//    - Fréquence : 915 MHz (bande ISM Amérique du Nord)
//    - Bande passante : 125 kHz
//    - Spreading Factor : 9 (compromis portée/débit)
//    - Puissance : 15 dBm
// ─────────────────────────────────────────────────────────────────────────────
static bool loraInit() {
    Serial.println("[lora] initialisation RYLR896...");

    _loraSerial.begin(LORA_BAUD);
    delay(100);

    // Test de communication
    if (!_loraSendAT("AT", "+OK")) {
        Serial.println("[lora] ERREUR : pas de réponse AT");
        return false;
    }

    // Configurer l'adresse du module
    char addrCmd[32];
    snprintf(addrCmd, sizeof(addrCmd), "AT+ADDRESS=%d", LORA_ADDRESS);
    if (!_loraSendAT(addrCmd, "+OK")) {
        Serial.println("[lora] ERREUR : config adresse");
        return false;
    }

    // Configurer le network ID
    char netCmd[32];
    snprintf(netCmd, sizeof(netCmd), "AT+NETWORKID=%d", LORA_NETWORK_ID);
    if (!_loraSendAT(netCmd, "+OK")) {
        Serial.println("[lora] ERREUR : config network ID");
        return false;
    }

    // Configurer les paramètres RF
    // AT+PARAMETER=<SF>,<BW>,<CR>,<Preamble>
    // SF=9, BW=7(125kHz), CR=1(4/5), Preamble=12
    if (!_loraSendAT("AT+PARAMETER=9,7,1,12", "+OK")) {
        Serial.println("[lora] ERREUR : config RF");
        return false;
    }

    // Configurer la bande de fréquence (915 MHz)
    if (!_loraSendAT("AT+BAND=915000000", "+OK")) {
        Serial.println("[lora] ERREUR : config fréquence");
        return false;
    }

    _loraInitialized = true;
    _loraLastAck = millis();
    Serial.println("[lora] initialisé — 915 MHz, SF9, 125kHz");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  _loraSendPacket() — Envoyer un paquet binaire via commande AT+SEND
//
//  Format AT : AT+SEND=<address>,<length>,<data>\r\n
//  Les données binaires sont encodées en hexadécimal pour la commande AT.
// ─────────────────────────────────────────────────────────────────────────────
static bool _loraSendPacket(uint8_t destAddr, const uint8_t* data, size_t len) {
    if (!_loraInitialized || len == 0 || len > LORA_MAX_PAYLOAD) return false;

    // Encoder en hexadécimal
    char hexBuf[LORA_MAX_PAYLOAD * 2 + 1];
    for (size_t i = 0; i < len; i++) {
        snprintf(hexBuf + i * 2, 3, "%02X", data[i]);
    }

    // Construire et envoyer la commande AT
    char cmd[LORA_MAX_PAYLOAD * 2 + 32];
    snprintf(cmd, sizeof(cmd), "AT+SEND=%d,%d,%s", destAddr, (int)len, hexBuf);

    return _loraSendAT(cmd, "+OK", 3000);
}

// ─────────────────────────────────────────────────────────────────────────────
//  loraSendHeartbeat() — Envoyer un heartbeat compact
//
//  Payload : [MAC 6][Type=0x01][CRC16 2] = 9 octets
// ─────────────────────────────────────────────────────────────────────────────
static bool loraSendHeartbeat(const uint8_t* mac) {
    uint8_t pkt[9];

    // MAC (6 octets)
    memcpy(pkt, mac, 6);
    // Type
    pkt[6] = LORA_MSG_HEARTBEAT;
    // CRC16 sur MAC + Type
    uint16_t crc = _loraCrc16(pkt, 7);
    pkt[7] = (uint8_t)(crc >> 8);
    pkt[8] = (uint8_t)(crc & 0xFF);

    return _loraSendPacket(0, pkt, sizeof(pkt));
}

// ─────────────────────────────────────────────────────────────────────────────
//  loraSendTelemetry() — Envoyer la télémétrie compacte
//
//  Payload : [MAC 6][Type=0x02][Speed 2][Voltage 2][Lat 4][Lon 4][CRC16 2] = 22 octets
//  Speed et Voltage sont en unités de 0.1 (ex: 253 = 25.3 km/h)
//  Lat/Lon en degrés * 1e6 (entier 32 bits signé)
// ─────────────────────────────────────────────────────────────────────────────
static bool loraSendTelemetry(const uint8_t* mac, float speed, float voltage,
                               float lat, float lon) {
    uint8_t pkt[22];
    int idx = 0;

    // MAC (6 octets)
    memcpy(pkt, mac, 6);
    idx = 6;

    // Type
    pkt[idx++] = LORA_MSG_TELEMETRY;

    // Vitesse (uint16, unité 0.1 km/h)
    uint16_t spd = (uint16_t)(speed * 10.0f);
    pkt[idx++] = (uint8_t)(spd >> 8);
    pkt[idx++] = (uint8_t)(spd & 0xFF);

    // Tension (uint16, unité 0.1 V)
    uint16_t volt = (uint16_t)(voltage * 10.0f);
    pkt[idx++] = (uint8_t)(volt >> 8);
    pkt[idx++] = (uint8_t)(volt & 0xFF);

    // Latitude (int32, degrés * 1e6)
    int32_t iLat = (int32_t)(lat * 1000000.0f);
    pkt[idx++] = (uint8_t)(iLat >> 24);
    pkt[idx++] = (uint8_t)(iLat >> 16);
    pkt[idx++] = (uint8_t)(iLat >> 8);
    pkt[idx++] = (uint8_t)(iLat & 0xFF);

    // Longitude (int32, degrés * 1e6)
    int32_t iLon = (int32_t)(lon * 1000000.0f);
    pkt[idx++] = (uint8_t)(iLon >> 24);
    pkt[idx++] = (uint8_t)(iLon >> 16);
    pkt[idx++] = (uint8_t)(iLon >> 8);
    pkt[idx++] = (uint8_t)(iLon & 0xFF);

    // CRC16 sur tout sauf les 2 derniers octets
    uint16_t crc = _loraCrc16(pkt, idx);
    pkt[idx++] = (uint8_t)(crc >> 8);
    pkt[idx++] = (uint8_t)(crc & 0xFF);

    return _loraSendPacket(0, pkt, idx);
}

// ─────────────────────────────────────────────────────────────────────────────
//  loraSendEmergencyStop() — Envoyer un arrêt d'urgence (prioritaire)
//
//  Payload : [MAC 6][Type=0x04][CRC16 2] = 9 octets
// ─────────────────────────────────────────────────────────────────────────────
static bool loraSendEmergencyStop(const uint8_t* mac) {
    uint8_t pkt[9];

    memcpy(pkt, mac, 6);
    pkt[6] = LORA_MSG_EMERGENCY;
    uint16_t crc = _loraCrc16(pkt, 7);
    pkt[7] = (uint8_t)(crc >> 8);
    pkt[8] = (uint8_t)(crc & 0xFF);

    Serial.println("[lora] ENVOI ARRET D'URGENCE !");
    return _loraSendPacket(0, pkt, sizeof(pkt));
}

// ─────────────────────────────────────────────────────────────────────────────
//  loraReceive() — Vérifier les messages entrants
//
//  Format reçu du RYLR896 : +RCV=<address>,<length>,<data>,<RSSI>,<SNR>\r\n
//  Les données sont en hexadécimal.
//
//  Retourne le type de message reçu (0 si rien ou erreur).
//  Les données du message sont décodées dans outPayload/outLen.
// ─────────────────────────────────────────────────────────────────────────────

// Structure pour les commandes reçues via LoRa
struct LoraCommand {
    uint8_t type;           // Type de message (LORA_MSG_*)
    uint8_t payload[32];    // Payload décodé
    uint8_t payloadLen;     // Longueur du payload
    bool    valid;          // CRC vérifié
};

static LoraCommand loraReceive() {
    LoraCommand result = { 0, {0}, 0, false };

    if (!_loraInitialized) return result;

    // Lire les caractères disponibles
    while (_loraSerial.available() && _loraRxLen < LORA_RX_BUF_SIZE - 1) {
        char c = _loraSerial.read();
        _loraRxBuf[_loraRxLen++] = c;

        // Chercher fin de ligne
        if (c == '\n') {
            _loraRxBuf[_loraRxLen] = '\0';

            // Parser le message +RCV=
            if (strncmp(_loraRxBuf, "+RCV=", 5) == 0) {
                // +RCV=<addr>,<len>,<hexdata>,<rssi>,<snr>\r\n
                char* p = _loraRxBuf + 5;

                // Sauter l'adresse
                char* comma1 = strchr(p, ',');
                if (!comma1) { _loraRxLen = 0; return result; }

                // Lire la longueur
                p = comma1 + 1;
                int dataLen = atoi(p);
                char* comma2 = strchr(p, ',');
                if (!comma2 || dataLen <= 0) { _loraRxLen = 0; return result; }

                // Lire les données hexadécimales
                p = comma2 + 1;
                uint8_t rawData[LORA_MAX_PAYLOAD];
                int rawLen = 0;
                for (int i = 0; i < dataLen * 2 && p[i] && p[i] != ','; i += 2) {
                    char hex[3] = { p[i], p[i+1], '\0' };
                    rawData[rawLen++] = (uint8_t)strtol(hex, NULL, 16);
                    if (rawLen >= LORA_MAX_PAYLOAD) break;
                }

                // Valider la trame : minimum MAC(6) + Type(1) + CRC(2) = 9 octets
                if (rawLen >= 9) {
                    // Vérifier le CRC
                    uint16_t crcCalc = _loraCrc16(rawData, rawLen - 2);
                    uint16_t crcRecv = ((uint16_t)rawData[rawLen - 2] << 8) |
                                        rawData[rawLen - 1];

                    if (crcCalc == crcRecv) {
                        result.type = rawData[6];  // Type après les 6 octets MAC
                        result.valid = true;

                        // Copier le payload (après MAC+Type, avant CRC)
                        int payloadStart = 7;
                        int payloadEnd   = rawLen - 2;
                        result.payloadLen = payloadEnd - payloadStart;
                        if (result.payloadLen > 0 && result.payloadLen <= 32) {
                            memcpy(result.payload, rawData + payloadStart, result.payloadLen);
                        }

                        // Mettre à jour le timestamp du dernier ACK
                        _loraLastAck = millis();

                        Serial.printf("[lora] reçu type=0x%02X len=%d\n",
                                      result.type, result.payloadLen);
                    } else {
                        Serial.println("[lora] CRC invalide !");
                    }
                }
            }

            _loraRxLen = 0;  // Réinitialiser le buffer
        }
    }

    // Overflow protection
    if (_loraRxLen >= LORA_RX_BUF_SIZE - 1) {
        _loraRxLen = 0;
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  loraLastAckTime() — Timestamp du dernier ACK reçu
// ─────────────────────────────────────────────────────────────────────────────
static uint32_t loraLastAckTime() {
    return _loraLastAck;
}

// ─────────────────────────────────────────────────────────────────────────────
//  loraIsActive() — Vérifier si le lien LoRa est actif
//  (dernier ACK reçu dans les LORA_LINK_TIMEOUT_MS)
// ─────────────────────────────────────────────────────────────────────────────
static bool loraIsActive() {
    if (!_loraInitialized) return false;
    return (millis() - _loraLastAck < LORA_LINK_TIMEOUT_MS);
}

#else  // LORA_ENABLED == false

// ── Stubs vides quand LoRa est désactivé ─────────────────────────────────────
struct LoraCommand {
    uint8_t type;
    uint8_t payload[32];
    uint8_t payloadLen;
    bool    valid;
};

static bool loraInit()                                                          { return false; }
static bool loraSendHeartbeat(const uint8_t*)                                   { return false; }
static bool loraSendTelemetry(const uint8_t*, float, float, float, float)       { return false; }
static bool loraSendEmergencyStop(const uint8_t*)                               { return false; }
static LoraCommand loraReceive()   { LoraCommand c = {0,{0},0,false}; return c; }
static uint32_t loraLastAckTime()                                               { return 0; }
static bool loraIsActive()                                                      { return false; }

#endif  // LORA_ENABLED
