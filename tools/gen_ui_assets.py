#!/usr/bin/env python3
"""Procedural hardware faceplate & control sprite generator for Tributary.

Aesthetic (Stampman3000 family — OP3000 / CZboy / Voxman / Twenty):
  - Matte obsidian-black anodized chassis with fine satin grain & machined screws
  - Understated studio plum-purple & calm emerald-sage accents (low-glare, studio-grade)
  - Recessed gunmetal-bezel OLED monitor windows for live routing & capture telemetry
  - Tactile chamfered hardware keycaps with crisp horizontal LED slits (purple tracks,
    emerald FX/IN & ACTIVE, soft violet MAIN, restrained crimson ARM)
  - Knurled matte-black rotary Level pot with calibrated 0-100 arc & 32-frame segmented meter

Run:
    python3 tools/gen_ui_assets.py
"""
import math
import os
import random
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART_DIR = os.path.join(ROOT, "vst", "art")

W, H = 1280, 628
SS = 2  # 2x supersampling for crisp edges and typography

# Core hardware palette (PANEL_BG matches theme_bg=141519 in vst/layout.conf)
CHASSIS_BG  = (13, 14, 17, 255)
PANEL_BG    = (20, 21, 25, 255)    # #141519 — exact match to theme_bg for seamless sprites
PANEL_RULE  = (46, 48, 58, 255)
PANEL_SUB   = (34, 36, 44, 255)
HEADER_BG   = (10, 11, 14, 255)

OLED_BEZEL  = (54, 56, 66, 255)
OLED_BEZ_HI = (86, 90, 104, 255)
OLED_BG     = (8, 11, 11, 255)

WHITE       = (232, 236, 234, 255)
OFF_WHITE   = (184, 188, 196, 255)
MUTED       = (116, 120, 132, 255)
DIM         = (74, 78, 88, 255)
DARK_INK    = (14, 15, 18, 255)

# Understated studio purple & calm emerald-sage (no orange, no light blue)
PURPLE_BAR  = (90, 62, 122, 255)     # deep matte plum-violet header
PURPLE      = (136, 92, 192, 255)    # lit track LED purple
PURPLE_HI   = (196, 166, 238, 255)   # soft lavender LED core
PURPLE_SOFT = (164, 134, 204, 255)   # muted violet accent text

GREEN_BAR   = (38, 94, 72, 255)      # deep matte forest-emerald header
GREEN       = (52, 176, 114, 255)    # calm emerald LED
GREEN_HI    = (148, 234, 186, 255)   # soft mint LED core
GREEN_SOFT  = (112, 194, 150, 255)   # muted sage accent text

# Functional status accents (lamp amber / capture arm crimson)
AMBER       = (204, 148, 44, 255)
RED         = (188, 56, 64, 255)
RED_HI      = (236, 116, 124, 255)


