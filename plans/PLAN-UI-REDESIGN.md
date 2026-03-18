# Plan de refonte UI — Dashboard Trottinette

## Diagnostic

**Pourquoi ça fait "généré par IA"** :
- Palette = thème GitHub dark (`#0d1117`, `#58a6ff`) — le défaut de tout outil IA
- 4 jauges identiques en grille 2x2 — le cliché IoT dashboard
- Texte trop petit : 11-14px partout, illisible au soleil sur mobile
- Emoji comme icônes (🎤 🗺️ 📊)
- Header surchargé : titre + 3 dots + 3 labels + 4 boutons + badge dans 48px

## Nouvelle palette — violet/mint (pas bleu générique)

| Variable | Hex | Usage |
|----------|-----|-------|
| `--bg` | `#0a0a0f` | Near-black avec undertone bleu |
| `--surface` | `#14141f` | Cards, panels |
| `--surface-raised` | `#1c1c2b` | Hover, éléments élevés |
| `--accent` | `#7c5cff` | **Violet** — accent principal |
| `--speed` | `#00e5a0` | Mint électrique — vitesse |
| `--battery` | `#ffd43b` | Ambre chaud — batterie |
| `--current` | `#ff6b35` | Orange électrique — courant |
| `--temp` | `#ff4757` | Rouge-rose — température |
| `--ptt-idle` | `#7c5cff` | Ring PTT inactif |
| `--ptt-active` | `#ff4757` | PTT enregistrement |

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
