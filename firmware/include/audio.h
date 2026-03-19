#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// audio.h  —  Capture I2S MEMS + lecture DAC avec timer hardware
//
// Architecture :
//   [I2S DMA] → int32_t raw → PCM16 → HTTP POST /audio → proxy
//   SSE /stream → base64 → PCM16 → dacRing → [Timer ISR] → DAC GPIO25 → haut-parleur
//
// Deux tâches FreeRTOS :
//   taskCapture (Core 1) : lit I2S, encode PCM16, POST HTTP
//   taskSSE     (Core 0) : lit SSE, décode audio, remplit dacRing, parse JSON
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <driver/i2s.h>
#include <driver/dac.h>
#include <soc/dac_channel.h>
#include <soc/sens_reg.h>
#include "mbedtls/base64.h"
#include "config.h"

// ── Ring buffer pour la lecture DAC (alimenté par taskCapture, lu par timer ISR)
#define DAC_RING_SIZE  32768   // ~1.36 s à 24 kHz (power of 2 pour masque rapide)
#define DAC_RING_MASK  (DAC_RING_SIZE - 1)

volatile uint8_t  dacRing[DAC_RING_SIZE];
volatile uint32_t dacHead = 0;   // index d'écriture
volatile uint32_t dacTail = 0;   // index de lecture (ISR)

// ── Insérer des samples PCM16 dans le ring buffer ────────────────────────────
// Non-bloquant : si le buffer est plein, attend brièvement par blocs, puis drop
inline void dacEnqueue(const uint8_t *pcm16, size_t byteCount) {
    const size_t nSamples = byteCount / 2;

    // Attendre de la place en bloc (max 50 ms) au lieu de par-sample
    uint32_t avail = (dacTail - dacHead - 1 + DAC_RING_SIZE) & DAC_RING_MASK;
    if (avail < nSamples) {
        uint32_t waited = 0;
        while (avail < nSamples && waited < 50) {
            vTaskDelay(pdMS_TO_TICKS(1));
            waited++;
            avail = (dacTail - dacHead - 1 + DAC_RING_SIZE) & DAC_RING_MASK;
        }
    }

    // Écrire autant de samples que possible
    size_t toWrite = nSamples;
    avail = (dacTail - dacHead - 1 + DAC_RING_SIZE) & DAC_RING_MASK;
    if (toWrite > avail) toWrite = avail;

    for (size_t i = 0; i < toWrite; i++) {
        int16_t s = (int16_t)(pcm16[i * 2] | (pcm16[i * 2 + 1] << 8));
        int32_t amp = (int32_t)(s * VOLUME_GAIN);
        if (amp >  32767) amp =  32767;
        if (amp < -32768) amp = -32768;
        dacRing[dacHead] = (uint8_t)(((int16_t)amp >> 8) + 128);
        dacHead = (dacHead + 1) & DAC_RING_MASK;
    }
}

// ── ISR timer — lecture ring buffer → DAC (écriture registre directe) ────────
static hw_timer_t *dacTimer = nullptr;

// Écriture directe au registre DAC — plus rapide que dac_output_voltage() dans l'ISR
static inline void IRAM_ATTR dacWriteFast(uint8_t val) {
    // DAC channel 1 = GPIO25
    SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_CW_EN1_M, 0, SENS_DAC_CW_EN1_S);
    SET_PERI_REG_BITS(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_DAC, val, RTC_IO_PDAC1_DAC_S);
}

// Flag global : si true, l'ISR ne génère pas de silence quand le ring buffer
// est vide — la musique écrit directement au DAC depuis loop().
volatile bool dacMusicMode = false;

void IRAM_ATTR onDacTimer() {
    if (dacHead != dacTail) {
        dacWriteFast(dacRing[dacTail]);
        dacTail = (dacTail + 1) & DAC_RING_MASK;
    } else if (!dacMusicMode) {
        dacWriteFast(128);  // silence DC (seulement si pas en mode musique)
    }
    // En mode musique avec buffer vide, on laisse le DAC tel quel
    // (musicTick() gère la sortie directement)
}

// ── Initialisation I2S (microphone MEMS) ─────────────────────────────────────
inline esp_err_t audioInitI2S() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = MIC_SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_RIGHT,  // SPH0645 sur ESP32 : RIGHT même avec SEL=GND
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 4,
        .dma_buf_len          = MIC_CHUNK_SAMPLES,
        .use_apll             = false,
        .tx_desc_auto_clear   = false,
        .fixed_mclk           = 0
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = MIC_BCLK_PIN,
        .ws_io_num    = MIC_LRCLK_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = MIC_DATA_PIN
    };
    esp_err_t e = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    if (e != ESP_OK) return e;
    return i2s_set_pin(I2S_NUM_0, &pins);
}

