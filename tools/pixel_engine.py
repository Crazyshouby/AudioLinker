"""Moteur des pictogrammes pixel (16x16, contour noir, aplats) de la suite KDX : palette et toile.

Chaque pictogramme se dessine avec quelques primitives, se contourne seul et s'exporte en SVG compact (un <path> par couleur).
Source unique : kdx-ui/gen/pixel_engine.py ; KIRC et AudioLinker en ont une copie (charte-sync) et y ajoutent leurs propres
pictogrammes (pixel_icons.py). svg(accent={}) garde le rouge fixe (AudioLinker) ; par défaut le rouge suit l'accent du thème.
"""
import math  # noqa: F401  (utilisé par les modules d'icônes qui font « from pixel_engine import * »)

PAL = {'K': '#000000', 'Y': '#fcc21c', 'y': '#c4840c', 'W': '#fff6dc', 'w': '#f4f4f4', 'g': '#c8c8c8', 'G': '#8c8c8c',
       'd': '#4a4a4a', 'D': '#2a2a2a', 'R': '#e02020', 'r': '#8a0202', 'O': '#f58a1a', 'o': '#b85a00',
       'E': '#e02020', 'e': '#8a0202',  # rouge de STATUT (erreur) : fixe, ne suit pas l'accent
       'B': '#2a5ae0', 'b': '#86a6ff', 'N': '#0a1a80', 'L': '#3ccf3c', 'l': '#0a7a14', 'P': '#ff5cf0', 'p': '#a0189a'}


# Le rouge des icônes (R, r) suit l'accent du thème : variables CSS --ic-accent / --ic-accent-dark (tokens.json), avec l'ancienne valeur en repli
# pour les pages qui ne chargent pas kdx-ui.css. Les autres couleurs (jaune, bleu, vert...) restent fixes : ce sont des couleurs d'objet.
ACCENT_FILL = {'R': 'var(--ic-accent,#e02020)', 'r': 'var(--ic-accent-dark,#8a0202)'}


class C:
    def __init__(s):
        s.p = {}

    def set(s, x, y, c):
        if 0 <= x < 16 and 0 <= y < 16:
            s.p[(x, y)] = c

    def rect(s, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                s.set(x, y, c)

    def circ(s, cx, cy, r, c):
        for y in range(16):
            for x in range(16):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    s.set(x, y, c)

    def line(s, x0, y0, x1, y1, c):
        n = max(abs(x1 - x0), abs(y1 - y0)) or 1
        for i in range(n + 1):
            s.set(round(x0 + (x1 - x0) * i / n), round(y0 + (y1 - y0) * i / n), c)

    def pat(s, x0, y0, rows):
        for j, row in enumerate(rows):
            for i, ch in enumerate(row):
                if ch != '.':
                    s.set(x0 + i, y0 + j, ch)

    def outline(s):
        add = {}
        for (x, y), c in s.p.items():
            if c == 'K':
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                q = (x + dx, y + dy)
                if q not in s.p and 0 <= q[0] < 16 and 0 <= q[1] < 16:
                    add[q] = 'K'
        s.p.update(add)
        return s

    def svg(s, accent=ACCENT_FILL):
        by = {}
        for y in range(16):
            x = 0
            while x < 16:
                c = s.p.get((x, y))
                if c is None:
                    x += 1
                    continue
                x2 = x
                while x2 + 1 < 16 and s.p.get((x2 + 1, y)) == c:
                    x2 += 1
                w = x2 - x + 1
                by.setdefault(c, []).append(f'M{x} {y}h{w}v1h-{w}z')
                x = x2 + 1
        paths = ''.join(f'<path fill="{accent.get(c, PAL[c])}" d="{"".join(d)}"/>' for c, d in by.items())
        return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" width="16" height="16" '
                f'shape-rendering="crispEdges">{paths}</svg>')
