#!/bin/bash
# start.sh — Démarrage intelligent du projet
# Usage : ./start.sh [build]
#
# - Démarre toujours le proxy
# - Démarre cloudflared uniquement si TUNNEL_TOKEN est défini dans .env
# - Passer "build" en argument pour forcer un rebuild

set -e

# Charger .env si présent
if [ -f .env ]; then
  export $(grep -v '^#' .env | grep -v '^$' | xargs)
fi

BUILD_FLAG=""
if [ "$1" = "build" ]; then
  BUILD_FLAG="--build"
fi

if [ -n "$TUNNEL_TOKEN" ]; then
  echo "[start] TUNNEL_TOKEN détecté — démarrage proxy + cloudflared"
  docker compose --profile tunnel up -d $BUILD_FLAG
else
  echo "[start] Pas de TUNNEL_TOKEN — démarrage proxy seulement (port ${PROXY_PORT:-3000})"
  docker compose up -d $BUILD_FLAG
fi

echo ""
echo "[start] En cours..."
docker compose ps
echo ""
echo "[start] Logs : docker compose logs -f proxy"
echo "[start] URL  : http://localhost:${PROXY_PORT:-3000}"
