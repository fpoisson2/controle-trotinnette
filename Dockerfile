FROM node:20-bookworm-slim

# Python + pip pour PlatformIO
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-pip python3-venv git \
    && rm -rf /var/lib/apt/lists/*

# PlatformIO dans un venv
RUN python3 -m venv /opt/pio && \
    /opt/pio/bin/pip install --no-cache-dir platformio
ENV PATH="/opt/pio/bin:$PATH"

# Pre-installer la plateforme + toolchain ESP32 pour accelerer les builds
RUN pio pkg install -g -p espressif32

WORKDIR /app
COPY package.json .
RUN npm install
COPY proxy.js .
COPY index.html .
# config.h est monté en volume via docker-compose.yml
RUN mkdir -p firmware/include
# Répertoire pour stocker les .bin pré-compilés
RUN mkdir -p builds

CMD ["node", "proxy.js"]
