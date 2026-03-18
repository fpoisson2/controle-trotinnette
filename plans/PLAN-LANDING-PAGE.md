# Landing Page — "LimoTrott" — Service de trottinettes du Cégep Limoilou

## Identité visuelle

**Nom** : LimoTrott (Limoilou + trottinette)

### Palette de couleurs

| Variable | Hex | Usage |
|----------|-----|-------|
| `--color-primary` | `#1B7A3D` | Vert CTA, headings |
| `--color-primary-dark` | `#145C2E` | Hover, footer |
| `--color-primary-light` | `#E8F5EC` | Backgrounds légers |
| `--color-secondary` | `#1A3A5C` | Bleu institutionnel (Cégep) |
| `--color-accent` | `#F4A623` | Badges, prix |
| `--color-bg` | `#FAFCFB` | Background page |
| `--color-text` | `#1A1A2E` | Texte body |

### Typographie
- Headings : Plus Jakarta Sans (moderne, géométrique)
- Body : Inter (lisible sur mobile)
- Mono : JetBrains Mono (prix)

## Sections

### 1. Hero (100vh)
- Eyebrow : "Nouveau au campus"
- Titre : **"Déplace-toi au campus. Par la voix."**
- Sous-titre : "Des trottinettes électriques en libre-service, contrôlées par la voix."
- CTA : "Essaie gratuitement" + "Voir la carte"
- Visuel : photo étudiant + bulles UI flottantes (vitesse, batterie, commande vocale)

### 2. Comment ça marche (4 étapes)
1. **Crée ton compte** — courriel @cegeplimoilou.ca, 30 secondes
2. **Scanne et débarque** — QR code sur la trottinette
3. **Parle, roule** — commande vocale ou manette
4. **Stationne et verrouille** — zone désignée, verrouillage auto

### 3. Carte campus
- Background bleu foncé pour le contraste
- Leaflet avec tuiles CartoDB Voyager
- Marqueurs verts (disponible), jaunes (en charge), gris (indisponible)
- Stats : "8 disponibles" | "2 en charge" | "Campus Charlesbourg"

### 4. Tarifs

**Étudiants et personnel** (carte mise en avant) :
- **0 $** par trajet
- Trajets illimités, contrôle vocal, casque prêté
- Inscription avec @cegeplimoilou.ca

**Visiteurs** :
- **2 $** par trajet (max 30 min)
- Paiement par carte de crédit

### 5. Sécurité
- Contrôle vocal intelligent (ralentissement auto zones piétonnes)
- Casque obligatoire (prêté gratuitement)
- Vitesse limitée à 20 km/h (10 km/h zones achalandées)

### 6. Footer
- Logo LimoTrott + "Mobilité verte pour le campus"
- Navigation, FAQ, conditions d'utilisation
- Contact : Cégep Limoilou, Campus Charlesbourg
- "Fait avec ❤ à Québec — Propulsé par ESP32 + OpenAI"

## Intégration technique

| URL | Fichier | Rôle |
|-----|---------|------|
| `/` | `landing.html` | Page publique |
| `/dashboard` | `index.html` | Dashboard technique (auth) |

- "Se connecter" → redirige vers `/dashboard` (login overlay existant)
- Carte temps réel via `GET /api/scooters/available` (endpoint public)
- Mobile-first, breakpoint 768px
