# Landing Page — "LimoTrott" — Service de trottinettes du Cégep Limoilou

## Identité visuelle

**Nom** : LimoTrott (Limoilou + trottinette)

**Slogan du Cégep** : "Vois grand" / "Chemine comme personne"

**Logo Cégep** : `https://www.cegeplimoilou.ca/app/uploads/2025/08/limoilou_logo-1.png` (406x406)

### Palette de couleurs — basée sur la charte graphique officielle du Cégep Limoilou

Les couleurs sont extraites directement du site web du Cégep (`cegeplimoilou.ca`).

| Variable | Hex | Source Cégep | Usage LimoTrott |
|----------|-----|-------------|-----------------|
| `--color-primary` | `#ed1e20` | `--wp--preset--color--red` | **Rouge Limoilou** — CTA, accents forts, alertes |
| `--color-primary-dark` | `#343030` | `--wp--preset--color--charcoal` | Hover, footer, headings |
| `--color-darkgrey` | `#121212` | `--wp--preset--color--darkgrey` | Backgrounds sombres, nav |
| `--color-teal` | `#00f8ce` | `--wp--preset--color--teal` | Accents tech, disponibilité, "en ligne" |
| `--color-blue` | `#42f7fb` | `--wp--preset--color--blue` | Liens, éléments interactifs |
| `--color-green` | `#bae562` | `--wp--preset--color--green` | Éco, disponible, succès |
| `--color-yellow` | `#f6fd54` | `--wp--preset--color--yellow` | Badges, prix, attention |
| `--color-grey` | `#dcdcdc` | `--wp--preset--color--grey` | Bordures, backgrounds secondaires |
| `--color-bg` | `#ffffff` | `--wp--preset--color--white` | Background principal |
| `--color-text` | `#121212` | `--wp--preset--color--darkgrey` | Texte body |

**Note** : Le Cégep utilise une palette audacieuse avec du rouge vif + néons (teal, bleu cyan, vert lime, jaune). C'est une identité jeune et énergique, parfaite pour un service de trottinettes étudiantes. On reprend ces couleurs plutôt que d'inventer les nôtres.

### Typographie — identique au site du Cégep

```css
--font-sans: ui-sans-serif, system-ui, -apple-system, sans-serif;
--font-serif: ui-serif, Georgia, Cambria, serif;
```

Le Cégep utilise la stack système native (pas de police custom chargée). On garde la même approche — c'est rapide, cohérent, et aligné.

### Tailles de texte (du site Cégep)

| Token | Taille |
|-------|--------|
| `--text-small` | 13px |
| `--text-medium` | 20px |
| `--text-large` | 36px |
| `--text-xlarge` | 42px |

## Inspirations Communauto — Organisation de l'information

Communauto suit une architecture **hero-to-conversion** efficace qu'on adapte :

1. **Hero dominant** avec vidéo/photo de fond + proposition de valeur en 1 phrase
2. **Bienvenue** : 3 lignes qui expliquent le concept à quelqu'un qui ne connaît pas
3. **3 arguments** : Pratique / Économique / Écologique (pas "Sympathique" — on est sérieux)
4. **Comment ça marche** : étapes visuelles claires
5. **Tarifs** : grille simple, pas de surprise
6. **Carte** : véhicules disponibles en temps réel (CTA fort)
7. **Sécurité / Confiance** : rassurer avant l'inscription
8. **CTA final** : "Inscris-toi gratuitement"

### Style Communauto qu'on reprend
- Vidéo ou photo hero en plein écran avec texte blanc ombré
- Boutons pill (border-radius 9999px)
- Ton chaleureux et motivant ("Donne de l'air à ton campus!")
- Navigation par CTA, pas par menu complexe
- Fond alternant blanc / gris clair entre les sections

### Style Communauto qu'on adapte
- Communauto utilise du bleu cyan (`#00aeef`) — nous on utilise les couleurs du **Cégep** (rouge `#ed1e20` + teal `#00f8ce`)
- Communauto est corporate (vous) — nous on est étudiant (tu)
- Communauto a 10 forfaits — nous on en a 2 (gratuit / 2$)
- Communauto cible des adultes — nous on cible des 17-25 ans

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
