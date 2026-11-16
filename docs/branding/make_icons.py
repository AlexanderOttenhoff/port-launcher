# Usage: python3 docs/branding/make_icons.py docs/branding/drafts
# Generates controller icon drafts (original artwork) as 108x108 SVGs on the Android adaptive-icon grid.
# The silhouette is built from thick round-capped strokes, so overlapping parts merge without seams.
import pathlib, sys
out = pathlib.Path(sys.argv[1])

# Skeleton: body bar plus three prongs (left/right splay outwards), centred in the safe zone.
BODY = ('M30 46 L78 46', 26)
LEFT = ('M31 50 L24 76', 17)
CENTRE = ('M54 54 L54 79', 17)
RIGHT = ('M77 50 L84 76', 17)

def strokes(colours, extra=0):
    parts = zip((LEFT, RIGHT, CENTRE, BODY), colours)
    return ''.join(f'<path d="{d}" stroke="{c}" stroke-width="{w + extra}" stroke-linecap="round" fill="none"/>'
                   for (d, w), c in parts)

def details(colour, accent):
    return f'''
    <rect x="23" y="44" width="12" height="4" rx="1.2" fill="{colour}"/>
    <rect x="27" y="40" width="4" height="12" rx="1.2" fill="{colour}"/>
    <rect x="50" y="42" width="8" height="5" rx="2.5" fill="{colour}"/>
    <circle cx="54" cy="64" r="5.6" fill="{accent}"/>
    <circle cx="54" cy="64" r="2.4" fill="{colour}"/>
    <circle cx="79" cy="40.5" r="2.6" fill="{colour}"/>
    <circle cx="84.5" cy="46" r="2.6" fill="{colour}"/>
    <circle cx="79" cy="51.5" r="2.6" fill="{colour}"/>
    <circle cx="73.5" cy="46" r="2.6" fill="{colour}"/>'''

def svg(name, background, content, defs=''):
    (out / f'{name}.svg').write_text(f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 108 108" width="108" height="108">
  <defs>{defs}</defs>
  <rect width="108" height="108" fill="{background}"/>
  <g transform="translate(54 58) scale(0.82) translate(-54 -58)">{content}
  </g>
</svg>
''')

# A: one silhouette with a violet-to-teal gradient
svg('a-gradient', '#16122a',
    strokes(['url(#g)'] * 4) + details('#ffffff', '#16122a'),
    '<linearGradient id="g" gradientUnits="userSpaceOnUse" x1="14" y1="32" x2="94" y2="90">'
    '<stop offset="0" stop-color="#8b6bff"/><stop offset="1" stop-color="#22d3c5"/></linearGradient>')

# B: colour blocks in our own palette (not the N64 cube's red/yellow/green/blue)
svg('b-blocks', '#10131c',
    strokes(['#ff6b5b', '#2ec4b6', '#ffc23d', '#7b5cff']) + details('#10131c', '#ffffff'))

# C: neon outline: a wider cyan silhouette with the background drawn inside it
svg('c-neon', '#0b0b12',
    strokes(['#39e6ff'] * 4, extra=5) + strokes(['#0b0b12'] * 4) + details('#ff4fd8', '#39e6ff'))
