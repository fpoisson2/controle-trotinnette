# Plan de refactoring — Séparation Frontend/Backend

## Recommandation : Vue 3 + Vite

- Réactivité élimine la duplication mobile/desktop (l'index.html a deux DOM parallèles)
- Single-File Components pour chaque panneau
- Composables pour SSE, WebSocket, audio
- Vite avec proxy dev vers le backend

## Structure cible

```
controle-trotinnette/
├── backend/
│   ├── server.js                    (entry point)
│   ├── routes/
│   │   ├── auth.js                  (POST /login)
│   │   ├── scooters.js              (GET/POST /api/scooters/*)
│   │   ├── commands.js              (POST /cmd, /debug, /audio)
│   │   ├── ota.js                   (POST /ota, /api/ota/flash, releases)
│   │   └── status.js                (GET /status, /stream SSE)
│   ├── services/
│   │   ├── scooter-manager.js       (Map scooters, sélection, broadcastSSE)
│   │   ├── openai-realtime.js       (WebSocket OpenAI, tools)
│   │   ├── local-ai.js              (Whisper + Ollama + Piper)
│   │   ├── firmware-builder.js      (GitHub polling, PlatformIO)
│   │   └── ota.js                   (flashFirmware, état OTA)
│   ├── middleware/
│   │   └── auth.js                  (JWT, rate limiting)
│   └── websocket/
│       ├── esp32.js                 (handler multi-trottinettes)
│       └── debug-audio.js
├── frontend/
│   ├── vite.config.js
│   └── src/
│       ├── composables/
│       │   ├── useAuth.js, useSSE.js, useTelemetry.js
│       │   ├── useAudio.js, useScooters.js, useOTA.js
│       ├── components/
│       │   ├── MapPanel.vue, GaugeItem.vue, VoiceControl.vue
│       │   ├── ManualControl.vue, VoiceLog.vue, DebugModal.vue
│       │   ├── ScooterSelector.vue, BottomNav.vue
│       └── views/
│           ├── DashboardView.vue
│           └── LandingView.vue
├── firmware/                         (inchangé)
└── Dockerfile                        (multi-stage)
```

## Dockerfile multi-stage

```dockerfile
# Stage 1: Build frontend
FROM node:20-alpine AS frontend-build
WORKDIR /app/frontend
COPY frontend/ ./
RUN npm ci && npm run build

# Stage 2: Backend + frontend built
FROM node:20-bookworm-slim
# ... PlatformIO ...
COPY backend/ ./
COPY --from=frontend-build /app/frontend/dist ./public
CMD ["node", "server.js"]
```

## Migration incrémentale (5 phases)

### Phase 1 : Backend restructure
Split proxy.js en modules. index.html servi depuis `backend/public/`. Aucun changement frontend.

### Phase 2 : Scaffold Vue
Vue 3 + Vite, login + SSE fonctionnels. Vite proxy vers backend:3000.

### Phase 3 : Migration composant par composant
1. GaugeItem + TelemetryPanel (pure display)
2. VoiceLog (SSE)
3. ManualControl (POST /cmd)
4. MapPanel (Leaflet)
5. AppHeader (status)
6. VoiceControl + useAudio (le plus complexe)
7. DebugModal + OTAPanel
8. BottomNav
9. DashboardView (assemblage)

### Phase 4 : Docker multi-stage

### Phase 5 : Nouvelles features
- Vue Router : `/` (landing) + `/dashboard` (auth)
- ScooterSelector, vues par rôle
