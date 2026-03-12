#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include "esp_wpa2.h"
#include "audio.h"
#include "config.h"

// ─────────────────────────────────────────────────────────────────────────────
//  État global
// ─────────────────────────────────────────────────────────────────────────────
static volatile bool voiceActive = false;
static volatile bool btnPrev     = HIGH;

// WebSocket serveur (télémétrie vers proxy)
static WebSocketsServer wsServer(WS_SERVER_PORT);
static uint8_t          wsServerClientNum = 255;

// WebSocket client unique vers proxy (audio envoi + réponses réception)
static WebSocketsClient wsProxy;
static volatile bool    wsProxyConnected = false;

static String lastAction    = "";
static float  lastIntensity = 0.0f;

// ─────────────────────────────────────────────────────────────────────────────
//  Connexion WiFi
// ─────────────────────────────────────────────────────────────────────────────
static void connectWiFi() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);

#if WIFI_ENTERPRISE
    Serial.println("[wifi] mode WPA2-Enterprise (EAP-PEAP)...");
    esp_wifi_sta_wpa2_ent_set_identity(
        (const uint8_t *)EAP_IDENTITY, strlen(EAP_IDENTITY));
    esp_wifi_sta_wpa2_ent_set_username(
        (const uint8_t *)EAP_USERNAME, strlen(EAP_USERNAME));
    esp_wifi_sta_wpa2_ent_set_password(
        (const uint8_t *)EAP_PASSWORD, strlen(EAP_PASSWORD));
    esp_wifi_sta_wpa2_ent_enable();
    WiFi.begin(WIFI_SSID);
