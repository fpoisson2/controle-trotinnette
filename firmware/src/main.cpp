#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include "esp_wpa2.h"
#include "audio.h"
#include "config.h"
#include "flipsky.h"

static FlipskyData escData;

// ─────────────────────────────────────────────────────────────────────────────
//  État global
// ─────────────────────────────────────────────────────────────────────────────
static volatile bool voiceActive = false;

// WebSocket serveur (télémétrie vers proxy)
static WebSocketsServer wsServer(WS_SERVER_PORT);
static uint8_t          wsServerClientNum = 255;

// WebSocket client unique vers proxy (audio envoi + réponses réception)
static WebSocketsClient wsProxy;
static volatile bool    wsProxyConnected = false;

static String   lastAction      = "";
static float    lastIntensity   = 0.0f;
static uint16_t currentThrottle = FTESC_THROTTLE_NEUTRAL;
static uint8_t  currentGear     = FTESC_GEAR_HIGH;
static bool     brakeLightOn    = false;

static int throttleAdcMin = THROTTLE_ADC_MIN;
static int brakeAdcMin    = BRAKE_ADC_MIN;

// Buffer télémétrie partagé Core0→Core1 (wsProxy uniquement sur Core1)
static volatile bool telemetryPending = false;
static char          telemetryJsonBuf[256];

// Timestamp dernière commande IA (priorité sur manette physique)
static volatile uint32_t lastAiCmd = 0;

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

    // OTA
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
        Serial.println("[ota] démarrage mise à jour...");
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\n[ota] terminé — redémarrage");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("[ota] %u%%\r", progress * 100 / total);
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("[ota] erreur %u\n", error);
    });
    ArduinoOTA.begin();
    Serial.printf("[ota] prêt — hostname: %s\n", OTA_HOSTNAME);
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
                lastIntensity = doc["intensity"] | 0.5f;
                lastAiCmd     = millis();
                Serial.printf("[cmd] %s @ %.2f\n", lastAction.c_str(), lastIntensity);

                // Cmd 02H : throttle 0-1023, 512=neutre
                if (lastAction == "avancer") {
                    currentThrottle = (uint16_t)(FTESC_THROTTLE_NEUTRAL +
                        lastIntensity * (FTESC_THROTTLE_MAX - FTESC_THROTTLE_NEUTRAL));
                    if (currentGear == FTESC_GEAR_REVERSE) currentGear = FTESC_GEAR_HIGH;
                    brakeLightOn    = false;
                } else if (lastAction == "freiner") {
                    currentThrottle = (uint16_t)(FTESC_THROTTLE_NEUTRAL -
                        lastIntensity * FTESC_THROTTLE_NEUTRAL);
                    brakeLightOn    = true;
                } else if (lastAction == "arreter") {
                    currentThrottle = FTESC_THROTTLE_NEUTRAL;
                    brakeLightOn    = false;
                } else if (lastAction == "vitesse_lente") {
                    currentGear = FTESC_GEAR_LOW;
                } else if (lastAction == "vitesse_moyenne") {
                    currentGear = FTESC_GEAR_MEDIUM;
                } else if (lastAction == "vitesse_haute") {
                    currentGear = FTESC_GEAR_HIGH;
                } else if (lastAction == "marche_arriere") {
                    currentGear     = FTESC_GEAR_REVERSE;
                    currentThrottle = FTESC_THROTTLE_NEUTRAL;
                    brakeLightOn    = false;
                }
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
        wsProxy.loop();  // maintenir connexion + recevoir réponses IA (Core1 seulement)

        // Envoyer télémétrie en attente (écrit par Core0, envoyé ici sur Core1)
        if (telemetryPending && wsProxyConnected) {
            wsProxy.sendTXT(telemetryJsonBuf);
            telemetryPending = false;
        }

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
    // Envoyer via wsProxy (/ws-esp32) — toujours disponible même à travers Cloudflare
    // Envoyer aussi via wsServer (port 8080) si un client local est connecté
    bool canSendProxy = wsProxyConnected;
    bool canSendServer = (wsServerClientNum != 255);
    if (!canSendProxy && !canSendServer) return;

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
    {
        float rpm   = fabsf(escData.rpm);
        float speed = rpm / (float)ESC_POLE_PAIRS * 60.0f / 1000.0f;
        doc["speed"]   = speed;
        doc["voltage"] = escData.voltage;
        doc["current"] = escData.motorCurrent;
        doc["temp"]    = escData.tempFet;
    }
    doc["lat"] = 0.0f; doc["lon"] = 0.0f;
