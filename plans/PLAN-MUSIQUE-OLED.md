# Plan — Mode musique sur écran OLED

## Concept

Une page supplémentaire sur l'écran OLED qui fait office de mini lecteur musical. L'ESP32 joue des mélodies via le DAC GPIO 25 (même sortie que la voix IA et les jingles) et affiche le titre, la progression et les contrôles sur l'écran.

## Source audio : mélodies en notation RTTTL

Les mélodies sont stockées en mémoire flash sous forme de chaînes RTTTL (Ring Tone Text Transfer Language) — le format utilisé par les sonneries Nokia. Chaque mélodie pèse ~100-300 octets.

Exemples inclus :
1. **Super Mario** — thème principal
2. **Tetris** — Korobeiniki
3. **Star Wars** — thème impérial
4. **Fur Elise** — Beethoven
5. **Nokia Tune** — classique
6. **Jingle Bells** — saisonnier

## Affichage écran (Page 5 — Musique)

```
┌──────────────────────────┐
│ ♪ MUSIQUE         03/06  │  titre page + index piste
│                          │
│   ♫ Super Mario ♫        │  nom de la mélodie
│                          │
│   ████████░░░░░░  0:12   │  barre de progression + temps
│                          │
│  [◄◄]  [ ▶ ]  [►►]      │  contrôles (ref visuelle)
│  LOCK   PTT    PAGE      │  mapping boutons
└──────────────────────────┘
```

Si en pause :
```
│  [◄◄]  [ ❚❚ ]  [►►]     │
```

## Contrôles physiques (en page Musique uniquement)

| Bouton | Action | Notes |
|--------|--------|-------|
| **BTN_PAGE (GPIO 0)** - appui court | Piste suivante | Cycle dans la playlist |
| **BTN_PAGE** - appui long (>1s) | Quitter mode musique → page 1 | Arrête la lecture |
| **BTN_LOCK (GPIO 15)** - appui court | Play / Pause | Toggle lecture |
| **PTT (GPIO 2)** - appui court | Piste précédente | Revient au début si < 3s |

## Contrôle via WebSocket (dashboard)

```json
{"type":"music", "action":"play|pause|next|prev|stop", "track": 0}
```

Le proxy relaie la commande, le dashboard affiche l'état.

## Architecture firmware

### Nouveau header : `music.h`

- Parseur RTTTL → séquence de notes (fréquence Hz + durée ms)
- Lecture non-bloquante via le DAC existant (timer hardware)
- File de notes avec index courant
- Fonctions :
  - `musicInit()` — charger les mélodies RTTTL
  - `musicPlay(trackIndex)` — démarrer une piste
  - `musicPause()` / `musicResume()`
  - `musicStop()` — arrêter
  - `musicNext()` / `musicPrev()`
  - `musicTick()` — appeler dans loop(), avance la note si durée écoulée
  - `musicIsPlaying()` — état
  - `musicGetTrackName()` — nom piste courante
  - `musicGetProgress()` — 0.0-1.0
  - `musicGetElapsedSec()` — temps écoulé
  - `musicGetTrackCount()` — nombre de pistes

### Intégration display.h

- Page 5 ajoutée au cycle des pages
- Affiche : nom piste, barre progression, état play/pause, index
- Icônes musicales en XBM ou dessinées avec U8g2

### Intégration main.cpp

- `musicTick()` dans `loop()` (Core 0, même que le DAC timer)
- Boutons redirigés vers musique quand page == 5
- Commande WebSocket `music` traitée dans `onWsProxyEvent`

## Coexistence avec l'audio IA

- **Priorité IA** : si une réponse vocale arrive pendant la musique → pause auto, reprend après
- **Priorité PTT** : si PTT enfoncé → pause musique
- La musique utilise le même DAC timer — on remplace temporairement le callback
- Quand `dacHead != dacTail` (audio IA en cours) → musique en pause

## Impact mémoire

| Élément | Flash | RAM |
|---------|-------|-----|
| 6 mélodies RTTTL | ~1.5 Ko | 0 |
| Séquence notes parsée | 0 | ~800 octets (max 200 notes) |
| État lecteur | 0 | ~40 octets |
| **Total** | **~1.5 Ko** | **~840 octets** |

## config.h

```cpp
#define MUSIC_ENABLED       true
#define MUSIC_MAX_NOTES     200     // notes max par mélodie
#define MUSIC_DEFAULT_OCTAVE 6
#define MUSIC_DEFAULT_DURATION 4    // noire par défaut
#define MUSIC_DEFAULT_BPM    120
```
