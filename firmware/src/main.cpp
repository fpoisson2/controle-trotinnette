// Définir les variables globales du modem dans cette unité de compilation
#define MODEM_DEFINE_GLOBALS
#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include "esp_wpa2.h"
#include <Wire.h>
#include "audio.h"
#include "config.h"
#include "flipsky.h"
#include "sleep.h"
#include "music.h"
#include "display.h"
#include "connectivity.h"
// modem.h et lora.h sont inclus par connectivity.h et utilisés via leurs gardes
#include "lora.h"
#include <Update.h>

static FlipskyData escData;

// ── OTA via WebSocket proxy ──
static volatile bool otaMode     = false;
static size_t        otaTotal    = 0;
static size_t        otaReceived = 0;

// ── Debug logs via WebSocket (activable depuis la page web) ──
static volatile bool debugMode   = false;

// File d'attente circulaire de logs (Core0 écrit, Core1 envoie)
#define LOG_QUEUE_SIZE 16
#define LOG_MSG_SIZE   256
static char logQueue[LOG_QUEUE_SIZE][LOG_MSG_SIZE];
static volatile uint8_t logHead = 0;
static volatile uint8_t logTail = 0;

static void wsLog(const char *fmt, ...) {
    if (!debugMode) return;
    uint8_t next = (logHead + 1) % LOG_QUEUE_SIZE;
    if (next == logTail) return;  // queue pleine, drop

    char msg[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    // Échapper les caractères spéciaux pour JSON valide
    char *dst = logQueue[logHead];
    int pos = 0;
    pos += snprintf(dst + pos, LOG_MSG_SIZE - pos, "{\"type\":\"log\",\"msg\":\"");
    for (int i = 0; msg[i] && pos < LOG_MSG_SIZE - 4; i++) {
        char c = msg[i];
        if (c == '"' || c == '\\') { dst[pos++] = '\\'; dst[pos++] = c; }
        else if (c == '\n') { dst[pos++] = '\\'; dst[pos++] = 'n'; }
        else if (c == '\r') { /* skip */ }
        else dst[pos++] = c;
    }
    pos += snprintf(dst + pos, LOG_MSG_SIZE - pos, "\"}");
    dst[pos] = '\0';
    logHead = next;
}

// ─────────────────────────────────────────────────────────────────────────────
//  État global
// ─────────────────────────────────────────────────────────────────────────────
static volatile bool voiceActive = false;

// Flag global pour le bridge réseau WiFi/LTE (utilisé par WebSocketsNetworkClientSecure)
volatile bool _wsUseLte = false;

// ── Verrouillage à distance (libre-service) ──
// Quand locked=true, le throttle est forcé au neutre (ESC ne répond pas)
static volatile bool scooterLocked = false;  // déverrouillée au boot (debug)

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

// Pré-buffer audio complet (mode LTE chained)
static volatile bool     audioFullBuffer    = false;  // true = attendre audio_end avant lecture
static volatile uint32_t audioExpectedSize  = 0;
static volatile uint32_t audioReceivedSize  = 0;
static volatile uint32_t audioBeginMs       = 0;      // chrono réception

// Watchdog Core 1 : Core 1 met à jour ce timestamp, Core 0 vérifie
static volatile uint32_t core1Heartbeat = 0;
static uint32_t          core1WatchdogStart = 0;  // début de la surveillance
#define CORE1_WATCHDOG_MS    30000  // 30s sans heartbeat → reboot
#define CORE1_GRACE_MS       60000  // grâce 60s après le boot avant de surveiller

// Timestamp dernière commande IA (priorité sur manette physique)
static volatile uint32_t lastAiCmd = 0;

// Reconnexion WiFi non-bloquante (backoff exponentiel)
static uint32_t          wifiRetryAt        = 0;
static uint32_t          wifiRetryDelay     = 2000;   // commence à 2 s
static uint8_t           wifiRetryCount     = 0;
static volatile bool     wifiLost           = false;

// ── Boutons physiques (écran + verrouillage) ─────────────────────────────────
static bool     btnLockState      = false;
static bool     btnLockRaw        = false;
static uint32_t btnLockChanged    = 0;
static uint32_t btnLockPressStart = 0;

static bool     btnPageState      = false;
static bool     btnPageRaw        = false;
static uint32_t btnPageChanged    = 0;

static uint32_t lastPageChange    = 0;  // Pour retour auto à page 0

// ── Données d'affichage ──────────────────────────────────────────────────────
static DisplayData dispData;
static uint32_t    lastDisplayUpdate = 0;

// ── Connectivité watchdog (HORS ZONE) ────────────────────────────────────────
static bool     outOfRange         = false;
static uint32_t outOfRangeStart    = 0;
static uint32_t lastWifiActivity   = 0;
static bool     alertTonePlayed    = false;

// ── Sons différés (flags posés par les handlers, joués dans loop) ────────────
static volatile uint8_t pendingBeep = 0;  // 0=rien, 1=wifi ok, 2=wifi lost, 3=proxy ok, 4=proxy lost

// ── MAC address (cache) ──────────────────────────────────────────────────────
static uint8_t macAddr[6];

// ─────────────────────────────────────────────────────────────────────────────
//  Connexion WiFi
// ─────────────────────────────────────────────────────────────────────────────

// Lance WiFi.begin() sans bloquer (identifiants EAP reconfigurés si nécessaire)
static void wifiBegin() {
    WiFi.disconnect(true);
    delay(50);
    WiFi.mode(WIFI_STA);
#if WIFI_ENTERPRISE
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
}

// Connexion bloquante au boot — retourne true si WiFi connecté, false sinon
static bool connectWiFi() {
#if WIFI_ENTERPRISE
    Serial.println("[wifi] mode WPA2-Enterprise (EAP-PEAP)...");
#else
    Serial.println("[wifi] mode WPA2 personnel...");
#endif
    wifiBegin();

    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t > 10000) {
            Serial.println("[wifi] timeout");
            WiFi.disconnect(true);
            return false;
        }
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\n[wifi] connecté : %s\n", WiFi.localIP().toString().c_str());

    if (MDNS.begin("trotilou")) {
        MDNS.addService("ws", "tcp", WS_SERVER_PORT);
        wsLog("[mdns] trotilou.local");
    }

    // OTA
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
        wsLog("[ota] démarrage mise à jour...");
    });
    ArduinoOTA.onEnd([]() {
        wsLog("\n[ota] terminé — redémarrage");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        wsLog("[ota] %u%%\r", progress * 100 / total);
    });
    ArduinoOTA.onError([](ota_error_t error) {
        wsLog("[ota] erreur %u\n", error);
    });
    ArduinoOTA.begin();
    wsLog("[ota] prêt — hostname: %s\n", OTA_HOSTNAME);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  WebSocket client vers proxy — callback événements