#else
    Serial.println("[wifi] mode WPA2 personnel...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
#endif

    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t > 30000) {
            Serial.println("[wifi] timeout — reboot");
            ESP.restart();
        }
        delay(500);
        Serial.print('.');
    }
    Serial.printf("\n[wifi] connecté : %s\n", WiFi.localIP().toString().c_str());

    if (MDNS.begin("trottinette")) {
        MDNS.addService("ws", "tcp", WS_SERVER_PORT);
        Serial.println("[mdns] trottinette.local");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  WebSocket client vers proxy — callback événements
//  Appelé depuis taskCapture (Core 1)
// ─────────────────────────────────────────────────────────────────────────────
static void onWsProxyEvent(WStype_t type, uint8_t *payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            wsProxyConnected = true;
            Serial.println("[ws-proxy] connecté");
            break;

        case WStype_DISCONNECTED:
            wsProxyConnected = false;
            Serial.println("[ws-proxy] déconnecté — reconnexion auto");
            break;

        case WStype_TEXT: {
            JsonDocument doc;
            if (deserializeJson(doc, payload, length)) break;
            const char *evtype = doc["type"] | "";

            if (strcmp(evtype, "audio") == 0) {
                const char *b64 = doc["data"] | "";
                size_t b64Len = strlen(b64);
                if (b64Len > 0) {
                    Serial.printf("[audio] delta %u chars\n", (unsigned)b64Len);
                    audioPushBase64(b64, b64Len);
                }
            } else if (strcmp(evtype, "transcript") == 0) {
                Serial.printf("[stt] %s\n", doc["text"] | "");
            } else if (strcmp(evtype, "ai_response") == 0) {
                Serial.printf("[ia] %s", doc["text"] | "");
            } else if (strcmp(evtype, "cmd") == 0) {
                lastAction    = doc["action"] | "";
                lastIntensity = doc["intensity"] | 0.0f;
                Serial.printf("[cmd] %s @ %.2f\n", lastAction.c_str(), lastIntensity);
                // → piloter les moteurs
            }
            break;
        }
        default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Tâche capture audio (Core 1)
//  Gère aussi wsProxy (loop + send) — tout sur Core 1 pour éviter les races
// ─────────────────────────────────────────────────────────────────────────────
static uint8_t pcmBuf[MIC_CHUNK_SAMPLES * 2];
static uint8_t pcmAccum[MIC_CHUNK_SAMPLES * 2 * 4];  // 4 chunks = 128 ms
static size_t  pcmAccumLen = 0;

static void taskCapture(void *) {
    Serial.println("[capture] tâche démarrée");
    while (true) {
#if AUDIO_LOOPBACK
        // Mode test : bouton enfoncé = enregistre, relâché = rejoue
        static const size_t REC_MAX = 16000 * 2 * 2;
        static uint8_t *recBuf  = nullptr;
        static size_t   recLen  = 0;
        static bool     wasActive = false;

        if (!recBuf) {
            recBuf = (uint8_t *)malloc(REC_MAX);
            if (!recBuf) {
                Serial.printf("[loopback] malloc échoué, heap=%u\n",
                              (unsigned)esp_get_free_heap_size());
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }
        bool nowActive = voiceActive;
        if (nowActive) {
            size_t len = audioCaptureChunk(pcmBuf);
            if (len > 0 && recLen + len <= REC_MAX) {
                memcpy(recBuf + recLen, pcmBuf, len);
                recLen += len;
            }
        } else if (wasActive && recLen > 0) {
            Serial.printf("[loopback] replay %u bytes\n", (unsigned)recLen);
            dacEnqueue(recBuf, recLen);
            recLen = 0;
        } else {
            uint8_t dummy[MIC_CHUNK_SAMPLES * 2];
            audioCaptureChunk(dummy);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        wasActive = nowActive;
#else
        wsProxy.loop();  // maintenir connexion + recevoir réponses IA

        size_t len = audioCaptureChunk(pcmBuf);  // toujours lire l'I2S
        if (voiceActive && wsProxyConnected && len > 0) {
            // Accumuler 4 chunks (128 ms) avant d'envoyer une frame WS
            if (pcmAccumLen + len <= sizeof(pcmAccum)) {
                memcpy(pcmAccum + pcmAccumLen, pcmBuf, len);
                pcmAccumLen += len;
            }
            if (pcmAccumLen >= sizeof(pcmAccum)) {
                wsProxy.sendBIN(pcmAccum, pcmAccumLen);
                pcmAccumLen = 0;
            }
        } else if (!voiceActive && pcmAccumLen > 0) {
            // Envoyer le reste quand le bouton est relâché
            wsProxy.sendBIN(pcmAccum, pcmAccumLen);
            pcmAccumLen = 0;
        }
#endif
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  WebSocket serveur — télémétrie vers proxy
// ─────────────────────────────────────────────────────────────────────────────
static void onWsServerEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            wsServerClientNum = num;
            Serial.printf("[ws-srv] proxy connecté (#%d)\n", num);
            break;
        case WStype_DISCONNECTED:
            if (wsServerClientNum == num) wsServerClientNum = 255;
            Serial.println("[ws-srv] proxy déconnecté");
            break;
        default: break;
    }
}

static void sendTelemetry() {
    if (wsServerClientNum == 255) return;
    JsonDocument doc;
#if FAKE_TELEMETRY
    static float simLat   = 46.8492f;
    static float simLon   = -71.3434f;
    static float simAngle = 0.0f;
    simAngle += 0.02f;
    simLat   += cosf(simAngle) * 0.0001f;
    simLon   += sinf(simAngle) * 0.0001f;
    float t = millis() / 1000.0f;
    doc["speed"]   = 15.0f + 10.0f * sinf(t * 0.3f);
    doc["voltage"] = 42.0f + 5.0f  * sinf(t * 0.1f);
    doc["current"] = 8.0f  + 6.0f  * fabsf(sinf(t * 0.4f));
    doc["temp"]    = 45.0f + 10.0f * sinf(t * 0.05f);
    doc["lat"]     = simLat;
    doc["lon"]     = simLon;
#else
    doc["speed"] = 0.0f; doc["voltage"] = 0.0f;
    doc["current"] = 0.0f; doc["temp"] = 0.0f;
    doc["lat"] = 0.0f; doc["lon"] = 0.0f;
#endif
    char buf[256];
    serializeJson(doc, buf, sizeof(buf));
    wsServer.sendTXT(wsServerClientNum, buf);
}

// ─────────────────────────────────────────────────────────────────────────────
//  setup / loop
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Trottinette Intelligente — ESP32 ===");

    pinMode(VOICE_BTN_PIN, INPUT_PULLUP);
    connectWiFi();

    esp_err_t e = audioInitI2S();
    Serial.printf("[i2s] %s\n", e == ESP_OK ? "OK" : "ERREUR");

    audioInitDAC();
    Serial.println("[dac] timer OK");

    // WebSocket serveur (télémétrie)
    wsServer.begin();
    wsServer.onEvent(onWsServerEvent);
    Serial.printf("[ws-srv] port %d\n", WS_SERVER_PORT);

    // WebSocket client unique vers proxy (audio + réponses IA)
    wsProxy.beginSSL(PROXY_HOST, PROXY_PORT, "/ws-esp32");
    wsProxy.onEvent(onWsProxyEvent);
    wsProxy.setReconnectInterval(3000);
    wsProxy.enableHeartbeat(10000, 3000, 2);  // ping/10 s — évite coupure Cloudflare
    Serial.printf("[ws-proxy] connexion vers wss://%s/ws-esp32\n", PROXY_HOST);

    // Tâche capture + wsProxy sur Core 1
    xTaskCreatePinnedToCore(taskCapture, "taskCapture", 20480, NULL, 2, NULL, 1);

    // Bip de démarrage
    for (int note = 0; note < 2; note++) {
        float    freq      = (note == 0) ? 440.0f : 880.0f;
        uint32_t period_us = (uint32_t)(1000000.0f / freq);
        uint32_t end_ms    = millis() + 80;
        while (millis() < end_ms) {
            dac_output_voltage(DAC_CHANNEL_1, 200);
            delayMicroseconds(period_us / 2);
            dac_output_voltage(DAC_CHANNEL_1, 56);
            delayMicroseconds(period_us / 2);
        }
        dac_output_voltage(DAC_CHANNEL_1, 128);
        delay(30);
    }

    Serial.println("[setup] prêt — appuyer sur le bouton pour activer le micro");
}

void loop() {
    wsServer.loop();

    // Bouton toggle vocal (actif bas, anti-rebond 200 ms)
    static uint32_t btnLastMs = 0;
    bool btnNow = digitalRead(VOICE_BTN_PIN);
    if (btnNow == LOW && btnPrev == HIGH && millis() - btnLastMs > 200) {
        btnLastMs = millis();
        voiceActive = !voiceActive;
        Serial.printf("[btn] micro %s\n", voiceActive ? "ACTIF" : "INACTIF");
    }
    btnPrev = btnNow;

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[wifi] connexion perdue — reconnexion...");
        connectWiFi();
    }

    static uint32_t lastTelemetry = 0;
    if (millis() - lastTelemetry >= TELEMETRY_MS) {
        lastTelemetry = millis();
        sendTelemetry();
    }
}
