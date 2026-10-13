#!/usr/bin/env python3
"""Generate the MagNET app icon: a magnetic dipole field, rendered as light.

The mark is the field of a bar magnet — the loops follow the real dipole
equation r = L·sin²θ, which is why it reads as "magnetic" rather than as
generic concentric rings. A hot core sits on the axis; the loops cool from
cyan at the poles through violet to magenta as they travel out.

Rendered at 4× and downsampled, so the curves are smooth without needing a
vector rasteriser. Two passes per line — a blurred copy for the glow, then the
crisp stroke over it — give the light-emitting look.

    python3 tool/gen_icon.py            # writes assets/images/*.png
"""
import math
import pathlib

from PIL import Image, ImageDraw, ImageFilter

SS = 4                      # supersample factor
OUT = 1024                  # master icon edge
S = OUT * SS
CENTER = (S / 2, S / 2)

# Palette — cool poles, warm equator, on deep space.
BG_INNER = (19, 26, 46)
BG_OUTER = (5, 6, 11)
STOPS = [                   # position along a field line → colour
    (0.00, (34, 211, 238)),     # cyan
    (0.35, (99, 102, 241)),     # indigo
    (0.65, (167, 139, 250)),    # violet
    (1.00, (244, 114, 182)),    # magenta
]


def lerp(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def ramp(t):
    """Colour at position t (0..1) along the gradient."""
    t = max(0.0, min(1.0, t))
    for (p0, c0), (p1, c1) in zip(STOPS, STOPS[1:]):
        if t <= p1:
            span = p1 - p0 or 1.0
            return lerp(c0, c1, (t - p0) / span)
    return STOPS[-1][1]


def background():
    """Radial gradient, drawn as nested ellipses then blurred smooth."""
    img = Image.new("RGB", (S, S), BG_OUTER)
    d = ImageDraw.Draw(img)
    steps = 160
    for i in range(steps, 0, -1):
        t = i / steps
        r = t * S * 0.78
        d.ellipse(
            [CENTER[0] - r, CENTER[1] - r, CENTER[0] + r, CENTER[1] + r],
            fill=lerp(BG_INNER, BG_OUTER, t),
        )
    return img.filter(ImageFilter.GaussianBlur(S / 90))


# A true dipole loop is about 2.6:1 wide, which leaves an app icon mostly
# empty top and bottom. Stretching the axis keeps the physics recognisable
# while filling the square; the safe factor keeps the widest loop clear of
# the rounded-corner mask iOS/macOS apply.
Y_STRETCH = 1.55
SAFE = 0.40          # widest loop radius as a fraction of the icon edge


def field_line(L, mirror=False, n=420):
    """Dipole field line r = L·sin²θ, axis vertical. Returns polyline points."""
    pts = []
    for i in range(n + 1):
        th = math.pi * i / n
        r = L * math.sin(th) ** 2
        x = r * math.sin(th)
        y = r * math.cos(th) * Y_STRETCH
        if mirror:
            x = -x
        pts.append((CENTER[0] + x, CENTER[1] - y))
    return pts


def stroke(layer, pts, width, alpha=255, tint=None):
    """Draw a polyline with the colour ramp applied along its length."""
    d = ImageDraw.Draw(layer)
    n = len(pts) - 1
    for i in range(n):
        # position along the loop: 0 at the top pole, 1 at the equator, back to 0
        t = 1.0 - abs(2.0 * (i / n) - 1.0)
        c = tint or ramp(t)
        d.line([pts[i], pts[i + 1]], fill=c + (alpha,), width=width)
        # round the joints so thick strokes don't show facets
        r = width / 2
        d.ellipse([pts[i][0] - r, pts[i][1] - r, pts[i][0] + r, pts[i][1] + r],
                  fill=c + (alpha,))


def core(layer):
    """The magnet itself: a hot capsule on the dipole axis."""
    d = ImageDraw.Draw(layer)
    h, w = S * 0.255, S * 0.056
    box = [CENTER[0] - w, CENTER[1] - h, CENTER[0] + w, CENTER[1] + h]
    # poles tinted like the field they launch
    d.rounded_rectangle(box, radius=w, fill=(226, 232, 240, 255))
    d.rounded_rectangle([box[0], box[1], box[2], CENTER[1] - h * 0.30],
                        radius=w, fill=(34, 211, 238, 255))
    d.rounded_rectangle([box[0], CENTER[1] + h * 0.30, box[2], box[3]],
                        radius=w, fill=(244, 114, 182, 255))
    # white-hot centre
    d.rounded_rectangle(
        [CENTER[0] - w * 0.42, CENTER[1] - h * 0.55,
         CENTER[0] + w * 0.42, CENTER[1] + h * 0.55],
        radius=w * 0.42, fill=(255, 255, 255, 235))


# Two line sets. The full field is what the mark is *for* — a dense dipole
# reads as a real field rather than a pair of rings. But below ~64 px the
# inner lines merge into a smear, so small icon slots get the reduced set.
# The appiconset lets each size carry its own art, so we use that rather
# than compromising the large sizes.
LOOPS_FULL = [(0.30, 34), (0.50, 40), (0.72, 42), (1.00, 38)]
LOOPS_SMALL = [(0.60, 52), (1.00, 46)]
DETAIL_CUTOFF = 64          # px at or below which the reduced set is used


def render(loops=None):
    img = background().convert("RGBA")

    spec = loops or LOOPS_FULL
    lines = [(S * SAFE * f, w) for f, w in spec]

    # glow pass — fat, blurred, additive
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    for L, w in lines:
        for mirror in (False, True):
            stroke(glow, field_line(L, mirror), int(w * 4.2), alpha=150)
    core(glow)
    glow = glow.filter(ImageFilter.GaussianBlur(S / 55))
    img = Image.alpha_composite(img, glow)
    img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(S / 22)))

    # crisp pass
    sharp = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    for L, w in lines:
        for mirror in (False, True):
            stroke(sharp, field_line(L, mirror), w)
    core(sharp)
    img = Image.alpha_composite(img, sharp)

    return img.convert("RGB").resize((OUT, OUT), Image.LANCZOS)