def load_font(px, bold=True):
    candidates = (
        [
            "/System/Library/Fonts/HelveticaNeue.ttc",
            "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        ]
        if bold else
        [
            "/System/Library/Fonts/HelveticaNeue.ttc",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        ]
    )
    for path in candidates:
        if os.path.exists(path):
            try:
                idx = 1 if (path.endswith(".ttc") and bold) else 0
                return ImageFont.truetype(path, int(px), index=idx)
            except Exception:
                try:
                    return ImageFont.truetype(path, int(px))
                except Exception:
                    pass
    return ImageFont.load_default()


def draw_text(img, text, x, y, size_px, fill=WHITE, align="center", bold=True, track=0.05):
    font = load_font(size_px * SS, bold=bold)
    tmp = Image.new("RGBA", (1, 1), (0, 0, 0, 0))
    tdr = ImageDraw.Draw(tmp)
    chars = list(text)
    widths = []
    for ch in chars:
        bb = tdr.textbbox((0, 0), ch, font=font)
        widths.append(max(bb[2] - bb[0], int(size_px * SS * 0.28) if ch == " " else 1))
    pad = int(round(size_px * SS * track))
    total = sum(widths) + pad * max(0, len(chars) - 1)
    cx = x * SS - (total / 2.0 if align == "center" else total if align == "right" else 0.0)
    cy = y * SS - (size_px * SS) * 0.56
    dr = ImageDraw.Draw(img)
    for ch, cw in zip(chars, widths):
        dr.text((cx, cy), ch, font=font, fill=fill)
        cx += cw + pad


def make_chassis():
    """Matte obsidian-black anodized chassis with fine satin grain and machined screws."""
    img = Image.new("RGBA", (W * SS, H * SS), CHASSIS_BG)
    dr = ImageDraw.Draw(img)

    for y in range(H * SS):
        t = y / float(H * SS - 1)
        d = int(round(3 * (1.0 - t) - 2 * t))
        c = (max(0, CHASSIS_BG[0] + d), max(0, CHASSIS_BG[1] + d), max(0, CHASSIS_BG[2] + d), 255)
        dr.line([(0, y), (W * SS, y)], fill=c)

    # Fine brushed horizontal satin texture
    rng = random.Random(3000)
    grain = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    gdr = ImageDraw.Draw(grain)
    for y in range(0, H * SS, 2):
        a = rng.randint(0, 6)
        val = 255 if rng.random() > 0.5 else 0
        gdr.line([(0, y), (W * SS, y)], fill=(val, val, val, a))
    img = Image.alpha_composite(img, grain)
    dr = ImageDraw.Draw(img)

    # Top header band
    dr.rectangle([0, 0, W * SS, 105 * SS], fill=HEADER_BG)

    # Outer subtle bevel
    dr.rectangle([0, 0, W * SS - 1, H * SS - 1], outline=(38, 40, 48, 255), width=2)
    dr.rectangle([2, 2, W * SS - 3, H * SS - 3], outline=(7, 8, 10, 255), width=2)

    # Restrained twin accent rule below header (muted purple + calm green)
    dr.rectangle([0, 105 * SS, W * SS, 107 * SS], fill=PURPLE_BAR)
    dr.rectangle([0, 109 * SS, W * SS, 110 * SS], fill=GREEN_BAR)

    # Corner machined dark-steel screws
    for sx, sy in [(11, 11), (W - 11, 11), (11, H - 11), (W - 11, H - 11)]:
        r = 5 * SS
        cx, cy = sx * SS, sy * SS
        dr.ellipse([cx - r - 2, cy - r - 2, cx + r + 2, cy + r + 2], fill=(5, 6, 8, 255))
        dr.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(34, 36, 44, 255), outline=(58, 62, 72, 255), width=1)
        dr.line([cx - r + 4, cy - 2, cx + r - 4, cy + 2], fill=(12, 14, 18, 255), width=2)

    return img


def draw_module_panel(img, x, y, w, h, title, accent, right_tag=None, header_ink=WHITE):
    """Hardware module box with understated colored header bar and exact PANEL_BG fill."""
    dr = ImageDraw.Draw(img)
    x0, y0, x1, y1 = x * SS, y * SS, (x + w) * SS, (y + h) * SS
    rad = 8 * SS
    hh = 30 * SS

    dr.rounded_rectangle([x0 + 2, y0 + 3, x1 + 2, y1 + 3], radius=rad, fill=(4, 5, 7, 150))
    dr.rounded_rectangle([x0, y0, x1, y1], radius=rad, fill=PANEL_BG, outline=PANEL_RULE, width=2)

    hdr = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    hdr_dr = ImageDraw.Draw(hdr)
    hdr_dr.rounded_rectangle([x0 + 2, y0 + 2, x1 - 2, y0 + hh + rad], radius=rad - 2, fill=accent)
    hdr_dr.rectangle([x0 + 2, y0 + hh, x1 - 2, y0 + hh + rad + 2], fill=(0, 0, 0, 0))
    hdr_dr.line([x0 + rad, y0 + 3, x1 - rad, y0 + 3], fill=(255, 255, 255, 36), width=2)

    # Diagonal louver stripes on right edge of header
    stripe_x = x1 - 36 * SS
    for i in range(3):
        sx = stripe_x + i * 9 * SS
        pts = [(sx, y0 + hh), (sx + 6 * SS, y0 + 2), (sx + 10 * SS, y0 + 2), (sx + 4 * SS, y0 + hh)]
        hdr_dr.polygon(pts, fill=(14, 15, 19, 175))
    img.alpha_composite(hdr)

    dr = ImageDraw.Draw(img)
    dr.line([x0 + 2, y0 + hh, x1 - 2, y0 + hh], fill=(8, 9, 12, 220), width=2)
    dr.rounded_rectangle([x0, y0, x1, y1], radius=rad, outline=PANEL_RULE, width=2)

    draw_text(img, title, x + 14, y + 15, 13, fill=header_ink, align="left", bold=True, track=0.10)
    if right_tag:
        draw_text(img, right_tag, x + w - 48, y + 15, 10, fill=(230, 236, 234, 195), align="right", bold=True, track=0.08)