#endif
    // Ajouter type pour que le proxy sache que c'est de la télémétrie
    doc["type"] = "telemetry";

    char buf[256];
    serializeJson(doc, buf, sizeof(buf));

    // wsProxy : copier dans le buffer partagé, Core1 (taskCapture) envoie
    if (canSendProxy) {
        memcpy(telemetryJsonBuf, buf, strlen(buf) + 1);
        telemetryPending = true;
    }
    // wsServer (port 8080) : envoi direct depuis Core0, c'est son propre serveur
    if (canSendServer) {
        wsServer.sendTXT(wsServerClientNum, buf);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  setup / loop
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Trottinette Intelligente — ESP32 ===");

    pinMode(VOICE_BTN_PIN, INPUT_PULLUP);

    // Pull-down sur le throttle : GPIO32 supporte les résistances internes
    // Évite que la pin flotte si le capteur est déconnecté
    pinMode(THROTTLE_ADC_PIN, INPUT_PULLDOWN);

    // Boutons manette gear (actif LOW)
    pinMode(GEAR_R_PIN, INPUT_PULLUP);
    pinMode(GEAR_L_PIN, INPUT_PULLUP);
    pinMode(GEAR_H_PIN, INPUT_PULLUP);

    connectWiFi();

    Serial2.begin(ESC_BAUD, SERIAL_8N1, ESC_RX_PIN, ESC_TX_PIN);
    Serial.println("[esc] Flipsky UART initialisé");

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

    Serial.println("[cal] calibration throttle au repos...");
    long tSum = 0;
    for (int i = 0; i < 100; i++) {
        tSum += analogRead(THROTTLE_ADC_PIN);
        delay(10);
    }
    throttleAdcMin = (int)(tSum / 100);
    // brakeAdcMin : valeur fixe config.h (GPIO39 stable, pas de circuit parasite)
    Serial.printf("[cal] thr_min=%d brk_min=%d (fixe)\n", throttleAdcMin, brakeAdcMin);

    Serial.println("[setup] prêt — appuyer sur le bouton pour activer le micro");
}

