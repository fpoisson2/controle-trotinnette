#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  music.h  —  Lecteur de mélodies RTTTL via DAC GPIO 25
//
//  Joue des mélodies stockées en flash au format RTTTL (Ring Tone Text
//  Transfer Language). Lecture non-bloquante : musicTick() avance la note
//  courante quand sa durée est écoulée.
//
//  Coexistence avec l'audio IA :
//    - Si le ring buffer DAC contient des données (voix IA), la musique
//      se met en pause automatiquement et reprend quand le buffer est vide.
//    - Si PTT est actif, la musique se met aussi en pause.
//
//  Contrôles :
//    - Boutons physiques (quand page musique affichée)
//    - Commande WebSocket {"type":"music","action":"play|pause|next|prev|stop"}
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <driver/dac.h>
#include "config.h"

#if MUSIC_ENABLED

// ── Déclarations externes (depuis audio.h) ──────────────────────────────────
// Le ring buffer DAC — musique y écrit des samples, l'ISR les joue à 24kHz
extern volatile uint8_t  dacRing[];
extern volatile uint32_t dacHead;
extern volatile uint32_t dacTail;
extern volatile bool dacMusicMode;
#define DAC_RING_SIZE  32768
#define DAC_RING_MASK  (DAC_RING_SIZE - 1)

// Phase continue de l'oscillateur musique
static float _musicPhase = 0.0f;

// ── Structure d'une note parsée ─────────────────────────────────────────────
struct MusicNote {
    uint16_t freq;      // Fréquence en Hz (0 = silence/pause)
    uint16_t duration;  // Durée en ms
};

// ── Playlist RTTTL ──────────────────────────────────────────────────────────
// Stockées en PROGMEM (flash) pour économiser la RAM

static const char PROGMEM _rtttl_mario[] =
    "Mario:d=4,o=5,b=100:"
    "16e6,16e6,32p,8e6,16c6,8e6,8g6,8p,8g,8p,"
    "8c6,16p,8g,16p,8e,16p,8a,8b,16a#,8a,16g.,16e6,16g6,8a6,16f6,8g6,8e6,16c6,16d6,8b";

static const char PROGMEM _rtttl_tetris[] =
    "Tetris:d=4,o=5,b=160:"
    "e6,8b,8c6,8d6,16c6,16b,a,8a,8c6,e6,8d6,8c6,b,8b,8c6,d6,e6,c6,a,2a,"
    "8p,d6,8f6,a6,8g6,8f6,e6,8e6,8c6,e6,8d6,8c6,b,8b,8c6,d6,e6,c6,a,a";

static const char PROGMEM _rtttl_starwars[] =
    "StarWars:d=4,o=5,b=45:"
    "32p,32f,32f,32f,8b.,8f6.,32e6,32d6,32c6,8b.6,16f6,32e6,32d6,32c6,8b.6,16f6,"
    "32e6,32d6,32e6,8c6";

static const char PROGMEM _rtttl_furelise[] =
    "FurElise:d=8,o=5,b=140:"
    "e6,d#6,e6,d#6,e6,b,d6,c6,4a,p,c,e,a,4b,p,e,g#,b,4c6,p,e,e6,d#6,e6,d#6,e6,b,d6,c6,"
    "4a,p,c,e,a,4b,p,e,c6,b,4a";

static const char PROGMEM _rtttl_nokia[] =
    "Nokia:d=4,o=5,b=225:"
    "8e6,8d6,f#,g#,8c#6,8b,d,e,8b,8a,c#,e,2a";

static const char PROGMEM _rtttl_jingle[] =
    "Jingle:d=4,o=5,b=240:"
    "8e,8e,e,8e,8e,e,8e,8g,8c,8d,2e,8f,8f,8f,8f,8f,8e,8e,16e,16e,8e,8d,8d,8e,d,g";

// ── Tableau des pistes ──────────────────────────────────────────────────────
struct MusicTrack {
    const char* rtttl;   // Chaîne RTTTL en PROGMEM
    const char* name;    // Nom court pour l'affichage OLED
};

static const MusicTrack _tracks[] = {
    { _rtttl_mario,    "Super Mario"  },
    { _rtttl_tetris,   "Tetris"       },
    { _rtttl_starwars, "Star Wars"    },
    { _rtttl_furelise, "Fur Elise"    },
    { _rtttl_nokia,    "Nokia Tune"   },
    { _rtttl_jingle,   "Jingle Bells" },
};
static const uint8_t _trackCount = sizeof(_tracks) / sizeof(_tracks[0]);

