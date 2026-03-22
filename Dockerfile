FROM node:20-bookworm-slim

# Python + pip pour PlatformIO, build-essential pour better-sqlite3 (node-gyp)
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-pip python3-venv git \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

# PlatformIO dans un venv
RUN python3 -m venv /opt/pio && \
    /opt/pio/bin/pip install --no-cache-dir platformio
ENV PATH="/opt/pio/bin:$PATH"

# Pre-installer la plateforme + toolchain ESP32 pour accelerer les builds
RUN pio pkg install -g -p espressif32

WORKDIR /app

# Installer les dépendances Node (better-sqlite3 nécessite build tools)
COPY package.json .
RUN npm install

# Copier tous les fichiers applicatifs
COPY proxy.js .
COPY database.js .
COPY auth-routes.js .
COPY fleet.js .
COPY index.html .
COPY landing.html .
COPY css/ css/
COPY js/ js/

# Copier les sources firmware pour les builds OTA
# config.h est exclu (contient des credentials) — monté en volume au runtime
COPY firmware/ firmware/
RUN rm -f firmware/include/config.h
# Installer les dépendances PlatformIO du projet (lib + toolchain)
RUN cd firmware && pio pkg install -e esp32dev 2>/dev/null || true
# Copier le header custom WebSockets dans la lib installée
RUN find /root/.platformio -path "*/WebSockets/src" -type d 2>/dev/null | head -1 | \
    xargs -I{} cp firmware/include/WebSocketsNetworkClientSecure.h {} 2>/dev/null || true
# Répertoire pour stocker les .bin pré-compilés
RUN mkdir -p builds
# Répertoire pour les données persistantes (SQLite, fleet.json)
RUN mkdir -p data

EXPOSE 3000

CMD ["node", "proxy.js"]
