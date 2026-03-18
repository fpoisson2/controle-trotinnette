# Plan de refonte UI — Dashboard Trottinette

## Diagnostic

**Pourquoi ça fait "généré par IA"** :
- Palette = thème GitHub dark (`#0d1117`, `#58a6ff`) — le défaut de tout outil IA
- 4 jauges identiques en grille 2x2 — le cliché IoT dashboard
- Texte trop petit : 11-14px partout, illisible au soleil sur mobile
- Emoji comme icônes (🎤 🗺️ 📊)
- Header surchargé : titre + 3 dots + 3 labels + 4 boutons + badge dans 48px

## Nouvelle palette — basée sur l'identité graphique du Cégep Limoilou

Le Cégep utilise une palette audacieuse : rouge vif + néons (teal, cyan, vert lime, jaune) sur fond sombre.
On adapte ces couleurs au dashboard dark mode pour une cohérence visuelle totale avec la landing page.

| Variable | Hex | Source | Usage dashboard |
|----------|-----|--------|-----------------|
| `--bg` | `#121212` | Cégep `darkgrey` | Background principal |
| `--surface` | `#1e1e1e` | Dérivé du charcoal Cégep | Cards, panels |
| `--surface-raised` | `#2a2a2a` | Dérivé | Hover, éléments élevés |
| `--accent` | `#00f8ce` | Cégep `teal` | **Accent principal** — liens, sélection |
| `--accent-alt` | `#42f7fb` | Cégep `blue` | Accents secondaires |
| `--speed` | `#00f8ce` | Cégep `teal` | Vitesse — speedomètre |
| `--battery` | `#bae562` | Cégep `green` | Batterie — vert lime |
| `--current` | `#f6fd54` | Cégep `yellow` | Courant — jaune néon |
| `--temp` | `#ed1e20` | Cégep `red` | Température / danger |
| `--danger` | `#ed1e20` | Cégep `red` | Rouge Limoilou — erreurs, arrêt urgence |
| `--ptt-idle` | `#00f8ce` | Cégep `teal` | Ring PTT inactif |
| `--ptt-active` | `#ed1e20` | Cégep `red` | PTT enregistrement |
| `--text` | `#f0f0f5` | — | Texte principal |
| `--text-secondary` | `#dcdcdc` | Cégep `grey` | Texte secondaire |
| `--border` | `#343030` | Cégep `charcoal` | Bordures |

**Pourquoi c'est mieux que violet/mint** : c'est la même identité que la landing page et le site du Cégep. Un étudiant qui vient du site Limoilou reconnaît immédiatement les couleurs. Le teal néon (`#00f8ce`) est distinctif et pas du tout "généré par IA".

## Typographie (Inter, base 16px mobile)

| Rôle | Mobile | Desktop |
|------|--------|---------|
| Vitesse (hero) | 72px | 64px |
| Unité "km/h" | 18px | 16px |
| Valeurs jauges | 28px | 24px |
| Labels | 13px uppercase | 12px |
| Body/logs | 15px | 14px |
| Boutons | 15px | 14px |

## Layout mobile — speed hero + bottom sheets

```
┌────────────────────────────────┐
│  [Scooter ▼]    [Status dots]  │  56px top bar
├────────────────────────────────┤
│         SPEED: 23              │  Speedomètre dominant
│          km/h                  │
│  42V ██████░░ | 12.3A | 34°C  │  Strip horizontal
├────────────────────────────────┤
│     [ Mini Map — 30% ]         │  Carte toujours visible
├────────────────────────────────┤
│       ( PTT BUTTON )           │  Flottant, toujours accessible
│   "Dis quelque chose..."       │
├────────────────────────────────┤
│ [Contrôle] [Journal] [Debug]   │  Bottom sheets (pas tabs)
└────────────────────────────────┘
```

## Layout desktop — deux colonnes

- Gauche (380px) : speedomètre + télémétrie + PTT + contrôles manuels
- Droite : carte (60%) + journal vocal (40%)
- Tout visible en même temps, pas de tabs

## Ce qui rend l'UI NON-IA

1. **Hiérarchie asymétrique** : vitesse 72px, tout le reste subordonné
2. **Violet au lieu de bleu** — aucun outil IA ne défaut sur le violet
3. **Un speedomètre hero** au lieu de 4 jauges identiques
4. **Bottom sheets** au lieu de tab bar (style Apple Maps)
5. **SVG custom** au lieu d'emoji
6. **Gradient subtil** au lieu d'un fond plat
7. **PTT avec ripple concentrique** au lieu d'un pulse générique
8. **Marqueur scooter SVG** au lieu d'un point bleu
9. **Espacement varié** — pas uniforme partout
10. **Pas de title bars** sur les panneaux auto-explicatifs

## Phases d'implémentation

1. CSS variables + typographie
2. Swap palette de couleurs
3. Layout mobile (speed hero + bottom sheets)
4. Layout desktop (deux colonnes)
5. Composants : speedomètre conic-gradient, pills télémétrie, PTT ripple
6. Sélecteur de trottinette
7. Polish : gradients, glow, transitions, marqueur carte
