# Améliorations OTA — Recommandations

## Problèmes actuels

### 1. Cold build à chaque mise à jour
Chaque `POST /api/ota/github` télécharge le source, installe les dépendances PlatformIO et recompile de zéro. Ça prend ~3-5 minutes.

**Solution** : Monter un volume Docker pour le cache PlatformIO :
```yaml
# docker-compose.yml
services:
  proxy:
    volumes:
      - pio-cache:/root/.platformio
volumes:
  pio-cache:
```
Les builds suivants réutilisent le cache et compilent en ~30 secondes.

### 2. config.h figé dans l'image Docker
Si les credentials WiFi/EAP changent, il faut rebuild l'image Docker.

**Solution** : Monter config.h en volume au lieu du `COPY` dans le Dockerfile :
```yaml
# docker-compose.yml
services:
  proxy:
    volumes:
      - ./firmware/include/config.h:/app/firmware/include/config.h:ro
```
Et retirer la ligne `COPY firmware/include/config.h ...` du Dockerfile.

### 3. Une release par push sur main
Un fix typo dans le README crée une release inutile.

**Solutions possibles** :
- Déclencher le workflow manuellement (`workflow_dispatch`) au lieu de sur chaque push
- Filtrer par fichiers modifiés (ne créer une release que si `firmware/` a changé) :
```yaml
on:
  push:
    branches: [main]
    paths:
      - 'firmware/**'
      - 'proxy.js'
      - 'index.html'
```

### 4. Pas de rollback
Si une mise à jour cause un problème, il n'y a aucun moyen de revenir à la version précédente depuis le dashboard.

**Solution** : Ajouter un `GET /api/releases` qui liste les N dernières releases, et permettre de choisir quelle version flasher (pas seulement la dernière).

### 5. Pas de vérification d'intégrité
Le `.bin` compilé n'est pas vérifié avant le flash. Une corruption réseau ou un build partiel pourrait bricker l'ESP32.

**Solution** : Calculer un hash SHA256 du `.bin` après compilation, l'afficher dans le dashboard, et vérifier côté ESP32 avant d'appliquer l'OTA (la lib ArduinoOTA supporte la vérification MD5).

### 6. Taille de l'image Docker
L'ajout de Python + PlatformIO + toolchain ESP32 rend l'image lourde (~2 Go).

**Solution** : Build multi-stage — une image builder avec PlatformIO, et l'image finale sans. Mais ça empêche la compilation à la demande. Compromis acceptable si le serveur a l'espace.

Alternative : pré-compiler le `.bin` sur la machine de dev et l'uploader sur un stockage privé (S3, serveur perso) au lieu de compiler dans Docker.

### 7. Sécurité OTA
Le firmware est flashé sans signature. Quelqu'un avec accès au JWT pourrait flasher un firmware malveillant via l'upload manuel.

**Solution** : Signer le firmware avec une clé privée et vérifier la signature côté ESP32 avant le flash. ESP-IDF supporte le Secure Boot nativement.