//  Appelé depuis taskCapture (Core 1)
// ─────────────────────────────────────────────────────────────────────────────
static void onWsProxyEvent(WStype_t type, uint8_t *payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED: {
            wsProxyConnected = true;
            lastWifiActivity = millis();
            Serial.println("[ws-proxy] connecté à " PROXY_HOST);
            pendingBeep = 3;  // proxy ok (joué dans loop)
            wsLog("[ws-proxy] connecté");
            // Envoyer la version firmware + MAC au proxy
            {
                char hello[160];
                snprintf(hello, sizeof(hello),
                    "{\"type\":\"hello\",\"version\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
                    FW_VERSION, macAddr[0], macAddr[1], macAddr[2],
                    macAddr[3], macAddr[4], macAddr[5]);
                wsProxy.sendTXT(hello);
            }
            break;
        }

        case WStype_DISCONNECTED:
            wsProxyConnected = false;
            Serial.printf("[ws-proxy] déconnecté (uptime %lu s) — reconnexion auto\n",
                          millis() / 1000);
            pendingBeep = 4;  // proxy lost (joué dans loop)
            break;

        case WStype_TEXT: {
            lastWifiActivity = millis();
            JsonDocument doc;
            if (deserializeJson(doc, payload, length)) break;
            const char *evtype = doc["type"] | "";

            if (strcmp(evtype, "audio_begin") == 0) {
                // Proxy signale le début d'une réponse audio complète
                dacHead = dacTail = 0;  // Reset ring buffer
                dacPreBuffering = true;
                audioReceivedSize = 0;
                audioExpectedSize = doc["size"] | 0;
                audioBeginMs = millis();
                // Buffer complet seulement si ça tient dans le ring buffer
                if (audioExpectedSize < DAC_RING_SIZE - 1024) {
                    audioFullBuffer = true;
                    Serial.printf("[audio] attente %u bytes avant lecture\n",
                        (unsigned)audioExpectedSize);
                } else {
                    audioFullBuffer = false;
                    Serial.printf("[audio] streaming %u bytes (trop gros pour buffer %u)\n",
                        (unsigned)audioExpectedSize, DAC_RING_SIZE);
                }
            } else if (strcmp(evtype, "audio_end") == 0) {
                // Tout l'audio est arrivé → lancer la lecture
                dacPreBuffering = false;
                audioFullBuffer = false;
                uint32_t buffered = (dacHead - dacTail + DAC_RING_SIZE) & DAC_RING_MASK;
                Serial.printf("[timing] download %u bytes = %lu ms | lecture %u samples (%ums)\n",
                    (unsigned)audioReceivedSize, (unsigned long)(millis() - audioBeginMs),
                    buffered, buffered / 8);
            } else if (strcmp(evtype, "transcript") == 0) {
                // stt visible sur la page web, pas besoin de log debug
            } else if (strcmp(evtype, "ai_response") == 0) {
                // ia response visible sur la page web, pas besoin de log debug
            } else if (strcmp(evtype, "debug") == 0) {
                debugMode = doc["enabled"] | false;
                wsLog("[debug] mode %s", debugMode ? "activé" : "désactivé");
            } else if (strcmp(evtype, "ota_begin") == 0) {
                size_t fwSize = doc["size"] | 0;
                if (fwSize == 0) break;
                wsLog("[ota] début — %u bytes\n", (unsigned)fwSize);
                otaTotal    = fwSize;
                otaReceived = 0;
                // Stopper I2S pendant l'OTA
                audioStopI2S();
                if (!Update.begin(fwSize)) {
                    wsLog("[ota] Update.begin() échoué");
                    char msg[128];
                    snprintf(msg, sizeof(msg),
                        "{\"type\":\"ota_result\",\"success\":false,\"error\":\"Update.begin échoué\"}");
                    wsProxy.sendTXT(msg);
                    // Reprendre I2S (reste en pause, PTT le démarrera)

                } else {
                    // Vérification d'intégrité MD5 si fourni par le proxy
                    const char* md5 = doc["md5"] | (const char*)nullptr;
                    if (md5 && strlen(md5) == 32) {
                        Update.setMD5(md5);
                        wsLog("[ota] vérification MD5 activée : %s\n", md5);
                    }
                    otaMode = true;
                }
            } else if (strcmp(evtype, "ota_end") == 0) {
                if (otaMode) {
                    if (Update.end(true)) {
                        wsProxy.sendTXT("{\"type\":\"ota_result\",\"success\":true}");
                        wsProxy.loop();  // flush
                        delay(1000);
                        ESP.restart();
                    } else {
                        wsLog("[ota] Update.end() échoué : %s\n",
                            Update.errorString());
                        char msg[128];
                        snprintf(msg, sizeof(msg),
                            "{\"type\":\"ota_result\",\"success\":false,\"error\":\"%s\"}",
                            Update.errorString());
                        wsProxy.sendTXT(msg);
                    }
                    otaMode = false;
                }
            } else if (strcmp(evtype, "cmd") == 0) {
                lastAction    = doc["action"] | "";
                lastIntensity = doc["intensity"] | 0.5f;
                lastAiCmd     = millis();
                sleepResetActivity();  // commande IA = activité

                // Sécurité : ignorer les commandes moteur si verrouillée
                if (scooterLocked) {
                    wsLog("[cmd] BLOQUÉ (verrouillée) : %s\n", lastAction.c_str());
                    // Seul "arreter" est accepté même verrouillée (sécurité)
                    if (lastAction != "arreter") break;
                }

                wsLog("[cmd] %s @ %.2f\n", lastAction.c_str(), lastIntensity);
                audioBeep(600.0f, 30);  // bip court : commande reçue

                // Cmd 02H : throttle 0-1023, 512=neutre
                if (lastAction == "avancer") {
                    currentThrottle = (uint16_t)(FTESC_THROTTLE_NEUTRAL +
                        lastIntensity * (FTESC_THROTTLE_MAX - FTESC_THROTTLE_NEUTRAL));
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
                }
            } else if (strcmp(evtype, "music") == 0) {
                const char* action = doc["action"] | "";
                if (strcmp(action, "play") == 0) {
                    int track = doc["track"] | -1;
                    if (track >= 0) musicPlay((uint8_t)track);
                    else musicToggle();
                } else if (strcmp(action, "pause") == 0) {
                    musicPause();
                } else if (strcmp(action, "next") == 0) {
                    musicNext();
                } else if (strcmp(action, "prev") == 0) {
                    musicPrev();
                } else if (strcmp(action, "stop") == 0) {
                    musicStop();
                }
                sleepResetActivity();
                wsLog("[music] commande: %s", action);
            } else if (strcmp(evtype, "lock") == 0) {
                bool newLocked = doc["locked"] | true;
                scooterLocked = newLocked;
                wsLog("[lock] trottinette %s", newLocked ? "verrouillée" : "déverrouillée");
                if (newLocked) {
                    // Forcer arrêt immédiat au verrouillage
                    currentThrottle = FTESC_THROTTLE_NEUTRAL;
                    brakeLightOn    = false;
                    lastAction      = "arreter";
                }
                // Confirmer l'état au proxy
                char lockMsg[64];
                snprintf(lockMsg, sizeof(lockMsg),
                    "{\"type\":\"lock_ack\",\"locked\":%s}",
                    newLocked ? "true" : "false");
                wsProxy.sendTXT(lockMsg);
            }
            break;
        }
        case WStype_BIN:
            lastWifiActivity = millis();
            if (otaMode && length > 0) {
                // Mode OTA : chunks firmware
                size_t written = Update.write(payload, length);
                if (written != length) {
                    wsLog("[ota] erreur écriture : %u/%u\n",
                        (unsigned)written, (unsigned)length);
                }
                otaReceived += written;
                uint8_t pct = (uint8_t)(otaReceived * 100 / otaTotal);
                static uint8_t lastPct = 255;
                if (pct != lastPct && pct % 10 == 0) {
                    lastPct = pct;
                    wsLog("[ota] %u%%\n", pct);
                    char msg[64];
                    snprintf(msg, sizeof(msg),
                        "{\"type\":\"ota_progress\",\"percent\":%u}", pct);
                    wsProxy.sendTXT(msg);
                }
            } else if (length > 0) {
                // Audio PCM8 unsigned 8kHz brut du proxy
                static uint32_t lastAudioChunk = 0;
                static uint32_t audioChunkCount = 0;
                uint32_t now = millis();
                uint32_t gap = now - lastAudioChunk;

                // Détecter début de nouvelle réponse (gap > 1s, mode WiFi sans signal)
                if (gap > 1000 && !audioFullBuffer) {
                    audioChunkCount = 0;
                    dacPreBuffering = true;
                }
                audioChunkCount++;
                audioReceivedSize += length;

                // Écrire dans le ring buffer DAC
                dacEnqueueRaw(payload, length);

                uint32_t buffered = (dacHead - dacTail + DAC_RING_SIZE) & DAC_RING_MASK;

                // Mode full buffer (LTE) : ne pas toucher dacPreBuffering (audio_end le fera)
                // Mode streaming (WiFi) : pré-buffer 0.5s puis lecture
                if (!audioFullBuffer && dacPreBuffering && buffered >= 4000) {
                    dacPreBuffering = false;
                    Serial.printf("[audio] pré-buffer OK (%u samples = %ums)\n",
                        buffered, buffered / 8);
                }

                if (audioChunkCount <= 3 || gap > 500) {
                    Serial.printf("[audio-rx] #%u len=%u gap=%ums buf=%u\n",
                        audioChunkCount, (unsigned)length, gap, buffered);
                }
                lastAudioChunk = now;
            }
            break;

        default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Tâche capture audio (Core 1)
//  Gère aussi wsProxy (loop + send) — tout sur Core 1 pour éviter les races
// ─────────────────────────────────────────────────────────────────────────────
static uint8_t pcmBuf[MIC_CHUNK_SAMPLES * 2];
static uint8_t pcmAccum[MIC_CHUNK_SAMPLES * 2 * 2];  // 2 chunks = 64 ms
static size_t  pcmAccumLen = 0;

// Buffer audio LTE — alloué tôt dans setup() avant fragmentation heap
static uint8_t *lteAudioBuf = nullptr;
static size_t   lteAudioLen = 0;
static size_t   lteAudioCap = 0;

static void taskCapture(void *) {
    wsLog("[capture] tâche démarrée");
    while (true) {
        core1Heartbeat = millis();  // Watchdog : prouver que Core 1 est vivant
#if AUDIO_LOOPBACK
        // Mode test : bouton enfoncé = enregistre, relâché = rejoue
        static const size_t REC_MAX = 16000 * 2 * 2;
        static uint8_t *recBuf  = nullptr;
        static size_t   recLen  = 0;
        static bool     wasActive = false;

        if (!recBuf) {
            recBuf = (uint8_t *)malloc(REC_MAX);
            if (!recBuf) {
                wsLog("[loopback] malloc échoué, heap=%u\n",
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
            wsLog("[loopback] replay %u bytes\n", (unsigned)recLen);
            dacEnqueue(recBuf, recLen);
            recLen = 0;
        } else {
            uint8_t dummy[MIC_CHUNK_SAMPLES * 2];
            audioCaptureChunk(dummy);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        wasActive = nowActive;
#else
        // wsProxy.loop() gère connexion + réception
        wsProxy.loop();

        // ── Reconnexion manuelle LTE : reset SSL du modem si déconnecté ──────
        // La lib WebSocket reconnecte automatiquement, mais les sessions SSL
        // fantômes du modem empêchent la reconnexion propre.
#if LTE_ENABLED
        {
            static uint32_t lastSslReset = 0;
            static int lteReconnFails = 0;

            if (_wsUseLte && !wsProxyConnected) {
                if (millis() - lastSslReset > 10000) {
                    lastSslReset = millis();
                    lteReconnFails++;

                    // Toutes les 3 tentatives (30s), vérifier que GPRS est toujours actif
                    if (lteReconnFails % 3 == 0) {
                        if (!_modem.isGprsConnected()) {
                            Serial.println("[ws-lte] GPRS perdu — reconnexion modem...");
                            if (!modemConnect()) {
                                Serial.printf("[ws-lte] reconnexion GPRS échouée (tentative %d)\n",
                                              lteReconnFails);
                            } else {
                                Serial.println("[ws-lte] GPRS reconnecté");
                            }
                        }
                    }

                    // Après 10 min de tentatives (60 × 10s), reboot
                    if (lteReconnFails > 60) {
                        Serial.println("[ws-lte] reconnexion impossible après 10 min — reboot");
                        delay(100);
                        ESP.restart();
                    }

                    Serial.printf("[ws-lte] reset SSL modem (CCHSTOP+CCHSTART) tentative %d\n",
                                  lteReconnFails);
                    _modem.sendAT("+CCHSTOP");
                    _modem.waitResponse(3000);
                    delay(500);
                    // Profiter de la pause SSL pour lire signal + position Cell-ID
                    connUpdateLteRSSI();
                    // Cell-ID : position approx sans GPS
                    {
                        float lat = 0, lon = 0, acc = 0;
                        if (_modem.getGsmLocation(&lat, &lon, &acc)) {
                            if (lat != 0 || lon != 0) {
                                _gpsLat = lat; _gpsLon = lon; _gpsValid = true;
                            }
                        }
                    }
                    _modem.sendAT("+CCHSTART");
                    _modem.waitResponse(3000);
                }
            } else if (_wsUseLte && wsProxyConnected && lteReconnFails > 0) {
                // Connexion rétablie — remettre le compteur à zéro
                Serial.printf("[ws-lte] reconnecté après %d tentatives\n", lteReconnFails);
                lteReconnFails = 0;
            }
        }
        // RSSI LTE : mis à jour uniquement lors des reconnexions (AT+CSQ
        // pendant que le SSL est connecté corrompt le flux AT → déconnexion)
#endif

        // Envoyer télémétrie en attente (écrit par Core0, envoyé ici sur Core1)
        if (telemetryPending && wsProxyConnected) {
            bool ok = wsProxy.sendTXT(telemetryJsonBuf);
            telemetryPending = false;
            static uint32_t lastTelLog = 0;
            if (millis() - lastTelLog > 30000) {  // Log toutes les 30s
                lastTelLog = millis();
                Serial.printf("[tel] sendTXT=%d len=%u ws=%d\n",
                    (int)ok, (unsigned)strlen(telemetryJsonBuf), (int)wsProxyConnected);
            }
        }
        // Envoyer logs debug en attente (vider la queue)
        while (logTail != logHead && wsProxyConnected) {
            wsProxy.sendTXT(logQueue[logTail]);
            logTail = (logTail + 1) % LOG_QUEUE_SIZE;
        }
        // Heartbeat debug toutes les 3s
        {
            static uint32_t lastDbgHb = 0;
            if (debugMode && wsProxyConnected && millis() - lastDbgHb > 3000) {
                lastDbgHb = millis();
                char hb[128];
                snprintf(hb, sizeof(hb),
                    "{\"type\":\"log\",\"msg\":\"[hb] heap=%u queue=%d\"}",
                    (unsigned)esp_get_free_heap_size(),
                    (int)((logHead - logTail + LOG_QUEUE_SIZE) % LOG_QUEUE_SIZE));
                wsProxy.sendTXT(hb);
            }
        }

        if (otaMode) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        // ── I2S toujours actif, envoi audio seulement quand PTT enfoncé ──
        {
            // Log diagnostic PTT une seule fois par transition
            static bool lastVoiceLog = false;
            if (voiceActive != lastVoiceLog) {
                Serial.printf("[audio-c1] voiceActive=%d i2sOff=%d ws=%d lte=%d\n",
                    (int)voiceActive, (int)sleepIsI2SDisabled(),
                    (int)wsProxyConnected, (int)_wsUseLte);
                lastVoiceLog = voiceActive;
            }
        }
        if (!sleepIsI2SDisabled()) {
            size_t len = audioCaptureChunk(pcmBuf);  // toujours drainer le DMA

            if (_wsUseLte) {
                // ── Mode LTE chained : accumuler tout le PTT, envoyer en un bloc ──
                static bool     lteWasActive = false;

                if (voiceActive && wsProxyConnected && len > 0 && lteAudioBuf) {
                    if (lteAudioLen + len <= lteAudioCap) {
                        memcpy(lteAudioBuf + lteAudioLen, pcmBuf, len);
                        lteAudioLen += len;
                    }
                    // else: buffer plein, ignorer silencieusement
                } else if (lteWasActive && !voiceActive && lteAudioLen > 0 && wsProxyConnected) {
                    // PTT relâché : chrono bout-à-bout
                    uint32_t t0 = millis();
                    // Encoder en ADPCM puis envoyer en chunks
                    size_t nSamples = lteAudioLen / 2;  // PCM16 = 2 bytes/sample
                    size_t adpcmSize = 4 + (nSamples + 1) / 2;  // header + nibbles

                    // Encoder ADPCM dans la partie libre du buffer (après les données PCM)
                    uint8_t *adpcmBuf = lteAudioBuf + lteAudioLen;
                    if (lteAudioLen + adpcmSize <= lteAudioCap) {
                        adpcmResetEncoder();
                        size_t encoded = adpcmEncodeBlock((int16_t*)lteAudioBuf, nSamples, adpcmBuf);

                        char sig[96];
                        snprintf(sig, sizeof(sig),
                            "{\"type\":\"voice_blob\",\"size\":%u,\"format\":\"adpcm\",\"samples\":%u}",
                            (unsigned)encoded, (unsigned)nSamples);
                        wsProxy.sendTXT(sig);

                        Serial.printf("[lte-audio] ADPCM %u → %u bytes (%.1fx)\n",
                            (unsigned)lteAudioLen, (unsigned)encoded,
                            (float)lteAudioLen / encoded);

                        // Remplacer le pointeur et la taille pour l'envoi
                        lteAudioLen = encoded;
                        // adpcmBuf contient les données à envoyer
                        // On copie en début de buffer pour simplifier l'envoi
                        memmove(lteAudioBuf, adpcmBuf, encoded);
                    } else {
                        // Pas assez de place pour ADPCM → envoyer PCM brut
                        char sig[64];
                        snprintf(sig, sizeof(sig), "{\"type\":\"voice_blob\",\"size\":%u}",
                            (unsigned)lteAudioLen);
                        wsProxy.sendTXT(sig);
                    }

                    // Découper en chunks de 1KB avec 50ms de pause (ADPCM = peu de chunks)
                    const size_t CHUNK_SZ = 1024;
                    const uint32_t CHUNK_DELAY_MS = 50;
                    size_t sent = 0;
                    int chunks = 0;
                    while (sent < lteAudioLen && wsProxyConnected) {
                        size_t toSend = lteAudioLen - sent;
                        if (toSend > CHUNK_SZ) toSend = CHUNK_SZ;
                        wsProxy.sendBIN(lteAudioBuf + sent, toSend);
                        sent += toSend;
                        chunks++;
                        vTaskDelay(pdMS_TO_TICKS(CHUNK_DELAY_MS));
                    }
                    uint32_t t1 = millis();
                    Serial.printf("[timing] upload %u bytes en %d chunks = %lu ms\n",
                        (unsigned)sent, chunks, (unsigned long)(t1 - t0));
                    lteAudioLen = 0;
                    audioBeep(600.0f, 30);  // bip : en cours de traitement
                }
                lteWasActive = voiceActive;
            } else {
                // ── Mode WiFi : streaming temps réel via Realtime API ──
                if (voiceActive && wsProxyConnected && len > 0) {
                    if (pcmAccumLen + len <= sizeof(pcmAccum)) {
                        memcpy(pcmAccum + pcmAccumLen, pcmBuf, len);
                        pcmAccumLen += len;
                    }
                    if (pcmAccumLen >= sizeof(pcmAccum)) {
                        wsProxy.sendBIN((uint8_t*)pcmAccum, pcmAccumLen);
                        static uint32_t lastAudioLog = 0;
                        if (millis() - lastAudioLog > 2000) {
                            Serial.printf("[audio] envoi bin %u bytes ws=%d\n",
                                (unsigned)pcmAccumLen, wsProxyConnected);
                            lastAudioLog = millis();
                        }
                        pcmAccumLen = 0;
                    }
                } else if (!voiceActive && pcmAccumLen > 0) {
                    wsProxy.sendBIN((uint8_t*)pcmAccum, pcmAccumLen);
                    pcmAccumLen = 0;
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));  // Économiser le CPU en mode veille
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
            wsLog("[ws-srv] proxy connecté (#%d)\n", num);
            break;
        case WStype_DISCONNECTED:
            if (wsServerClientNum == num) wsServerClientNum = 255;
            wsLog("[ws-srv] proxy déconnecté");
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
    doc["lat"] = _gpsValid ? _gpsLat : 0.0f;
    doc["lon"] = _gpsValid ? _gpsLon : 0.0f;
    doc["gps_fix"] = _gpsValid ? "ok" : "none";
#endif
    // Ajouter type pour que le proxy sache que c'est de la télémétrie
    doc["type"] = "telemetry";
    doc["locked"] = scooterLocked;
    doc["rssi"] = connGetRSSI();
    doc["conn"] = connGetTypeName();

    char buf[320];
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
//  Remplir les données d'affichage depuis la télémétrie ESC + état connexion
// ─────────────────────────────────────────────────────────────────────────────
static void populateDisplayData() {
#if FAKE_TELEMETRY
    float t = millis() / 1000.0f;
    dispData.speed    = 15.0f + 10.0f * sinf(t * 0.3f);
    dispData.voltage  = 42.0f + 5.0f  * sinf(t * 0.1f);
    dispData.current  = 8.0f  + 6.0f  * fabsf(sinf(t * 0.4f));
    dispData.tempFet  = 45.0f + 10.0f * sinf(t * 0.05f);
    dispData.tempMotor = 40.0f;
    dispData.rpm      = dispData.speed * ESC_POLE_PAIRS * 1000.0f / 60.0f;
#else
    float rpm = fabsf(escData.rpm);
    dispData.speed     = rpm / (float)ESC_POLE_PAIRS * 60.0f * 0.216f * 3.14159f / 1000.0f;
    dispData.voltage   = escData.voltage;
    dispData.current   = escData.motorCurrent;
    dispData.tempFet   = escData.tempFet;
    dispData.tempMotor = escData.tempMotor;
    dispData.rpm       = escData.rpm;
#endif

    // Pourcentage batterie (linéaire entre min et max)
    dispData.battPercent = constrain(
        (dispData.voltage - BATT_VOLTAGE_MIN) / (BATT_VOLTAGE_MAX - BATT_VOLTAGE_MIN) * 100.0f,
        0.0f, 100.0f);

    // État connexion
    dispData.wifiConnected = (WiFi.status() == WL_CONNECTED);
    dispData.wsConnected   = wsProxyConnected;
    dispData.micActive     = voiceActive;
    dispData.locked        = scooterLocked;

    // Type de connexion et signal
    dispData.connType = (uint8_t)connGetType();
    dispData.rssi     = connGetRSSI();

    // État énergie
    dispData.powerState = (uint8_t)sleepGetPowerState();

    // LoRa
    dispData.loraConnected = loraIsActive();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Watchdog connectivité — détection HORS ZONE
//
//  Si tous les liens RF sont perdus (WiFi + LTE + LoRa) pendant plus de
//  WATCHDOG_GRACE_MS, forcer le neutre et afficher l'alerte HORS ZONE.
// ─────────────────────────────────────────────────────────────────────────────
static void checkConnectivityWatchdog() {
    bool wifiOk = (WiFi.status() == WL_CONNECTED) &&
                  (millis() - lastWifiActivity < WIFI_LINK_TIMEOUT_MS);
    bool wsOk   = wsProxyConnected;
    bool loraOk = loraIsActive();

    // LTE : NE PAS appeler modemIsConnected() depuis Core 0 !
    // TinyGSM n'est pas thread-safe : les commandes AT sont envoyées par Core 1
    // via wsProxy.loop(). Utiliser wsProxyConnected comme indicateur LTE.
    bool lteOk = _wsUseLte && wsProxyConnected;

    bool anyLink = wifiOk || wsOk || lteOk || loraOk;

    if (!anyLink) {
        if (!outOfRange) {
            // Premier constat de perte totale
            if (outOfRangeStart == 0) {
                outOfRangeStart = millis();
            }
            // Attendre la période de grâce
            if (millis() - outOfRangeStart >= WATCHDOG_GRACE_MS) {
                outOfRange = true;
                Serial.println("[watchdog] HORS ZONE — tous les liens RF perdus !");

                // Forcer le neutre par sécurité
                currentThrottle = FTESC_THROTTLE_NEUTRAL;
                brakeLightOn    = false;

                // Activer l'alerte sur l'écran
                displayShowOutOfRange(true);

                // Jouer la tonalité d'alerte (une seule fois)
                if (!alertTonePlayed) {
                    sleepPlayAlertTone();
                    alertTonePlayed = true;
                }
            }
        }
    } else {
        // Au moins un lien est actif
        if (outOfRange) {
            Serial.println("[watchdog] lien RF retrouvé — sortie HORS ZONE");
            outOfRange = false;
            displayShowOutOfRange(false);
            alertTonePlayed = false;
        }
        outOfRangeStart = 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  setup / loop
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    delay(500);

    // ── Allocation buffer audio LTE tôt (heap non fragmentée) ──────────────
#if LTE_ENABLED
    lteAudioCap = 16000 * 2 * 5;  // 5s de PCM16 16kHz = 160KB
    lteAudioBuf = (uint8_t*)ps_malloc(lteAudioCap);
    if (!lteAudioBuf) {
        lteAudioBuf = (uint8_t*)malloc(lteAudioCap);
    }
    if (!lteAudioBuf) {
        lteAudioCap = 16000 * 2 * 3;  // Fallback 3s = 96KB
        lteAudioBuf = (uint8_t*)malloc(lteAudioCap);
    }
    if (lteAudioBuf) {
        Serial.printf("[lte] buffer audio %u KB alloué (heap=%u)\n",
            (unsigned)(lteAudioCap / 1024), (unsigned)esp_get_free_heap_size());
    } else {
        Serial.printf("[lte] ERREUR buffer audio (heap=%u)\n",
            (unsigned)esp_get_free_heap_size());
    }
#endif

    // ── Détection réveil deep sleep et jingle de boot ──────────────────────
    bool wokeFromSleep = sleepIsWakeFromDeepSleep();
    if (wokeFromSleep) {
        // Jouer le jingle de réveil IMMÉDIATEMENT (avant WiFi, I2S, etc.)
        // L'utilisateur sait que la trottinette s'active
        sleepPlayBootJingle();
        Serial.begin(115200);
        Serial.printf("\n=== RÉVEIL DEEP SLEEP — cause: %s ===\n", sleepGetWakeReason());
    }

    // Initialiser le compteur d'inactivité
    sleepResetActivity();

    Serial.begin(115200);
    Serial.println("\n=== Trottinette Intelligente — ESP32 ===");
    wsLog("\n=== Trottinette Intelligente — ESP32 ===");

    // ── I2C pour l'écran OLED (SDA=21, SCL=22) ──────────────────────────────
    // Initialiser Wire au lieu de le désactiver (l'écran OLED en a besoin)
    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);

    // ── Écran OLED : initialisation et logo de démarrage ─────────────────────
#if OLED_ENABLED
    displayInit();
#endif

    // ── Cacher la MAC pour usage ultérieur (hello, LoRa, etc.) ───────────────
    WiFi.macAddress(macAddr);

    pinMode(VOICE_BTN_PIN, INPUT_PULLUP);

    // Pull-down sur le throttle : GPIO32 supporte les résistances internes
    // Évite que la pin flotte si le capteur est déconnecté
    pinMode(THROTTLE_ADC_PIN, INPUT_PULLDOWN);

    // Bouton PTT (actif LOW)
    pinMode(GEAR_R_PIN, INPUT_PULLUP);

    // ── Boutons physiques : verrouillage et changement de page ───────────────
    pinMode(BTN_LOCK_PIN, INPUT_PULLUP);
    pinMode(BTN_PAGE_PIN, INPUT_PULLUP);

    // Event WiFi : déconnexion détectée immédiatement, reconnexion déclenchée au prochain loop
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t) {
        if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            wifiLost    = true;
            wifiRetryAt = 0;  // retry immédiat
            pendingBeep = 2;  // wifi lost (joué dans loop)
            wsLog("[wifi] déconnecté (event)");
        } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
            wifiLost       = false;
            wifiRetryDelay = 2000;
            wifiRetryCount = 0;
            pendingBeep = 1;  // wifi ok (joué dans loop)
            lastWifiActivity = millis();
            wsLog("[wifi] reconnecté : %s\n", WiFi.localIP().toString().c_str());
            // Ré-enregistrer mDNS après reconnexion
            MDNS.end();
            if (MDNS.begin("trottinette")) {
                MDNS.addService("ws", "tcp", WS_SERVER_PORT);
                wsLog("[mdns] trotilou.local (ré-enregistré)");
            }
        }
    });

    bool wifiOk = connectWiFi();
    if (wifiOk) {
        lastWifiActivity = millis();
        connSetType(CONN_WIFI);
    }
#if LTE_ENABLED
    if (!wifiOk) {
        Serial.println("[lte] WiFi échoué — tentative LTE...");
        if (modemInit()) {
            Serial.println("[lte] modem initialisé");
            if (modemConnect()) {
                Serial.println("[lte] connecté via LTE — désactivation WiFi");
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
                connSetType(CONN_LTE);
                _wsUseLte = true;
            } else {
                Serial.println("[lte] connexion LTE échouée — reboot");
                ESP.restart();
            }
        } else {
            Serial.println("[lte] init modem échouée — reboot");
            ESP.restart();
        }
    }
#else
    if (!wifiOk) {
        Serial.println("[wifi] pas de fallback LTE — reboot");
        ESP.restart();
    }
#endif

    Serial2.begin(ESC_BAUD, SERIAL_8N1, ESC_RX_PIN, ESC_TX_PIN);
    wsLog("[esc] Flipsky UART initialisé");

    esp_err_t e = audioInitI2S();
    wsLog("[i2s] %s\n", e == ESP_OK ? "OK" : "ERREUR");

    audioInitDAC();
    wsLog("[dac] timer OK");

    // ── Initialisation LoRa (si activé) ──────────────────────────────────────
#if LORA_ENABLED
    if (loraInit()) {
        wsLog("[lora] module LoRa initialisé");
    } else {
        wsLog("[lora] ERREUR initialisation LoRa");
    }
#endif

    // ── Lecteur de musique ──────────────────────────────────────────────────
#if MUSIC_ENABLED
    musicInit();
#endif

    // WebSocket serveur (télémétrie)
    wsServer.begin();
    wsServer.onEvent(onWsServerEvent);
    wsLog("[ws-srv] port %d\n", WS_SERVER_PORT);

    // WebSocket client unique vers proxy (audio + réponses IA)
    wsProxy.beginSSL(PROXY_HOST, PROXY_PORT, "/ws-esp32");
    wsProxy.onEvent(onWsProxyEvent);
    wsProxy.setReconnectInterval(5000);  // 5s entre les tentatives de reconnexion
    // Pas de heartbeat WebSocket en LTE : TinyGSM ne lit pas les pongs assez vite.
    // La détection de connexion morte se fait via le silence timeout (120s)
    // dans WebSocketsNetworkClientSecure::available()
    if (!_wsUseLte) {
        wsProxy.enableHeartbeat(15000, 5000, 2);  // heartbeat WiFi seulement
    }
    wsLog("[ws-proxy] connexion vers wss://%s/ws-esp32\n", PROXY_HOST);

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

    // Attendre 2 s que l'ADC et la tension de référence se stabilisent
    // (I2S, DAC, WiFi radio tous actifs → référence analogique stable)
    wsLog("[cal] attente stabilisation ADC (2 s)...");
    delay(2000);

    wsLog("[cal] calibration throttle au repos (médiane 200 samples)...");
    {
        int samples[200];
        for (int i = 0; i < 200; i++) { samples[i] = analogRead(THROTTLE_ADC_PIN); delay(10); }
        // tri insertion pour médiane
        for (int i = 1; i < 200; i++) {
            int k = samples[i], j = i - 1;
            while (j >= 0 && samples[j] > k) { samples[j+1] = samples[j]; j--; }
            samples[j+1] = k;
        }
        throttleAdcMin = samples[100]; // médiane
    }
    // brakeAdcMin : valeur fixe config.h (GPIO39 stable, pas de circuit parasite)
    wsLog("[cal] thr_min=%d brk_min=%d (fixe)\n", throttleAdcMin, brakeAdcMin);

    wsLog("[setup] prêt — appuyer sur le bouton pour activer le micro");
}

void loop() {
    ArduinoOTA.handle();
    if (!_wsUseLte) wsServer.loop();  // pas de serveur local en mode LTE

    // ── Sons différés (posés par handlers WiFi/proxy, joués ici dans loop) ────
    {
        uint8_t beep = pendingBeep;
        if (beep) {
            pendingBeep = 0;
            switch (beep) {
                case 1: audioBeepWifiOk();    break;
                case 2: audioBeepWifiLost();  break;
                case 3: audioBeepProxyOk();   break;
                case 4: audioBeepProxyLost(); break;
            }
        }
    }

    // ── Gear-R = push-to-talk : tenir = micro actif, relâcher = inactif ────────
    // Anti-rebond 300 ms : filtre les pulses RTS du chip USB-série sur GPIO2
    {
        static bool     pttState   = false;
        static bool     pttRaw     = false;
        static uint32_t pttChanged = 0;
        bool now = (digitalRead(GEAR_R_PIN) == LOW);
        if (now != pttRaw) { pttRaw = now; pttChanged = millis(); }
        if (millis() - pttChanged >= 300) {
            if (pttRaw != pttState) {
                pttState = pttRaw;
                if (pttState) audioBeepPttOn();   // bip aigu : micro actif
                else          audioBeepPttOff();  // bip grave : micro coupé
            }
        }
        voiceActive = pttState;
    }

    // Vitesse unique : toujours FTESC_GEAR_HIGH (sélection vocale via IA)

    // ── Bouton LOCK : anti-rebond + appui long pour verrouiller/déverrouiller ──
    {
        bool now = (digitalRead(BTN_LOCK_PIN) == LOW);
        if (now != btnLockRaw) { btnLockRaw = now; btnLockChanged = millis(); }
        if (millis() - btnLockChanged >= BTN_DEBOUNCE_MS) {
            if (btnLockRaw && !btnLockState) {
                // Front montant (bouton enfoncé)
                btnLockState = true;
                btnLockPressStart = millis();
                sleepResetActivity();
            } else if (!btnLockRaw && btnLockState) {
                // Front descendant (bouton relâché)
                btnLockState = false;
                uint32_t pressDuration = millis() - btnLockPressStart;
                if (pressDuration >= BTN_LONG_PRESS_MS) {
                    // Appui long : basculer verrouillage
                    scooterLocked = !scooterLocked;
                    Serial.printf("[btn] verrouillage basculé → %s\n",
                                  scooterLocked ? "VERROUILLÉ" : "DÉVERROUILLÉ");
                    if (scooterLocked) {
                        currentThrottle = FTESC_THROTTLE_NEUTRAL;
                        brakeLightOn    = false;
                    }
                    // Confirmer au proxy si connecté
                    if (wsProxyConnected) {
                        char lockMsg[64];
                        snprintf(lockMsg, sizeof(lockMsg),
                            "{\"type\":\"lock_ack\",\"locked\":%s}",
                            scooterLocked ? "true" : "false");
                        // On ne peut pas envoyer directement (Core0 vs Core1)
                        // Utiliser le buffer de télémétrie ou la queue de logs
                        wsLog("[btn] lock_ack envoyé : locked=%s",
                              scooterLocked ? "true" : "false");
                    }
                }
            }
        }
    }

    // ── Bouton PAGE : anti-rebond + changement de page OLED ──────────────────
    {
        bool now = (digitalRead(BTN_PAGE_PIN) == LOW);
        if (now != btnPageRaw) { btnPageRaw = now; btnPageChanged = millis(); }
        if (millis() - btnPageChanged >= BTN_DEBOUNCE_MS) {
            if (btnPageRaw && !btnPageState) {
                // Front montant : changer de page
                btnPageState = true;
                displayNextPage();
                lastPageChange = millis();
                sleepResetActivity();
                // page suivante (pas de log)
            } else if (!btnPageRaw && btnPageState) {
                btnPageState = false;
            }
        }
    }

    // ── Retour automatique à la page 0 après DISPLAY_AUTO_RETURN_MS ─────────
    // Pas de retour auto si on est sur la page musique et que la musique joue
    bool onMusicPage = (_displayPage == MUSIC_PAGE);
    if (lastPageChange > 0 && millis() - lastPageChange >= DISPLAY_AUTO_RETURN_MS) {
        if (!(onMusicPage && musicIsActive())) {
            displaySetPage(0);
            lastPageChange = 0;
        }
    }

    // ── Lecteur de musique : tick non-bloquant ───────────────────────────────
#if MUSIC_ENABLED
    musicTick(voiceActive);
#endif

    // ── Scan WiFi périodique pour géolocalisation (toutes les 60s) ─────────
    // Le scan utilise le radio WiFi ESP32, pas le modem LTE — aucun conflit
#if LTE_ENABLED
    if (_wsUseLte) {
        static uint32_t lastWifiScan = 0;
        if (millis() - lastWifiScan > 300000) {  // 5 min = ~8600 req/mois (gratuit Google)
            lastWifiScan = millis();
            // Activer WiFi brièvement pour scanner
            WiFi.mode(WIFI_STA);
            delay(100);
            int n = WiFi.scanNetworks(false, false, false, 300);
            Serial.printf("[wifi-scan] %d réseaux trouvés\n", n);
            if (n > 0) {
                // Construire JSON compact — max 4 APs pour tenir dans LOG_MSG_SIZE (256)
                // Google Geolocation API n'a besoin que de 2-3 APs
                int count = min(n, 4);
                int pos = 0;
                char buf[LOG_MSG_SIZE];
                pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"type\":\"wifi_scan\",\"aps\":[");
                for (int i = 0; i < count && pos < (int)sizeof(buf) - 60; i++) {
                    if (i > 0) buf[pos++] = ',';
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "{\"mac\":\"%s\",\"rssi\":%d,\"ch\":%d}",
                        WiFi.BSSIDstr(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
                }
                pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
                Serial.printf("[wifi-scan] JSON (%d octets): %s\n", pos, buf);
                // Envoyer via la queue de logs (Core 1 envoie via wsProxy)
                uint8_t next = (logHead + 1) % LOG_QUEUE_SIZE;
                if (next != logTail) {
                    strncpy(logQueue[logHead], buf, LOG_MSG_SIZE - 1);
                    logQueue[logHead][LOG_MSG_SIZE - 1] = '\0';
                    logHead = next;
                }
            }
            WiFi.scanDelete();
            WiFi.mode(WIFI_OFF);
        }
    }
#endif

    // Reconnexion WiFi : backoff exponentiel 2s → 4s → 8s → … → 30s max, reboot après 20 échecs
    if (wifiLost && !_wsUseLte && millis() >= wifiRetryAt) {
        wifiRetryCount++;
        wsLog("[wifi] tentative reconnexion #%d (prochain essai dans %lu ms)\n",
                      wifiRetryCount, (unsigned long)wifiRetryDelay);
        if (wifiRetryCount >= 20) {
            wsLog("[wifi] trop d'échecs — reboot");
            ESP.restart();
        }
        wifiBegin();
        wifiRetryAt = millis() + wifiRetryDelay;
        wifiRetryDelay = min(wifiRetryDelay * 2, (uint32_t)30000);
    }

    // ── Cmd 02H : contrôle ESC à 20 Hz (50 ms) ──────────────────────────────
    static uint32_t lastControl = 0;
    if (millis() - lastControl >= 50) {
        lastControl = millis();
#if !FAKE_TELEMETRY
        // Attendre 3s après le boot pour laisser l'ESC finir son initialisation
        const bool escReady = (millis() >= 3000);
        if (escReady) {
        // Priorité IA pendant AI_CMD_PRIORITY_MS, ensuite manette physique
        // Si verrouillée : ignorer manette ET commandes IA
        bool aiActive = (millis() - lastAiCmd < AI_CMD_PRIORITY_MS);
        if (scooterLocked) {
            currentThrottle = FTESC_THROTTLE_NEUTRAL;
            brakeLightOn    = false;
        } else if (!aiActive) {
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
            //   accélération → +150/cycle (~170ms neutre→max)
            //   décélération relâche → -80/cycle (doux, évite coupure brusque)
            //   frein → immédiat
            const uint16_t RAMP_UP   = (fabsf(escData.rpm) < 100) ? 511 : 150;
            const uint16_t RAMP_DOWN = 80;
            if (targetThrottle > currentThrottle + RAMP_UP)
                currentThrottle += RAMP_UP;
            else if (!brakeOn && targetThrottle < currentThrottle && currentThrottle > targetThrottle + RAMP_DOWN)
                currentThrottle -= RAMP_DOWN;
            else
                currentThrottle = targetThrottle;
        }

        // Réduction progressive du frein moteur quand la vitesse diminue :
        // entre 100 RPM et 5 RPM → frein réduit linéairement vers zéro
        if (brakeLightOn) {
            float rpm = fabsf(escData.rpm);
            if (rpm < 5.0f) {
                currentThrottle = FTESC_THROTTLE_NEUTRAL;
                brakeLightOn    = false;
            } else if (rpm < 100.0f) {
                float factor = (rpm - 5.0f) / 95.0f;  // 0 à 5 RPM, 1 à 100 RPM
                uint16_t maxBrake = (uint16_t)(FTESC_THROTTLE_NEUTRAL * factor);
                uint16_t minAllowed = FTESC_THROTTLE_NEUTRAL - maxBrake;
                if (currentThrottle < minAllowed) currentThrottle = minAllowed;
            }
        }

        // Verrouillage : forcer neutre si locked (aucune commande moteur)
        {
            uint16_t escThrottle = currentThrottle;
            bool escBrake = brakeLightOn;
            if (scooterLocked) {
                escThrottle = FTESC_THROTTLE_NEUTRAL;
                escBrake    = false;
            }
            ftesc_control(Serial2, escThrottle, currentGear, escBrake);
        }
        if (ftesc_poll(Serial2, escData)) {
            // RPM → km/h : roue 8.5" (≈0.216m diam) avec réduction ESC_POLE_PAIRS
            float kmh = fabsf(escData.rpm) / ESC_POLE_PAIRS
                        * 60.0f * 0.216f * 3.14159f / 1000.0f;
            wsLog("[esc] thr=%u %.1fkm/h V=%.1fV A=%.2fA RPM=%.0f T=%.1f°C",
                currentThrottle, kmh, escData.voltage, escData.motorCurrent,
                escData.rpm, escData.tempFet);
        }
        }  // end if (escReady)
#endif
        // Utiliser l'intervalle de télémétrie dynamique selon l'état de puissance
        static uint32_t lastTelSend = 0;
        uint32_t telInterval = sleepGetTelemetryInterval();
        if (millis() - lastTelSend >= telInterval) {
            lastTelSend = millis();
            sendTelemetry();
        }
    }

    // ── Rafraîchissement de l'écran OLED ─────────────────────────────────────
#if OLED_ENABLED
    if (millis() - lastDisplayUpdate >= OLED_REFRESH_MS) {
        lastDisplayUpdate = millis();
        populateDisplayData();
        displayUpdate(&dispData);
    }
#endif

    // ── Watchdog connectivité (détection HORS ZONE) ──────────────────────────
    {
        static uint32_t lastWatchdog = 0;
        if (millis() - lastWatchdog >= 1000) {
            lastWatchdog = millis();
            checkConnectivityWatchdog();
        }
    }

    // ── Sécurité HORS ZONE : forcer neutre en continu ───────────────────────
    if (outOfRange) {
        currentThrottle = FTESC_THROTTLE_NEUTRAL;
        brakeLightOn    = false;
    }

    // ── LoRa : envoi périodique et réception ─────────────────────────────────
#if LORA_ENABLED
    {
        static uint32_t lastLoraSend = 0;
        if (millis() - lastLoraSend >= LORA_SEND_INTERVAL_MS) {
            lastLoraSend = millis();
            loraSendHeartbeat(macAddr);
        }

        // Vérifier les commandes LoRa entrantes
        LoraCommand cmd = loraReceive();
        if (cmd.valid) {
            switch (cmd.type) {
                case LORA_MSG_COMMAND:
                    wsLog("[lora] commande reçue");
                    sleepResetActivity();
                    break;
                case LORA_MSG_EMERGENCY:
                    Serial.println("[lora] ARRÊT D'URGENCE REÇU !");
                    currentThrottle = FTESC_THROTTLE_NEUTRAL;
                    brakeLightOn    = false;
                    scooterLocked   = true;
                    break;
                case LORA_MSG_LOCK: {
                    bool lockCmd = (cmd.payloadLen > 0) ? (cmd.payload[0] != 0) : true;
                    scooterLocked = lockCmd;
                    Serial.printf("[lora] verrouillage → %s\n",
                                  lockCmd ? "VERROUILLÉ" : "DÉVERROUILLÉ");
                    if (lockCmd) {
                        currentThrottle = FTESC_THROTTLE_NEUTRAL;
                        brakeLightOn    = false;
                    }
                    break;
                }
                case LORA_MSG_ACK:
                    // ACK reçu, le timestamp est déjà mis à jour par loraReceive()
                    break;
                default:
                    break;
            }
        }
    }
#endif

    // ── Heartbeat Core 0 (diagnostic) + Watchdog Core 1 ─────────────────────
#if LTE_ENABLED
    if (_wsUseLte) {
        static uint32_t lastC0Hb = 0;
        if (millis() - lastC0Hb >= 15000) {
            lastC0Hb = millis();
            uint32_t hb = core1Heartbeat;
            uint32_t delta = hb > 0 ? millis() - hb : 0;
            Serial.printf("[hb] C0 ok, C1 delta=%lu ms, ws=%d, heap=%u\n",
                          (unsigned long)delta, (int)wsProxyConnected,
                          (unsigned)esp_get_free_heap_size());
        }
        if (millis() > CORE1_GRACE_MS) {
            uint32_t hb = core1Heartbeat;
            if (hb > 0) {
                uint32_t delta = millis() - hb;
                if (delta > CORE1_WATCHDOG_MS) {
                    Serial.printf("[watchdog] Core 1 bloqué %lu ms — reboot!\n",
                                  (unsigned long)delta);
                    delay(100);
                    ESP.restart();
                }
            }
        }
    }
#endif

    // ── Moniteur d'inactivité → machine à états de puissance ─────────────────
    // Vérifier toutes les secondes si le système est inactif
    {
        static uint32_t lastSleepCheck = 0;
        if (millis() - lastSleepCheck >= 1000) {
            lastSleepCheck = millis();

            // Lire ADC bruts pour le test d'inactivité
            int thrRaw = analogRead(THROTTLE_ADC_PIN);
            int brkRaw = analogRead(BRAKE_ADC_PIN);
            bool pttNow = (digitalRead(GEAR_R_PIN) == LOW);
            bool aiNow  = (millis() - lastAiCmd < AI_CMD_PRIORITY_MS);

            sleepCheckInactivity(thrRaw, brkRaw, pttNow, aiNow,
                                 wsProxyConnected, throttleAdcMin, brakeAdcMin);
        }
    }
}