def draw_oled_window(img, x, y, w, h, label=None):
    """Recessed dark-bezel OLED monitor glass."""
    dr = ImageDraw.Draw(img)
    x0, y0, x1, y1 = x * SS, y * SS, (x + w) * SS, (y + h) * SS
    dr.rounded_rectangle([x0, y0, x1, y1], radius=6 * SS, fill=(24, 26, 32, 255), outline=OLED_BEZEL, width=2)
    dr.line([x0 + 6 * SS, y0 + 2, x1 - 6 * SS, y0 + 2], fill=OLED_BEZ_HI, width=1)
    pad = 4 * SS
    dr.rounded_rectangle([x0 + pad, y0 + pad, x1 - pad, y1 - pad], radius=4 * SS, fill=OLED_BG, outline=(4, 5, 6, 255), width=2)
    glare = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    gdr = ImageDraw.Draw(glare)
    gdr.rounded_rectangle([x0 + pad + 2, y0 + pad + 2, x1 - pad - 2, y0 + pad + (h * SS) // 3],
                          radius=3 * SS, fill=(160, 210, 185, 7))
    img.alpha_composite(glare)
    if label:
        draw_text(img, label, x + 14, y + 14, 9.5, fill=(116, 152, 136, 210), align="left", bold=True, track=0.12)


def draw_pill_badge(img, x, y, w, h, text, fill_col, ink_col=WHITE):
    dr = ImageDraw.Draw(img)
    dr.rounded_rectangle([x * SS, y * SS, (x + w) * SS, (y + h) * SS], radius=4 * SS, fill=fill_col)
    draw_text(img, text, x + w / 2.0, y + h / 2.0 + 0.5, 10, fill=ink_col, align="center", bold=True, track=0.09)


def draw_brand_header(img, badge_text, badge_col, subtitle):
    draw_text(img, "TRIBUTARY", 28, 36, 26, fill=WHITE, align="left", bold=True, track=0.09)
    draw_pill_badge(img, 214, 24, 114, 22, badge_text, badge_col, WHITE)
    dr = ImageDraw.Draw(img)
    dr.rectangle([28 * SS, 56 * SS, 328 * SS, 58 * SS], fill=PURPLE_BAR)
    dr.rectangle([232 * SS, 56 * SS, 328 * SS, 58 * SS], fill=GREEN_BAR)
    draw_text(img, subtitle, 28, 76, 10.5, fill=MUTED, align="left", bold=True, track=0.08)


def generate_bg_tributary():
    img = make_chassis()
    draw_brand_header(img, "OVERBRIDGE", PURPLE_BAR, "USB AUDIO BRIDGE  •  MPC IS THE CLOCK")

    # Center OLED routing monitor
    draw_oled_window(img, 356, 14, 650, 76, label="ACTIVE MIX ROUTING")

    # Right USB LINK status pod
    draw_oled_window(img, 1022, 14, 234, 76)
    draw_text(img, "USB LINK", 1040, 36, 11.5, fill=OFF_WHITE, align="left", bold=True, track=0.11)
    dr = ImageDraw.Draw(img)
    for dx, col, lbl in [(1042, RED, "OFF"), (1080, AMBER, "SYNC"), (1126, GREEN, "LIVE")]:
        dr.ellipse([(dx - 3) * SS, (56 - 3) * SS, (dx + 3) * SS, (56 + 3) * SS], fill=col)
        draw_text(img, lbl, dx + 7, 56, 8.5, fill=MUTED, align="left", bold=True, track=0.05)

    # Left module: STREAM & OUTPUT (deep plum-purple header)
    draw_module_panel(img, 24, 122, 280, 486, "STREAM & OUTPUT", PURPLE_BAR)
    draw_text(img, "SESSION & BUS", 164, 174, 10, fill=MUTED, align="center", bold=True, track=0.12)
    dr = ImageDraw.Draw(img)
    dr.line([42 * SS, 304 * SS, 286 * SS, 304 * SS], fill=PANEL_SUB, width=2)

    draw_text(img, "TRACK LEVEL", 112, 328, 10, fill=MUTED, align="center", bold=True, track=0.10)
    draw_text(img, "PEAK", 232, 328, 10, fill=MUTED, align="center", bold=True, track=0.10)

    # Calibrated 0 / 50 / 100 markings around Level knob (cx=112, local_cy=434, r=44)
    kcx, kcy = 112, 434
    for deg, rad_r, lbl in [(-124, 60, "0"), (0, 58, "50"), (124, 61, "100")]:
        rad = math.radians(deg - 90)
        tx = kcx + math.cos(rad) * rad_r
        ty = kcy + math.sin(rad) * rad_r
        draw_text(img, lbl, tx, ty, 9.5, fill=MUTED, align="center", bold=True, track=0.02)

    # Recessed bezel around vertical meter (cx=232, local_cy=454, w=38, h=180)
    mx0, my0, mx1, my1 = (232 - 22) * SS, (454 - 93) * SS, (232 + 22) * SS, (454 + 93) * SS
    dr.rounded_rectangle([mx0, my0, mx1, my1], radius=5 * SS, fill=(8, 10, 11, 255), outline=(46, 50, 58, 255), width=2)
    for frac, db in [(0.05, "0"), (0.28, "-6"), (0.55, "-12"), (0.86, "-24")]:
        ly = 454 - 90 + frac * 180
        dr.line([(232 + 25) * SS, int(ly * SS), (232 + 29) * SS, int(ly * SS)], fill=DIM, width=2)
        draw_text(img, db, 266, ly, 8.5, fill=MUTED, align="left", bold=True, track=0.02)

    # Right module: CHANNEL MATRIX (deep emerald-green header)
    draw_module_panel(img, 320, 122, 936, 486, "CHANNEL MATRIX", GREEN_BAR, right_tag="SUM SELECTED OUTPUTS")

    # Row dividers and bank labels
    for ry, tag, tag_col in [
        (174, "TRACKS 1 – 8", PURPLE_SOFT),
        (312, "TRACKS 9 – 16", PURPLE_SOFT),
        (450, "FX RETURNS & EXTERNAL INPUT", GREEN_SOFT),
    ]:
        draw_text(img, tag, 344, ry, 10, fill=tag_col, align="left", bold=True, track=0.12)
        rule_x0 = 452 if "1 – 8" in tag else 460 if "9 – 16" in tag else 562
        dr.line([rule_x0 * SS, ry * SS, 1232 * SS, ry * SS], fill=PANEL_SUB, width=2)

    # Subtle spec badge in the open right half of Row 3
    bx0, by0, bx1, by1 = 804 * SS, 482 * SS, 1230 * SS, 578 * SS
    dr.rounded_rectangle([bx0, by0, bx1, by1], radius=6 * SS, fill=(13, 14, 18, 255), outline=(38, 42, 50, 255), width=2)
    dr.rectangle([bx0 + 2, by0 + 10 * SS, bx0 + 5 * SS, by1 - 10 * SS], fill=PURPLE_BAR)
    draw_text(img, "OVERBRIDGE 2  •  INTERRUPT AUDIO", 822, 504, 10.5, fill=OFF_WHITE, align="left", bold=True, track=0.10)
    draw_text(img, "48 KHZ USB IN  /  44.1 KHZ MPC TRACK  •  11 MS BUFFER", 822, 528, 9.5, fill=MUTED, align="left", bold=False, track=0.05)
    draw_text(img, "CLOCK MASTER: MPC  •  MIDI INTERFACE UNTOUCHED", 822, 552, 9.5, fill=GREEN_SOFT, align="left", bold=True, track=0.06)

    out = img.resize((W, H), Image.Resampling.LANCZOS).convert("RGB")
    out.save(os.path.join(ART_DIR, "bg_tributary.png"), optimize=True)


def generate_bg_capture():
    img = make_chassis()
    draw_brand_header(img, "CAPTURE", GREEN_BAR, "BAR-LOCKED 48 KHZ 24-BIT WAV RECORDER")

    draw_oled_window(img, 740, 14, 516, 76)
    draw_text(img, "DIRECT 48 KHZ CAPTURE PATH", 758, 36, 10.5, fill=GREEN_SOFT, align="left", bold=True, track=0.10)
    draw_text(img, "RECORDS MACHINE FRAMES FOR ACTIVE SWITCHES  •  NOT RESAMPLED", 758, 60, 9.5, fill=MUTED, align="left", bold=True, track=0.05)

    draw_module_panel(img, 24, 122, 1232, 174, "RECORDER TELEMETRY", GREEN_BAR, right_tag="LIVE CLOCK LOCK")
    draw_oled_window(img, 46, 170, 404, 104, label="STATUS")
    draw_oled_window(img, 468, 170, 766, 104, label="DESTINATION FOLDER")

    draw_module_panel(img, 24, 312, 670, 296, "TAKE SETTINGS", GREEN_BAR, right_tag="BAR LENGTH & TARGET")
    draw_module_panel(img, 710, 312, 546, 296, "RECORD CONTROL", PURPLE_BAR, right_tag="ARM ON NEXT BAR")
    dr = ImageDraw.Draw(img)
    dr.line([732 * SS, 502 * SS, 1234 * SS, 502 * SS], fill=PANEL_SUB, width=2)

    out = img.resize((W, H), Image.Resampling.LANCZOS).convert("RGB")
    out.save(os.path.join(ART_DIR, "bg_capture.png"), optimize=True)


def generate_lamps():
    """Jeweled hardware LEDs with machined dark bezel (56x56)."""
    sz = 56
    specs = [
        ("lamp0.png", (62, 18, 22), (148, 38, 44), (214, 86, 92), False),    # off/red
        ("lamp1.png", (102, 66, 16), (196, 134, 34), (240, 204, 114), True), # amber
        ("lamp2.png", (16, 78, 44), (44, 178, 108), (156, 240, 192), True),  # emerald green
    ]
    for fname, dark, mid, hi, glow in specs:
        im = Image.new("RGBA", (sz * SS, sz * SS), (0, 0, 0, 0))
        c = sz * SS // 2
        if glow:
            g = Image.new("RGBA", (sz * SS, sz * SS), (0, 0, 0, 0))
            gdr = ImageDraw.Draw(g)
            gr = 23 * SS
            gdr.ellipse([c - gr, c - gr, c + gr, c + gr], fill=(mid[0], mid[1], mid[2], 105))
            g = g.filter(ImageFilter.GaussianBlur(5 * SS))
            im.alpha_composite(g)

        dr = ImageDraw.Draw(im)
        r_out = 21 * SS
        dr.ellipse([c - r_out, c - r_out, c + r_out, c + r_out], fill=(20, 22, 28, 255), outline=(64, 68, 78, 255), width=2)
        r_mid = 17 * SS
        dr.ellipse([c - r_mid, c - r_mid, c + r_mid, c + r_mid], fill=(10, 12, 15, 255), outline=(38, 42, 50, 255), width=2)

        r_led = 14 * SS
        for step in range(r_led, 0, -1):
            t = 1.0 - step / float(r_led)
            if t < 0.6:
                u = t / 0.6
                col = tuple(int(dark[i] + (mid[i] - dark[i]) * u) for i in range(3)) + (255,)
            else:
                u = (t - 0.6) / 0.4
                col = tuple(int(mid[i] + (hi[i] - mid[i]) * u) for i in range(3)) + (255,)
            dr.ellipse([c - step, c - step, c + step, c + step], fill=col)

        dr.ellipse([c - 7 * SS, c - 8 * SS, c - 1 * SS, c - 3 * SS], fill=(255, 255, 255, 180 if glow else 110))
        im.resize((sz, sz), Image.Resampling.LANCZOS).save(os.path.join(ART_DIR, fname), optimize=True)


def generate_knob():
    """Matte-black knurled rotary pot with soft emerald indicator needle (98x98)."""
    sz = 98
    c = sz * SS // 2

    base = Image.new("RGBA", (sz * SS, sz * SS), (0, 0, 0, 0))
    dr = ImageDraw.Draw(base)
    for i in range(11):
        deg = -135 + i * 27
        rad = math.radians(deg - 90)
        major = i in (0, 5, 10)
        r0 = (41 if major else 42) * SS
        r1 = (47 if major else 46) * SS
        col = PURPLE_SOFT if i == 10 else (GREEN_SOFT if i == 5 else (OFF_WHITE if major else DIM))
        dr.line([c + math.cos(rad) * r0, c + math.sin(rad) * r0,
                 c + math.cos(rad) * r1, c + math.sin(rad) * r1],
                fill=col, width=3 if major else 2)
    sh = Image.new("RGBA", (sz * SS, sz * SS), (0, 0, 0, 0))
    sdr = ImageDraw.Draw(sh)
    sdr.ellipse([c - 36 * SS, c - 34 * SS, c + 36 * SS, c + 38 * SS], fill=(0, 0, 0, 175))
    sh = sh.filter(ImageFilter.GaussianBlur(3 * SS))
    base.alpha_composite(sh)
    base.resize((sz, sz), Image.Resampling.LANCZOS).save(os.path.join(ART_DIR, "knob_base.png"), optimize=True)

    cap = Image.new("RGBA", (sz * SS, sz * SS), (0, 0, 0, 0))
    dr = ImageDraw.Draw(cap)
    r_skirt = 35 * SS
    dr.ellipse([c - r_skirt, c - r_skirt, c + r_skirt, c + r_skirt],
               fill=(22, 24, 29, 255), outline=(60, 64, 76, 255), width=2)
    for k in range(24):
        ang = math.radians(k * 15)
        dr.line([c + math.cos(ang) * 30 * SS, c + math.sin(ang) * 30 * SS,
                 c + math.cos(ang) * 34 * SS, c + math.sin(ang) * 34 * SS],
                fill=(42, 46, 56, 255), width=2)
    r_top = 27 * SS
    dr.ellipse([c - r_top, c - r_top, c + r_top, c + r_top],
               fill=(32, 34, 42, 255), outline=(74, 78, 92, 255), width=2)
    r_in = 22 * SS
    dr.ellipse([c - r_in, c - r_in, c + r_in, c + r_in],
               fill=(24, 26, 32, 255), outline=(14, 15, 19, 255), width=2)
    # Soft emerald-sage indicator needle pointing straight UP
    dr.rounded_rectangle([c - 2.5 * SS, c - 31 * SS, c + 2.5 * SS, c - 8 * SS], radius=2 * SS, fill=GREEN_SOFT)
    cap.resize((sz, sz), Image.Resampling.LANCZOS).save(os.path.join(ART_DIR, "knob_cap.png"), optimize=True)


def generate_toggle_pair(name, w, h, led_on, led_hi, led_dim=(44, 46, 56)):
    """Tactile matte-black rectangular keycap with restrained glowing horizontal LED slit."""
    for state, is_on in [("off", False), ("on", True)]:
        im = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
        dr = ImageDraw.Draw(im)
        W2, H2 = w * SS, h * SS

        dr.rounded_rectangle([2, 2, W2 - 3, H2 - 3], radius=6 * SS, fill=(6, 7, 9, 255))

        inset = 3 * SS
        body_fill = (36, 38, 46, 255) if is_on else (27, 29, 35, 255)
        body_edge = (82, 86, 100, 255) if is_on else (50, 54, 64, 255)
        dr.rounded_rectangle([inset, inset, W2 - inset, H2 - inset],
                             radius=5 * SS, fill=body_fill, outline=body_edge, width=2)

        dr.line([inset + 5 * SS, inset + 2, W2 - inset - 5 * SS, inset + 2],
                fill=(255, 255, 255, 34 if is_on else 18), width=2)
        dr.line([inset + 5 * SS, H2 - inset - 2, W2 - inset - 5 * SS, H2 - inset - 2],
                fill=(0, 0, 0, 125), width=2)

        lw = int(W2 * 0.56)
        lh = max(5 * SS, int(H2 * 0.16))
        lx0 = (W2 - lw) // 2
        ly0 = int(H2 * 0.28)
        lx1, ly1 = lx0 + lw, ly0 + lh

        if is_on:
            glow = Image.new("RGBA", (W2, H2), (0, 0, 0, 0))
            gdr = ImageDraw.Draw(glow)
            gdr.rounded_rectangle([lx0 - 5 * SS, ly0 - 4 * SS, lx1 + 5 * SS, ly1 + 4 * SS],
                                  radius=4 * SS, fill=(led_on[0], led_on[1], led_on[2], 115))
            glow = glow.filter(ImageFilter.GaussianBlur(4 * SS))
            im.alpha_composite(glow)
            dr = ImageDraw.Draw(im)
            dr.rounded_rectangle([lx0, ly0, lx1, ly1], radius=3 * SS, fill=led_on)
            dr.rounded_rectangle([lx0 + 3, ly0 + 2, lx1 - 3, ly1 - 3], radius=2 * SS, fill=led_hi)
            dr.line([inset + 6 * SS, H2 - inset - 3, W2 - inset - 6 * SS, H2 - inset - 3],
                    fill=(led_on[0], led_on[1], led_on[2], 125), width=2)
        else:
            dr.rounded_rectangle([lx0, ly0, lx1, ly1], radius=3 * SS,
                                 fill=(15, 16, 20, 255), outline=(led_dim[0], led_dim[1], led_dim[2], 175), width=1)

        im.resize((w, h), Image.Resampling.LANCZOS).save(os.path.join(ART_DIR, f"{name}_{state}.png"), optimize=True)


def generate_cancel_button(w=210, h=44):
    """Restrained dark hardware button for Capture CANCEL."""
    for state, pressed in [("off", False), ("on", True)]:
        im = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
        dr = ImageDraw.Draw(im)
        W2, H2 = w * SS, h * SS
        fill = (128, 42, 52, 255) if pressed else (34, 24, 30, 255)
        edge = RED_HI if pressed else (112, 58, 72, 255)
        dr.rounded_rectangle([2, 2, W2 - 3, H2 - 3], radius=6 * SS, fill=(6, 7, 9, 255))
        dr.rounded_rectangle([4, 4, W2 - 5, H2 - 5], radius=5 * SS, fill=fill, outline=edge, width=2)
        dr.line([12, 6, W2 - 13, 6], fill=(255, 255, 255, 40 if pressed else 22), width=2)
        im.resize((w, h), Image.Resampling.LANCZOS).save(os.path.join(ART_DIR, f"btn_cancel_{state}.png"), optimize=True)


def generate_meter_strip(w=38, h=180, frames=32, segs=16):
    """32-frame segmented meter: calm emerald green -> muted violet -> crimson clip."""
    strip = Image.new("RGB", (w, h * frames), (8, 10, 11))
    pad_x = 4
    pad_y = 5
    gap = 3
    usable_h = h - 2 * pad_y
    seg_h = (usable_h - gap * (segs - 1)) / float(segs)

    for f in range(frames):
        lit = int(round((f / float(frames - 1)) * segs))
        frame = Image.new("RGBA", (w * SS, h * SS), (8, 10, 11, 255))
        dr = ImageDraw.Draw(frame)

        for s in range(segs):
            y1 = h - pad_y - s * (seg_h + gap)
            y0 = y1 - seg_h
            if s >= segs - 2:
                on_col, hi_col, off_col = RED, RED_HI, (38, 18, 22, 255)
            elif s >= segs - 5:
                on_col, hi_col, off_col = PURPLE, PURPLE_HI, (32, 22, 44, 255)
            else:
                on_col, hi_col, off_col = GREEN, GREEN_HI, (16, 32, 24, 255)

            box = [int(pad_x * SS), int(y0 * SS), int((w - pad_x) * SS), int(y1 * SS)]
            if s < lit:
                dr.rounded_rectangle(box, radius=2 * SS, fill=on_col)
                inner = [box[0] + 2, box[1] + 2, box[2] - 2, max(box[1] + 3, box[3] - 4)]
                dr.rounded_rectangle(inner, radius=SS, fill=hi_col)
            else:
                dr.rounded_rectangle(box, radius=2 * SS, fill=off_col)

        small = frame.resize((w, h), Image.Resampling.LANCZOS).convert("RGB")
        strip.paste(small, (0, f * h))

    strip.save(os.path.join(ART_DIR, "level.png"), optimize=True)


def main():
    os.makedirs(ART_DIR, exist_ok=True)
    generate_bg_tributary()
    generate_bg_capture()
    generate_lamps()
    generate_knob()
    generate_toggle_pair("tog_active", 96, 44, GREEN, GREEN_HI, (28, 62, 44))
    generate_toggle_pair("tog_main",   96, 44, PURPLE_SOFT, PURPLE_HI, (56, 44, 74))
    generate_toggle_pair("tog_trk",    84, 42, PURPLE, PURPLE_HI, (52, 38, 72))
    generate_toggle_pair("tog_fx",     84, 42, GREEN, GREEN_HI, (26, 58, 42))
    generate_toggle_pair("tog_arm",   112, 48, RED, RED_HI, (68, 28, 34))
    generate_toggle_pair("tog_rearm", 112, 48, GREEN, GREEN_HI, (26, 58, 42))
    generate_cancel_button(210, 44)
    generate_meter_strip(38, 180, frames=32, segs=16)
    print("generated Tributary UI assets in", ART_DIR)


if __name__ == "__main__":
    main()
