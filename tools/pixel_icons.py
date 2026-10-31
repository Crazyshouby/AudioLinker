"""Pixel-art icon set (16x16, black outline, flat colours) of the KDX-style UI.

Each icon is drawn from a few primitives, outlined automatically and exported
as a compact SVG (one <path> per colour). Imported by gen_assets.py, which
writes them into assets/icons.js.
"""
import math

from pixel_engine import C, PAL  # noqa: F401  (copie de cdx-ui/gen/pixel_engine.py)


def speaker():
    c = C()
    c.rect(3, 1, 12, 14, 'Y'); c.rect(3, 2, 3, 13, 'W'); c.rect(12, 2, 12, 13, 'y'); c.rect(3, 14, 12, 14, 'y')
    c.circ(7.5, 4.5, 2, 'K'); c.circ(7.5, 4.5, 1.2, 'D'); c.set(7, 4, 'G')
    c.circ(7.5, 9.7, 3.7, 'K'); c.circ(7.5, 9.7, 2.8, 'd'); c.circ(7.5, 9.7, 1.4, 'D'); c.set(6, 8, 'G'); c.set(5, 9, 'G')
    return c.outline()


def headphones():
    c = C()
    for y in range(0, 9):
        for x in range(16):
            r = math.hypot(x - 7.5, y - 8)
            if 5.0 <= r <= 7.0:
                c.set(x, y, 'g' if x < 8 else 'G')
    c.rect(1, 8, 4, 13, 'O'); c.rect(11, 8, 14, 13, 'O')
    c.rect(1, 8, 1, 13, 'W'); c.rect(11, 8, 11, 13, 'W'); c.rect(4, 9, 4, 13, 'o'); c.rect(14, 9, 14, 13, 'o')
    return c.outline()


def btspeaker():
    c = C()
    c.rect(2, 1, 13, 14, 'B'); c.rect(2, 2, 2, 13, 'b'); c.rect(2, 14, 13, 14, 'N'); c.rect(13, 2, 13, 13, 'N')
    c.pat(6, 3, ['..w..', '..ww.', 'w.w.w', '.www.', '..w..', '.www.', 'w.w.w', '..ww.', '..w..'])
    return c.outline()


def monitor():
    c = C()
    c.rect(1, 1, 14, 11, 'g'); c.rect(1, 10, 14, 11, 'G'); c.rect(3, 3, 12, 9, 'B'); c.rect(3, 7, 12, 9, 'N')
    c.rect(3, 3, 12, 3, 'b'); c.rect(3, 4, 7, 4, 'b')
    c.rect(6, 12, 9, 13, 'G'); c.rect(4, 14, 11, 14, 'g'); c.set(12, 10, 'L')
    return c.outline()


def mic():
    c = C()
    c.rect(5, 0, 10, 7, 'g'); c.rect(5, 0, 5, 7, 'w'); c.rect(10, 0, 10, 7, 'G')
    for y in range(1, 7):
        for x in range(6, 10):
            if (x + y) % 2 == 0:
                c.set(x, y, 'd')
    for y in range(6, 11):
        for x in range(16):
            r = math.hypot(x - 7.5, y - 5.5)
            if 4.0 <= r <= 5.4:
                c.set(x, y, 'w')
    c.rect(7, 11, 8, 12, 'G'); c.rect(4, 13, 11, 14, 'g'); c.rect(4, 14, 11, 14, 'G')
    return c.outline()


def folder():
    c = C()
    c.rect(1, 2, 6, 4, 'Y'); c.rect(1, 4, 14, 13, 'Y'); c.rect(1, 4, 14, 4, 'W')
    c.rect(1, 13, 14, 13, 'y'); c.rect(14, 5, 14, 13, 'y')
    return c.outline()