// ── État du lecteur ─────────────────────────────────────────────────────────
static MusicNote _notes[MUSIC_MAX_NOTES];
static uint16_t  _noteCount    = 0;
static uint16_t  _noteIndex    = 0;
static bool      _musicPlaying = false;
static bool      _musicPaused  = false;
static uint8_t   _currentTrack = 0;
static uint32_t  _noteStartMs  = 0;
static uint32_t  _trackStartMs = 0;
static uint32_t  _totalDurMs   = 0;   // Durée totale de la piste (calculée au parse)

// ── Fréquences des notes (octave 0) ─────────────────────────────────────────
// Index : 0=C, 1=C#, 2=D, 3=D#, 4=E, 5=F, 6=F#, 7=G, 8=G#, 9=A, 10=A#, 11=B
// Fréquences de l'octave 4 (C4=262 Hz ... B4=494 Hz)
static const uint16_t _baseFreq[] = {
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494
};

// ── Parseur RTTTL ───────────────────────────────────────────────────────────
// Format RTTTL : "name:d=4,o=5,b=120:note,note,note,..."
// Note : [durée][lettre][#][.][octave]
//   durée : 1,2,4,8,16,32 (optionnel, défaut = d)
//   lettre : c,d,e,f,g,a,b,p (p = pause)
//   # : dièse (optionnel)
//   . : pointée (durée * 1.5)
//   octave : 4-7 (optionnel, défaut = o)

static uint16_t _parseRTTTL(const char* rtttlPgm) {
    // Sur ESP32, flash et RAM partagent le même espace d'adressage
    // Pas besoin de copier — on lit directement la chaîne PROGMEM
    // (On copie quand même car on modifie p en avançant le pointeur)
    static char buf[512];  // static pour éviter 512 octets sur la pile
    strncpy(buf, rtttlPgm, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* p = buf;
    _noteCount = 0;
    _totalDurMs = 0;

    // Sauter le nom (jusqu'au premier ':')
    while (*p && *p != ':') p++;
    if (!*p) return 0;
    p++; // passer le ':'

    // Paramètres par défaut
    uint8_t defDuration = MUSIC_DEFAULT_DURATION;
    uint8_t defOctave   = MUSIC_DEFAULT_OCTAVE;
    uint16_t bpm        = MUSIC_DEFAULT_BPM;

    // Parser les paramètres (d=4,o=5,b=120)
    while (*p && *p != ':') {
        while (*p == ' ' || *p == ',') p++;
        if (*p == 'd' && *(p+1) == '=') {
            p += 2;
            defDuration = (uint8_t)atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        } else if (*p == 'o' && *(p+1) == '=') {
            p += 2;
            defOctave = (uint8_t)atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        } else if (*p == 'b' && *(p+1) == '=') {
            p += 2;
            bpm = (uint16_t)atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        } else {
            p++;
        }
    }
    if (!*p) return 0;
    p++; // passer le deuxième ':'

    // Durée d'une ronde (whole note) en ms
    uint32_t wholeNote = (60000UL * 4) / bpm;

    // Parser les notes
    while (*p && _noteCount < MUSIC_MAX_NOTES) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        // Durée (optionnelle)
        uint8_t dur = 0;
        if (*p >= '0' && *p <= '9') {
            dur = (uint8_t)atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        }
        if (dur == 0) dur = defDuration;

        // Note (c, d, e, f, g, a, b, p)
        int8_t noteIdx = -1; // -1 = pause
        switch (tolower(*p)) {
            case 'c': noteIdx = 0;  break;
            case 'd': noteIdx = 2;  break;
            case 'e': noteIdx = 4;  break;
            case 'f': noteIdx = 5;  break;
            case 'g': noteIdx = 7;  break;
            case 'a': noteIdx = 9;  break;
            case 'b': noteIdx = 11; break;
            case 'p': noteIdx = -1; break;
            default: p++; continue;
        }
        p++;

        // Dièse (#)
        if (*p == '#') {
            if (noteIdx >= 0) noteIdx++;
            p++;
        }

        // Pointée (.)
        bool dotted = false;
        if (*p == '.') {
            dotted = true;
            p++;
        }

        // Octave (optionnelle)
        uint8_t oct = defOctave;
        if (*p >= '0' && *p <= '9') {
            oct = (uint8_t)atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        }

        // Deuxième pointée possible après l'octave
        if (*p == '.') {
            dotted = true;
            p++;
        }

        // Calculer la fréquence
        uint16_t freq = 0;
        if (noteIdx >= 0) {
            // Fréquence base (octave 5) ajustée
            freq = _baseFreq[noteIdx % 12];
            int8_t octShift = (int8_t)oct - 4;  // base = octave 4
            if (octShift > 0) {
                for (int8_t i = 0; i < octShift; i++) freq *= 2;
            } else if (octShift < 0) {
                for (int8_t i = 0; i < -octShift; i++) freq /= 2;
            }
        }

        // Calculer la durée en ms
        uint16_t durMs = (uint16_t)(wholeNote / dur);
        if (dotted) durMs = durMs + durMs / 2;

        _notes[_noteCount].freq     = freq;
        _notes[_noteCount].duration = durMs;
        _totalDurMs += durMs;
        _noteCount++;
    }

    return _noteCount;
}