void loop() {
    ArduinoOTA.handle();
    wsServer.loop();


    // ── Gear-R = push-to-talk : tenir = micro actif, relâcher = inactif ────────
    voiceActive = (digitalRead(GEAR_R_PIN) == LOW);

    // ── Gear-L / H : sélection de vitesse (M supprimé — GPIO22 = PTT) ────────
    static uint8_t prevGear = FTESC_GEAR_HIGH;
    bool gH = digitalRead(GEAR_H_PIN) == LOW;
    bool gL = digitalRead(GEAR_L_PIN) == LOW;
    if (gH) currentGear = FTESC_GEAR_HIGH;
    else if (gL) currentGear = FTESC_GEAR_LOW;
    if (currentGear != prevGear) {
        Serial.printf("[gear] L=%d H=%d → gear=%d\n", gL, gH, currentGear);
        prevGear = currentGear;
    }

    // ── Log ADC toutes les 2s pour calibration ────────────────────────────────
    static uint32_t lastAdcLog = 0;
    if (millis() - lastAdcLog > 2000) {
        lastAdcLog = millis();
        Serial.printf("[adc] thr=%d brk=%d ptt=%d gL=%d gH=%d\n",
            analogRead(THROTTLE_ADC_PIN), analogRead(BRAKE_ADC_PIN),
            !digitalRead(GEAR_R_PIN), !digitalRead(GEAR_L_PIN),
            !digitalRead(GEAR_H_PIN));
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[wifi] connexion perdue — reconnexion...");
        connectWiFi();
    }

    // ── Cmd 02H : contrôle + télémétrie toutes les 50 ms (20 Hz) ────────────
    static uint32_t lastControl = 0;
    if (millis() - lastControl >= 50) {
        lastControl = millis();
#if !FAKE_TELEMETRY
        // Attendre 3s après le boot pour laisser l'ESC finir son initialisation
        const bool escReady = (millis() >= 3000);
        if (escReady) {
        // Priorité IA pendant AI_CMD_PRIORITY_MS, ensuite manette physique
        bool aiActive = (millis() - lastAiCmd < AI_CMD_PRIORITY_MS);
        if (!aiActive) {
            // Filtre médian 5 échantillons : immunisé contre les spikes ADC
            auto adcMedian5 = [](int pin) -> int {
                int s[5];
                for (int i = 0; i < 5; i++) s[i] = analogRead(pin);
                // tri insertion
                for (int i = 1; i < 5; i++) {
                    int k = s[i], j = i - 1;
                    while (j >= 0 && s[j] > k) { s[j+1] = s[j]; j--; }
                    s[j+1] = k;
                }
                return s[2]; // médiane
            };
            // IIR sur les lectures médianes (α=0.5 : rapide mais lisse les résidus)
            static float thrSmooth = -1;
            static float brkSmooth = -1;
            if (thrSmooth < 0) thrSmooth = throttleAdcMin;
            if (brkSmooth < 0) brkSmooth = brakeAdcMin;
            thrSmooth = 0.5f * adcMedian5(THROTTLE_ADC_PIN) + 0.5f * thrSmooth;
            brkSmooth = 0.5f * adcMedian5(BRAKE_ADC_PIN)    + 0.5f * brkSmooth;
            int throttleVal = (int)thrSmooth;
            int brakeVal    = (int)brkSmooth;

            // Hystérésis throttle : ON > MIN+100, OFF < MIN+50 → empêche vmvmvm
            static bool throttleOn = false;
            if (!throttleOn && throttleVal > throttleAdcMin + THROTTLE_DEADZONE)
                throttleOn = true;
            else if (throttleOn && throttleVal < throttleAdcMin + THROTTLE_DEADZONE / 2)
                throttleOn = false;

            // Hystérésis frein : ON > MIN+100, OFF < MIN+50
            static bool brakeOn = false;
            if (!brakeOn && brakeVal > brakeAdcMin + BRAKE_DEADZONE)
                brakeOn = true;
            else if (brakeOn && brakeVal < brakeAdcMin + BRAKE_DEADZONE / 2)
                brakeOn = false;

            uint16_t targetThrottle;
            if (brakeOn) {
                int mapped = map(brakeVal, brakeAdcMin, BRAKE_ADC_MAX,
                                 FTESC_THROTTLE_NEUTRAL, FTESC_THROTTLE_MIN);
                targetThrottle = (uint16_t)constrain(mapped,
                                 FTESC_THROTTLE_MIN, FTESC_THROTTLE_NEUTRAL);
                brakeLightOn = true;
            } else if (throttleOn) {
                float n = (float)(throttleVal - throttleAdcMin)
                        / (float)(THROTTLE_ADC_MAX - throttleAdcMin);
                n = constrain(n, 0.0f, 1.0f);
                targetThrottle = FTESC_THROTTLE_NEUTRAL +
                    (uint16_t)(n * (FTESC_THROTTLE_MAX - FTESC_THROTTLE_NEUTRAL));
                brakeLightOn = false;
            } else {
                targetThrottle = FTESC_THROTTLE_NEUTRAL;
                brakeLightOn   = false;
            }

            // Soft gear limit (firmware, indépendant du ESC)
            uint16_t gearMax;
            switch (currentGear) {
                case FTESC_GEAR_LOW:    gearMax = 680;  break;  // ~33% puissance
                case FTESC_GEAR_MEDIUM: gearMax = 840;  break;  // ~64% puissance
                default:                gearMax = FTESC_THROTTLE_MAX; break;
            }
            if (targetThrottle > gearMax) targetThrottle = gearMax;

            // Rate limiting :
            //   démarrage (RPM < 100) → immédiat (couple max pour vaincre la charge)
            //   accélération en mouvement → +150/cycle (~170ms neutre→max)
            //   relâche/frein → immédiat (neutre ou frein sans délai)
            const uint16_t RAMP_UP = (fabsf(escData.rpm) < 100) ? 511 : 150;
            if (targetThrottle > currentThrottle + RAMP_UP)
                currentThrottle += RAMP_UP;
            else
                currentThrottle = targetThrottle;
        }

        // Si on freine et moteur arrêté → retour au neutre
        if (brakeLightOn && fabsf(escData.rpm) < 5.0f) {
            currentThrottle = FTESC_THROTTLE_NEUTRAL;
            brakeLightOn    = false;
        }

        ftesc_control(Serial2, currentThrottle, currentGear, brakeLightOn);
        if (ftesc_poll(Serial2, escData)) {
            // RPM → km/h : roue 8.5" (≈0.216m diam) avec réduction ESC_POLE_PAIRS
            float kmh = fabsf(escData.rpm) / ESC_POLE_PAIRS
                        * 60.0f * 0.216f * 3.14159f / 1000.0f;
            Serial.printf("[esc] thr=%u %.1fkm/h V=%.1fV A=%.2fA RPM=%.0f T=%.1f°C\n",
                currentThrottle, kmh, escData.voltage, escData.motorCurrent,
                escData.rpm, escData.tempFet);
        }
        }  // end if (escReady)
#endif
        static uint32_t lastTelSend = 0;
        if (millis() - lastTelSend >= TELEMETRY_MS) {
            lastTelSend = millis();
            sendTelemetry();
        }
    }
}
