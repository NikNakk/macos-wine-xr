#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Per-eye PS VR2 lens test images: coloured bands between lens-radius rings.

Each ring is where Monado's PS VR2 visibility mask would cut for that
PSVR2_VISIBILITY_MASK_R2 value (squared lens radius on the panel, margin 0):
psvr2_visibility_outline() in Monado's psvr2_distortion.c, ported here with
the same constants and lookup table (read from that file). Labels give the r2
and the share of the eye's image the mask would hide there. A 10-degree grid
shows view angles. Black is image the panel never shows.

The images cover each eye's render field of view in its image coordinates,
so psvr2-openxr-test --lens-images shows them exactly where a game's frame
would be. Look for the outermost ring you can still see all the way round.

Usage:
  psvr2_lens_rings.py OUT_DIR [--calibration-log SERVICE_LOG | --calibration C0 ... C7]
                      [--size 2800x2856] [--distortion-source PATH]
Writes OUT_DIR/lens-rings-left.png and lens-rings-right.png.
"""

import argparse
import math
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

DEFAULT_SOURCE = os.path.expanduser(
    "~/Code/alyx-native/workspace/monado-quad-gaze/src/xrt/drivers/psvr2/psvr2_distortion.c")
# The driver's fallback offsets with no lens tilt; real headsets report their own.
TYPICAL_CALIBRATION = [-0.09919293, 0.0, 0.09919293, 0.0, 1.0, 0.0, 1.0, 0.0]
# psvr2.c: left eye 61.5 degrees out, 43.5 in, 53 up and down; the right eye mirrors it.
FOV_DEGREES = {0: (-61.5, 43.5, 53.0, -53.0), 1: (-43.5, 61.5, 53.0, -53.0)}
LEVELS = [round(0.50 + 0.05 * i, 2) for i in range(11)]  # 0.50 ... 1.00

FX, FY = 0.3585564, 0.3762281
SX, SY = 0.9803922 * 0.9394987, 0.9394987


def load_lookup(path):
    text = open(path).read()
    body = text[text.index("static const struct LookupItem_t lookup[] = {"):]
    body = body[:body.index("};")]
    values = [float(v) for v in re.findall(r"-?\d+\.\d+", body)]
    table = np.array(values, dtype=np.float64).reshape(-1, 3)
    if table.shape[0] != 1024:
        sys.exit(f"expected 1024 lookup entries in {path}, found {table.shape[0]}")
    return table


def eye_constants(calibration, eye):
    k1, k2 = calibration[0 if eye == 0 else 2], calibration[1 if eye == 0 else 3]
    k3, k4 = calibration[4 if eye == 0 else 6], calibration[5 if eye == 0 else 7]
    cx = 0.6603788 if eye == 0 else 0.3396213
    return k1, k2, k3, k4, cx, 0.5


def distortion(lookup, calibration, eye, u, v):
    """psvr2_compute_distortion_asymmetric: panel UV -> image UV for (r, g, b)."""
    k1, k2, k3, k4, cx, cy = eye_constants(calibration, eye)
    lx = k1 + (2 * u - 1) * SX
    ly = k2 + (2 * v - 1) * SY
    scaled = (lx * lx + ly * ly) * 1023.0
    index = np.floor(scaled).astype(int)
    inside = (index > -1) & (index + 1 < 1024)
    i0 = np.clip(index, 0, 1022)
    frac = (scaled - index)[..., None]
    lut = frac * lookup[i0 + 1] + (1 - frac) * lookup[i0]
    beyond = (scaled - 1023.0)[..., None] * np.array([0.003648639, 0.004862547, 0.007425308]) + \
        np.array([1.592586, 1.692979, 1.841771])
    scale = np.where(inside[..., None], lut, beyond)
    out = []
    for c in range(3):
        y = ly - (0.0002302693 if c == 1 else 0.0)
        a, b = lx * scale[..., c], y * scale[..., c]
        out.append(((k3 * a - k4 * b) * FX + cx, (k4 * a + k3 * b) * FY + cy))
    return out


def outline_radius(lookup, calibration, eye, max_r2, angles):
    """psvr2_visibility_outline with margin 0: image-space radius per direction."""
    k1, k2, k3, k4, cx, cy = eye_constants(calibration, eye)
    norm = k3 * k3 + k4 * k4
    if norm < 1e-6:
        sys.exit("calibration has no lens scale (the driver's fallback); pass a real calibration")
    u0, v0 = 0.5 - k1 / (2 * SX), 0.5 - k2 / (2 * SY)
    dx, dy = np.cos(angles), np.sin(angles)
    ax, ay = dx / FX, dy / FY
    lx, ly = (k3 * ax + k4 * ay) / norm, (-k4 * ax + k3 * ay) / norm
    length = np.hypot(lx, ly)
    lx, ly = lx / length, ly / length
    t = np.full_like(angles, math.sqrt(max_r2) if max_r2 > 0 else np.inf)
    du, dv = lx / (2 * SX), ly / (2 * SY)
    with np.errstate(divide="ignore", invalid="ignore"):
        t = np.minimum(t, np.where(du > 0, (1 - u0) / du, np.where(du < 0, -u0 / du, np.inf)))
        t = np.minimum(t, np.where(dv > 0, (1 - v0) / dv, np.where(dv < 0, -v0 / dv, np.inf)))
    channels = distortion(lookup, calibration, eye, u0 + du * t, v0 + dv * t)
    r = np.zeros_like(angles)
    for x, y in channels:
        r = np.maximum(r, (x - cx) * dx + (y - cy) * dy)
    return r


def font(size):
    for path in ("/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/System/Library/Fonts/Helvetica.ttc",
                 "/Library/Fonts/Arial.ttf"):
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def band_colours(count):
    # Distinct hues from blue (inner) to red (outer), then magenta past r2 1.0.
    hues = [220, 195, 170, 140, 105, 75, 55, 40, 25, 10, 355]
    out = []
    for h in hues[:count]:
        c = Image.new("HSV", (1, 1), (int(h / 360 * 255), 200, 235)).convert("RGB").getpixel((0, 0))
        out.append(c)
    return out


def draw_eye(lookup, calibration, eye, width, height, path, source_note):
    left, right, up, down = [math.radians(a) for a in FOV_DEGREES[eye]]
    tl, tr, tu, td = math.tan(left), math.tan(right), math.tan(up), math.tan(down)
    _, _, _, _, cx, cy = eye_constants(calibration, eye)

    angles = np.linspace(-math.pi, math.pi, 2881)
    levels = LEVELS + [0.0]  # 0: the whole panel
    radii = [outline_radius(lookup, calibration, eye, r2, angles) for r2 in levels]

    ys, xs = np.mgrid[0:height, 0:width]
    u, v = (xs + 0.5) / width, (ys + 0.5) / height
    dxu, dyv = u - cx, v - cy
    theta = np.arctan2(dyv, dxu)
    rho = np.hypot(dxu, dyv)
    ring_r = [np.interp(theta, angles, r) for r in radii]

    # Band 0 is inside the innermost ring; band i is between ring i-1 and ring i.
    band = np.zeros(rho.shape, dtype=np.int32)
    for r in ring_r:
        band += rho > r
    colours = band_colours(len(LEVELS)) + [(205, 60, 205)]
    palette = np.array([(70, 70, 70)] + colours[1:len(LEVELS)] + [colours[-1], (0, 0, 0)], dtype=np.uint8)
    rgb = palette[np.clip(band, 0, len(palette) - 1)]

    # Share of the image each mask setting would hide (pixels are uniform in tangent space).
    hidden = {r2: float((rho > r).mean()) for r2, r in zip(levels, ring_r)}

    # Ring outlines, measured in pixels along each pixel's direction.
    pixel_scale = np.hypot(np.cos(theta) * width, np.sin(theta) * height)
    for i, (r2, r) in enumerate(zip(levels, ring_r)):
        wide = 4.0 if (r2 == 0.0 or abs(r2 * 10 - round(r2 * 10)) < 1e-6) else 2.0
        rgb[np.abs(rho - r) * pixel_scale < wide] = (255, 255, 255)

    image = Image.fromarray(rgb, "RGB")
    draw = ImageDraw.Draw(image, "RGBA")

    # 10-degree grid.
    small = font(max(18, width // 90))
    grid_boxes = []
    for deg in range(-60, 61, 10):
        t = math.tan(math.radians(deg))
        if tl < t < tr:
            x = (t - tl) / (tr - tl) * width
            draw.line([(x, 0), (x, height)], fill=(255, 255, 255, 70 if deg else 160), width=2)
            draw.text((x + 6, cy * height + 8), f"{deg}°", font=small, fill=(255, 255, 255, 220))
            grid_boxes.append(draw.textbbox((x + 6, cy * height + 8), f"{deg}°", font=small))
        if td < t < tu:
            y = (tu - t) / (tu - td) * height
            draw.line([(0, y), (width, y)], fill=(255, 255, 255, 70 if deg else 160), width=2)
            draw.text((cx * width + 8, y + 4), f"{deg}°", font=small, fill=(255, 255, 255, 220))
            grid_boxes.append(draw.textbbox((cx * width + 8, y + 4), f"{deg}°", font=small))

    # Lens centre.
    px, py = cx * width, cy * height
    draw.ellipse([px - 14, py - 14, px + 14, py + 14], outline=(255, 255, 255), width=4)

    # Title in the middle, laid out first so ring labels keep clear of it.
    title_font = font(max(24, width // 70))
    lines = [f"{'LEFT' if eye == 0 else 'RIGHT'} EYE - PS VR2 lens rings",
             "Outermost ring you can see all the way round",
             "= PSVR2_VISIBILITY_MASK_R2.  % = image hidden.",
             "Magenta: past r² 1.00 (panel corners).  Black: never shown.",
             source_note]
    title = []
    ty = py - height * 0.16
    for line in lines:
        box = draw.textbbox((0, 0), line, font=title_font)
        w, h = box[2] - box[0], box[3] - box[1]
        title.append((line, (px - w / 2 - 8, ty - 4, px + w / 2 + 8, ty + h + 10), (px - w / 2, ty)))
        ty += h + 18

    # Ring labels: up to four per ring, spread round it, in the band just inside
    # the ring, wherever they fit without overlapping another label or the title.
    label_font = font(max(22, width // 75))
    placed = list(grid_boxes) + [b for _, b, _ in title]

    def free(box):
        x0, y0, x1, y1 = box
        if x0 < 0 or y0 < 0 or x1 > width or y1 > height:
            return False
        return all(x1 + 6 < a or a2 + 6 < x0 or y1 + 6 < b or b2 + 6 < y0 for a, b, a2, b2 in placed)

    for index, (r2, r) in enumerate(zip(levels, radii)):
        text = "panel edge" if r2 == 0.0 else f"{r2:.2f} · {hidden[r2] * 100:.0f}%"
        box = draw.textbbox((0, 0), text, font=label_font)
        w, h = box[2] - box[0], box[3] - box[1]
        count = 0
        # Each quadrant in turn, trying angles near its middle first; neighbouring rings
        # start at different angles so their labels fan out.
        for quadrant in range(4):
            base = math.radians(45 + 90 * quadrant + (index % 3 - 1) * 12)
            for step in (0, 1, -1, 2, -2, 3, -3, 4, -4):
                a = base + math.radians(7 * step)
                a = math.atan2(math.sin(a), math.cos(a))
                ri = float(np.interp(a, angles, r))
                ri_inner = float(np.interp(a, angles, radii[index - 1])) if index > 0 else 0.0
                mid = (ri + ri_inner) * 0.5 if index > 0 else ri * 0.9
                lx, ly = (cx + math.cos(a) * mid) * width, (cy + math.sin(a) * mid) * height
                cand = (lx - w / 2 - 6, ly - h / 2 - 4, lx + w / 2 + 6, ly + h / 2 + 8)
                if free(cand):
                    placed.append(cand)
                    draw.rectangle(cand, fill=(0, 0, 0, 160))
                    draw.text((lx - w / 2, ly - h / 2), text, font=label_font, fill=(255, 255, 255))
                    count += 1
                    break
            if count >= 4:
                break

    for line, box, origin in title:
        draw.rectangle(box, fill=(0, 0, 0, 170))
        draw.text(origin, line, font=title_font, fill=(255, 255, 255))

    image.save(path)
    return hidden


def calibration_from_log(path):
    pattern = re.compile(r"Lens calibration version (\d+): ((?:-?[\d.eE+-]+\s*){8})")
    found = None
    for line in open(path, errors="replace"):
        m = pattern.search(line)
        if m:
            found = (int(m.group(1)), [float(x) for x in m.group(2).split()])
    if not found:
        sys.exit(f"no 'Lens calibration version' line in {path} (run the service with PSVR2_LOG=info)")
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out_dir")
    parser.add_argument("--calibration-log")
    parser.add_argument("--calibration", nargs=8, type=float)
    parser.add_argument("--size", default="2800x2856")
    parser.add_argument("--distortion-source", default=DEFAULT_SOURCE)
    args = parser.parse_args()

    if args.calibration_log:
        version, calibration = calibration_from_log(args.calibration_log)
        note = f"Calibration v{version} from this headset."
        if version < 4:
            sys.exit("the headset reported an old calibration version; the driver falls back to the default mask")
    elif args.calibration:
        calibration, note = args.calibration, "Calibration given on the command line."
    else:
        calibration, note = TYPICAL_CALIBRATION, "TYPICAL calibration, not this headset's."
    width, height = (int(x) for x in args.size.split("x"))
    lookup = load_lookup(args.distortion_source)
    os.makedirs(args.out_dir, exist_ok=True)
    for eye, name in ((0, "left"), (1, "right")):
        hidden = draw_eye(lookup, calibration, eye, width, height,
                          os.path.join(args.out_dir, f"lens-rings-{name}.png"), note)
        summary = ", ".join(f"{'panel' if k == 0.0 else f'{k:.2f}'}: {v * 100:.1f}%" for k, v in hidden.items())
        print(f"{name}: hidden share by r2: {summary}")


if __name__ == "__main__":
    main()