// ── API publique ────────────────────────────────────────────────────────────

static void musicInit() {
    Serial.printf("[music] %d piste(s) disponible(s)\n", _trackCount);
}

static void musicPlay(uint8_t trackIndex) {
    if (trackIndex >= _trackCount) trackIndex = 0;
    _currentTrack = trackIndex;

    uint16_t count = _parseRTTTL(_tracks[trackIndex].rtttl);
    if (count == 0) {
        Serial.printf("[music] erreur parsing RTTTL : %s\n", _tracks[trackIndex].name);
        return;
    }

    _noteIndex    = 0;
    _musicPlaying = true;
    _musicPaused  = false;
    _noteStartMs  = millis();
    _trackStartMs = millis();
    dacMusicMode  = true;  // Empêcher l'ISR DAC d'écrire du silence

    Serial.printf("[music] lecture : %s (%d notes, %lu ms)\n",
                  _tracks[trackIndex].name, count, (unsigned long)_totalDurMs);
}

static void musicPause() {
    if (_musicPlaying) {
        _musicPaused = true;
        dacMusicMode = false;
    }
}

static void musicResume() {
    if (_musicPlaying && _musicPaused) {
        _musicPaused = false;
        _noteStartMs = millis(); // Reprendre la note courante depuis le début
    }
}

static void musicStop() {
    _musicPlaying = false;
    _musicPaused  = false;
    _noteIndex    = 0;
    dacMusicMode  = false;
    _musicPhase   = 0.0f;
}

static void musicNext() {
    uint8_t next = (_currentTrack + 1) % _trackCount;
    musicPlay(next);
}

static void musicPrev() {
    // Si on est à plus de 3s dans la piste, revenir au début
    if (millis() - _trackStartMs > 3000) {
        musicPlay(_currentTrack);
    } else {
        uint8_t prev = (_currentTrack == 0) ? _trackCount - 1 : _currentTrack - 1;
        musicPlay(prev);
    }
}

static void musicToggle() {
    if (!_musicPlaying) {
        musicPlay(_currentTrack);
    } else if (_musicPaused) {
        musicResume();
    } else {
        musicPause();
    }
}

// ── musicTick() — appeler dans loop() ───────────────────────────────────────
// Génère des samples dans le ring buffer DAC (joués par l'ISR timer à 24kHz).
// Plus de sautillement : l'ISR assure un débit constant, indépendant de loop().
//
// Coexistence IA : si le ring buffer DAC contient des données voix IA,
// la musique se met en pause automatiquement.

// Pré-remplir le ring buffer avec des samples de la note courante
// Génère ~100ms de samples par appel (2400 samples à 24kHz)
#define MUSIC_SAMPLES_PER_TICK  2400