// ── Initialisation DAC + timer hardware ──────────────────────────────────────
inline void audioInitDAC() {
    dac_output_enable(DAC_CHANNEL_1);
    dac_output_voltage(DAC_CHANNEL_1, 128);

    // Timer 1, prescaler 80 → tick = 1 µs
    // En mode loopback, on utilise MIC_SAMPLE_RATE pour éviter la distorsion de pitch
#if AUDIO_LOOPBACK
    const uint32_t dacRate = MIC_SAMPLE_RATE;
#else
    const uint32_t dacRate = SPK_SAMPLE_RATE;
#endif
    dacTimer = timerBegin(1, 80, true);
    timerAttachInterrupt(dacTimer, &onDacTimer, true);
    timerAlarmWrite(dacTimer, 1000000UL / dacRate, true);
    timerAlarmEnable(dacTimer);
}

// ── Pause / reprise I2S (PTT-gated) ────────────────────────────────────────
// Plus fiable que install/uninstall répétés
inline void audioStopI2S()  { i2s_stop(I2S_NUM_0); }
inline void audioStartI2S() { i2s_start(I2S_NUM_0); }

// ── Capturer un chunk I2S, filtrer et encoder en PCM16 little-endian ────────
// Filtre passe-haut IIR 1er ordre (~50 Hz cutoff à 16 kHz) :
//   y[n] = α × (y[n-1] + x[n] - x[n-1])   avec α ≈ 0.98
// Supprime le DC offset du MEMS et le bruit basse fréquence.
//
// pcmOut : buffer de sortie (MIC_CHUNK_SAMPLES * 2 bytes minimum)
// Retourne le nombre de bytes écrits
inline size_t audioCaptureChunk(uint8_t *pcmOut) {
    static int32_t raw[MIC_CHUNK_SAMPLES];
    // État du filtre passe-haut (persistant entre les appels)
    static float hpPrev = 0.0f;   // y[n-1]
    static float hpPrevX = 0.0f;  // x[n-1]
    const float alpha = 0.98f;

    size_t bytesRead = 0;
    i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytesRead, pdMS_TO_TICKS(100));
    const size_t n = bytesRead / sizeof(int32_t);
    for (size_t i = 0; i < n; i++) {
        // SPH0645 / ICS-43434 : données en bits [31:14], >> 16 → PCM16
        float x = (float)(raw[i] >> 16);

        // Filtre passe-haut IIR
        float y = alpha * (hpPrev + x - hpPrevX);
        hpPrevX = x;
        hpPrev  = y;

        // Gain logiciel
        int32_t amp = (int32_t)(y * MIC_GAIN);
        if (amp >  32767) amp =  32767;
        if (amp < -32768) amp = -32768;
        int16_t s = (int16_t)amp;
        pcmOut[i * 2]     = (uint8_t)(s & 0xFF);
        pcmOut[i * 2 + 1] = (uint8_t)((s >> 8) & 0xFF);
    }
    return n * 2;
}

// ── Bip court via DAC (non-bloquant via ring buffer) ────────────────────────
// Génère un bip synthétique directement dans le ring buffer DAC
// freq: fréquence Hz, durMs: durée ms
inline void audioBeep(float freq, uint32_t durMs) {
    const uint32_t nSamples = (SPK_SAMPLE_RATE * durMs) / 1000;
    const float step = freq * 2.0f * 3.14159f / (float)SPK_SAMPLE_RATE;
    for (uint32_t i = 0; i < nSamples; i++) {
        // Vérifier qu'il y a de la place dans le ring buffer
        uint32_t next = (dacHead + 1) & DAC_RING_MASK;
        if (next == dacTail) break;  // buffer plein, stop
        // Onde carrée douce (sinusoïde écrêtée) pour un son net
        float val = sinf(step * i);
        uint8_t dac = (uint8_t)(128 + (int8_t)(val > 0 ? 90 : -90));
        dacRing[dacHead] = dac;
        dacHead = next;
    }
}

// Bip PTT ON : note aiguë courte (880 Hz, 60 ms)
inline void audioBeepPttOn()  { audioBeep(880.0f, 60); }
// Bip PTT OFF : note grave courte (440 Hz, 60 ms)
inline void audioBeepPttOff() { audioBeep(440.0f, 60); }
// Bip WiFi connecté : double-bip grave montant (330→500 Hz) — distinct du PTT
inline void audioBeepWifiOk()   { audioBeep(330.0f, 40); audioBeep(500.0f, 40); }
// Bip WiFi déconnecté : double-bip grave descendant (500→220 Hz)
inline void audioBeepWifiLost() { audioBeep(500.0f, 40); audioBeep(220.0f, 80); }

// ── Décoder base64 et enqueue dans le ring buffer DAC ────────────────────────
// Buffer statique : les chunks base64 font max 4096 chars → ~3072 bytes PCM
static uint8_t b64DecodeBuf[4096];

inline void audioPushBase64(const char *b64, size_t b64Len) {
    size_t outLen = 0;
    int rc = mbedtls_base64_decode(b64DecodeBuf, sizeof(b64DecodeBuf), &outLen,
                                   (const unsigned char *)b64, b64Len);
    if (rc == 0 && outLen > 0) {
        dacEnqueue(b64DecodeBuf, outLen);
    }
}