def eq():
    c = C()
    c.rect(1, 1, 14, 14, 'D')
    for x in (4, 8, 12):
        c.rect(x, 2, x, 13, 'G')
    for x, y, col in ((4, 9, 'R'), (8, 4, 'O'), (12, 7, 'Y')):
        c.rect(x - 1, y, x + 1, y + 1, col); c.rect(x - 1, y, x + 1, y, 'w')
    return c.outline()


def journal():
    c = C()
    c.rect(3, 0, 12, 14, 'w')
    for x, y in ((10, 0), (11, 0), (12, 0), (11, 1), (12, 1), (12, 2)):
        c.p.pop((x, y), None)
    c.rect(9, 1, 10, 3, 'g'); c.rect(11, 3, 12, 3, 'g')
    c.rect(4, 5, 10, 5, 'd'); c.rect(4, 7, 11, 7, 'd'); c.rect(4, 9, 9, 9, 'd'); c.rect(4, 11, 11, 11, 'd')
    c.rect(12, 4, 12, 14, 'g'); c.rect(3, 14, 12, 14, 'G')
    return c.outline()


def gear():
    c = C()
    for y in range(16):
        for x in range(16):
            dx, dy = x - 7.5, y - 7.5
            r = math.hypot(dx, dy)
            a = math.degrees(math.atan2(dy, dx)) % 45
            tooth = r <= 7.4 and (a <= 14 or a >= 31)
            if r <= 5.4 or tooth:
                c.set(x, y, 'Y' if (x + y) < 15 else 'O')
            if r <= 2.4:
                c.p.pop((x, y), None)
    return c.outline()


def spk_glyph(c, col='g'):
    c.rect(1, 5, 3, 10, col); c.rect(4, 4, 5, 11, col); c.rect(6, 2, 7, 13, col); c.rect(1, 5, 1, 10, 'w')


def mute():
    c = C()
    spk_glyph(c)
    c.line(9, 4, 14, 11, 'R'); c.line(10, 4, 15, 11, 'R'); c.line(14, 4, 9, 11, 'R'); c.line(15, 4, 10, 11, 'R')
    return c.outline()


def volume():
    c = C()
    spk_glyph(c)
    for r, col in ((3.2, 'b'), (5.4, 'B')):
        for y in range(16):
            for x in range(16):
                d = math.hypot(x - 7, y - 7.5)
                if r - 0.6 <= d <= r + 0.6 and x >= 9 and abs(y - 7.5) <= r * 0.85:
                    c.set(x, y, col)
    return c.outline()


def cable():
    c = C()
    c.rect(0, 6, 4, 9, 'D'); c.rect(1, 6, 1, 9, 'd'); c.rect(5, 7, 10, 8, 'g'); c.rect(5, 7, 10, 7, 'w')
    c.rect(7, 6, 7, 9, 'R'); c.rect(11, 7, 13, 8, 'G'); c.set(14, 7, 'G'); c.set(14, 8, 'G'); c.rect(0, 6, 4, 6, 'd')
    return c.outline()


def floppy():
    c = C()
    c.rect(1, 1, 14, 14, 'B'); c.rect(1, 1, 1, 14, 'b'); c.rect(14, 2, 14, 14, 'N'); c.rect(1, 14, 14, 14, 'N')
    c.rect(4, 1, 11, 5, 'g'); c.rect(4, 5, 11, 5, 'G'); c.rect(8, 2, 9, 4, 'd')
    c.rect(3, 8, 12, 13, 'w'); c.rect(4, 10, 11, 10, 'G'); c.rect(4, 12, 9, 12, 'G')
    for x, y in ((14, 1), (13, 1), (14, 2)):
        c.p.pop((x, y), None)
    return c.outline()


ICONS = dict(enceinte=speaker, casque=headphones, bluetooth=btspeaker, ecran=monitor, micro=mic, setups=folder,
             egaliseur=eq, journal=journal, options=gear, muet=mute, volume=volume, cable=cable, disquette=floppy)


def all_svgs():
    return {name: draw().svg({}) for name, draw in ICONS.items()}