static void musicTick(bool pttActive) {
    if (!_musicPlaying) return;

    // ── Pause automatique si PTT actif ──────────────────────────────────
    // (la coexistence voix IA / musique est gérée par le ring buffer :
    //  la voix IA écrase les données musique quand elle arrive)
    if (pttActive) {
        if (!_musicPaused) {
            _musicPaused = true;
        }
        return;
    }
    if (_musicPaused && !pttActive) {
        _musicPaused = false;
        _noteStartMs = millis();
        _musicPhase  = 0.0f;
    }

    if (_musicPaused) return;

    // ── Garder ~500ms d'avance dans le buffer pour absorber les pauses de loop() ──
    uint32_t buffered = (dacHead - dacTail + DAC_RING_SIZE) & DAC_RING_MASK;
    if (buffered > SPK_SAMPLE_RATE / 2) return;  // déjà ~500ms en buffer

    // ── Vérifier si la note courante est terminée ────────────────────────
    if (_noteIndex >= _noteCount) {
        _noteIndex    = 0;
        _noteStartMs  = millis();
        _trackStartMs = millis();
        _musicPhase   = 0.0f;
        return;
    }

    const MusicNote& note = _notes[_noteIndex];
    uint32_t elapsed = millis() - _noteStartMs;

    if (elapsed >= note.duration) {
        _noteIndex++;
        _noteStartMs = millis();
        _musicPhase  = 0.0f;
        // Silence inter-notes : quelques samples à 128
        for (int i = 0; i < SPK_SAMPLE_RATE / 200 && // ~5ms
             ((dacHead + 1) & DAC_RING_MASK) != dacTail; i++) {
            dacRing[dacHead] = 128;
            dacHead = (dacHead + 1) & DAC_RING_MASK;
        }
        return;
    }

    // ── Générer des samples dans le ring buffer ──────────────────────────
    const float phaseInc = (note.freq > 0) ? (float)note.freq / SPK_SAMPLE_RATE : 0.0f;

    for (int i = 0; i < MUSIC_SAMPLES_PER_TICK; i++) {
        // Vérifier qu'il y a de la place
        uint32_t next = (dacHead + 1) & DAC_RING_MASK;
        if (next == dacTail) break;  // buffer plein

        uint8_t sample;
        if (note.freq == 0) {
            sample = 128;  // silence
        } else {
            // Onde carrée douce (légèrement arrondie pour réduire les harmoniques)
            float phase01 = _musicPhase - (int)_musicPhase;  // 0.0 à 1.0
            sample = (phase01 < 0.5f) ? 160 : 96;
            _musicPhase += phaseInc;
        }

        dacRing[dacHead] = sample;
        dacHead = next;
    }
}

// ── Getters d'état ──────────────────────────────────────────────────────────

static bool musicIsPlaying() {
    return _musicPlaying && !_musicPaused;
}

static bool musicIsPaused() {
    return _musicPlaying && _musicPaused;
}

static bool musicIsActive() {
    return _musicPlaying;
}

static const char* musicGetTrackName() {
    return _tracks[_currentTrack].name;
}

static uint8_t musicGetTrackIndex() {
    return _currentTrack;
}

static uint8_t musicGetTrackCount() {
    return _trackCount;
}

// Progression 0.0 — 1.0
static float musicGetProgress() {
    if (!_musicPlaying || _totalDurMs == 0) return 0.0f;

    // Calculer la durée écoulée en sommant les notes déjà jouées
    uint32_t playedMs = 0;
    for (uint16_t i = 0; i < _noteIndex && i < _noteCount; i++) {
        playedMs += _notes[i].duration;
    }
    // Ajouter la portion de la note en cours
    if (_noteIndex < _noteCount) {
        uint32_t noteElapsed = millis() - _noteStartMs;
        playedMs += min(noteElapsed, (uint32_t)_notes[_noteIndex].duration);
    }

    return constrain((float)playedMs / (float)_totalDurMs, 0.0f, 1.0f);
}

// Temps écoulé en secondes
static uint32_t musicGetElapsedSec() {
    if (!_musicPlaying) return 0;
    return (millis() - _trackStartMs) / 1000;
}

// Durée totale en secondes
static uint32_t musicGetTotalSec() {
    return _totalDurMs / 1000;
}

#else  // MUSIC_ENABLED == false

// ── Stubs vides ─────────────────────────────────────────────────────────────
static void        musicInit()                {}
static void        musicPlay(uint8_t)         {}
static void        musicPause()               {}
static void        musicResume()              {}
static void        musicStop()                {}
static void        musicNext()                {}
static void        musicPrev()                {}
static void        musicToggle()              {}
static void        musicTick(bool)            {}
static bool        musicIsPlaying()           { return false; }
static bool        musicIsPaused()            { return false; }
static bool        musicIsActive()            { return false; }
static const char* musicGetTrackName()        { return ""; }
static uint8_t     musicGetTrackIndex()       { return 0; }
static uint8_t     musicGetTrackCount()       { return 0; }
static float       musicGetProgress()         { return 0.0f; }
static uint32_t    musicGetElapsedSec()       { return 0; }
static uint32_t    musicGetTotalSec()         { return 0; }

#endif  // MUSIC_ENABLED