def main():
    here = pathlib.Path(__file__).resolve().parent.parent
    assets = here / "assets" / "images"
    assets.mkdir(parents=True, exist_ok=True)

    icon = render()
    small = render(LOOPS_SMALL)
    icon.save(assets / "magnet-icon-1024.png")
    icon.resize((512, 512), Image.LANCZOS).save(assets / "magnet-icon-512.png")
    icon.resize((192, 192), Image.LANCZOS).save(assets / "icon-192.png")

    # Splash: the mark alone on a transparent field, so the splash background
    # colour shows through and the platform can letterbox it cleanly.
    sp = Image.new("RGBA", (OUT, OUT), (0, 0, 0, 0))
    full = icon.convert("RGBA")
    # keep the luminous mark, drop the background gradient
    px_src, px_dst = full.load(), sp.load()
    for y in range(OUT):
        for x in range(OUT):
            r, g, b, _ = px_src[x, y]
            lum = max(r, g, b)
            # background tops out well below the field lines
            a = 0 if lum < 46 else min(255, int((lum - 46) * 1.9))
            px_dst[x, y] = (r, g, b, a)
    sp.save(assets / "magnet-splash.png")

    # macOS wants a squircle floating on transparency, not a full-bleed
    # square — a bleeding square reads as foreign in the Dock. Apple's grid
    # puts the art at ~80% of the canvas with a ~22.5% corner radius.
    macos_icon(icon, small, here)
    ios_icons(icon, small, here)
    android_icons(icon, small, here)

    print("wrote:", ", ".join(sorted(p.name for p in assets.glob("magnet-*.png"))))


def android_icons(icon, small, root):
    """Same per-size rule for the Android mipmaps (mdpi is 48 px)."""
    base = root / "android" / "app" / "src" / "main" / "res"
    if not base.is_dir():
        return
    n = 0
    for f in sorted(base.glob("mipmap-*/ic_launcher.png")):
        with Image.open(f) as im:
            w = im.size[0]
        src = small if w <= DETAIL_CUTOFF else icon
        src.resize((w, w), Image.LANCZOS).convert("RGB").save(f)
        n += 1
    print(f"wrote {n} Android mipmaps -> {base}")


def ios_icons(icon, small, root):
    """Rewrite the iOS appiconset so each slot gets size-appropriate art.

    flutter_launcher_icons resizes one master into every slot; run this after
    it to swap the small slots for the reduced-detail field.
    """
    dest = root / "ios" / "Runner" / "Assets.xcassets" / "AppIcon.appiconset"
    if not dest.is_dir():
        return
    n = 0
    for f in sorted(dest.glob("*.png")):
        with Image.open(f) as im:
            w = im.size[0]
        src = small if w <= DETAIL_CUTOFF else icon
        src.resize((w, w), Image.LANCZOS).convert("RGB").save(f)
        n += 1
    print(f"wrote {n} iOS slots -> {dest}")


def macos_icon(icon, small, root):
    """Write the macOS .appiconset directly, in Apple's squircle grid."""
    from PIL import ImageDraw as _D
    dest = root / "macos" / "Runner" / "Assets.xcassets" / "AppIcon.appiconset"
    if not dest.is_dir():
        return
    for size in (16, 32, 64, 128, 256, 512, 1024):
        ss = 4 if size <= 256 else 1
        c = size * ss
        art = int(c * 0.80)
        canvas = Image.new("RGBA", (c, c), (0, 0, 0, 0))
        source = small if size <= DETAIL_CUTOFF else icon
        body = source.resize((art, art), Image.LANCZOS).convert("RGBA")
        mask = Image.new("L", (art, art), 0)
        _D.Draw(mask).rounded_rectangle(
            [0, 0, art - 1, art - 1], radius=int(art * 0.225), fill=255)
        off = ((c - art) // 2, (c - art) // 2)
        canvas.paste(body, off, mask)
        if ss > 1:
            canvas = canvas.resize((size, size), Image.LANCZOS)
        canvas.save(dest / f"app_icon_{size}.png")
    print(f"wrote macOS appiconset -> {dest}")


if __name__ == "__main__":
    main()
