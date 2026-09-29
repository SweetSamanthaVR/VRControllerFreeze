#!/usr/bin/env python3
"""Generates assets/icon/VRControllerFreeze.ico, a white snowflake on an icy-blue tile, and
docs/icon.png, the 256 px size shown at the top of the README.

Each size is drawn separately (supersampled, then downscaled) so small sizes get a simpler,
bolder snowflake instead of a blurred copy of the large one. Requires Pillow.
Usage: python tools/make-icon.py
"""
from __future__ import annotations

import io
import math
import pathlib
import struct

from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'assets' / 'icon' / 'VRControllerFreeze.ico'
README_ICON = ROOT / 'docs' / 'icon.png'
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
SUPERSAMPLE = 8
TOP = (30, 64, 175)       # deep blue
BOTTOM = (56, 189, 248)   # icy cyan
FLAKE = (255, 255, 255, 255)


def tile(size: int) -> Image.Image:
    """Rounded square with a vertical gradient."""
    gradient = Image.new('RGBA', (size, size))
    pixels = gradient.load()
    for y in range(size):
        t = y / max(1, size - 1)
        colour = tuple(round(a + (b - a) * t) for a, b in zip(TOP, BOTTOM)) + (255,)
        for x in range(size):
            pixels[x, y] = colour
    mask = Image.new('L', (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=round(size * 0.22), fill=255)
    gradient.putalpha(mask)
    return gradient


def snowflake(draw: ImageDraw.ImageDraw, size: int, final_size: int) -> None:
    centre = size / 2
    arm = size * 0.36
    # Thicker strokes at small sizes keep the shape legible.
    width = max(1, round(size * (0.11 if final_size <= 24 else 0.07 if final_size <= 48 else 0.045)))
    branch_width = max(1, round(width * 0.8))
    branches = final_size >= 32

    def stroke(start, end, stroke_width):
        draw.line((*start, *end), fill=FLAKE, width=stroke_width)
        radius = stroke_width / 2
        for x, y in (start, end):  # round caps
            draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=FLAKE)

    for i in range(6):
        angle = math.radians(90 + i * 60)
        dx, dy = math.cos(angle), -math.sin(angle)
        stroke((centre, centre), (centre + dx * arm, centre + dy * arm), width)
        if branches:
            for along, length in ((0.42, 0.28), (0.70, 0.19)):
                base = (centre + dx * arm * along, centre + dy * arm * along)
                for side in (-1, 1):
                    branch_angle = angle + side * math.radians(50)
                    tip = (base[0] + math.cos(branch_angle) * arm * length, base[1] - math.sin(branch_angle) * arm * length)
                    stroke(base, tip, branch_width)
    hub = width * 0.9
    draw.ellipse((centre - hub, centre - hub, centre + hub, centre + hub), fill=FLAKE)


def render(final_size: int) -> Image.Image:
    size = final_size * SUPERSAMPLE
    image = tile(size)
    snowflake(ImageDraw.Draw(image), size, final_size)
    return image.resize((final_size, final_size), Image.LANCZOS)


def write_ico(images: list[Image.Image], path: pathlib.Path) -> None:
    """Writes an ICO whose entries are PNG-compressed (supported by Windows Vista and later)."""
    blobs = []
    for image in images:
        buffer = io.BytesIO()
        image.save(buffer, format='PNG')
        blobs.append(buffer.getvalue())
    header = struct.pack('<HHH', 0, 1, len(images))
    offset = len(header) + 16 * len(images)
    directory = b''
    for image, blob in zip(images, blobs):
        dimension = image.width if image.width < 256 else 0  # 0 means 256
        directory += struct.pack('<BBBBHHII', dimension, dimension, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + directory + b''.join(blobs))


if __name__ == '__main__':
    images = [render(size) for size in SIZES]
    write_ico(images, OUTPUT)
    images[-1].save(README_ICON, format='PNG')
    print(f'Wrote {OUTPUT.relative_to(ROOT)} ({", ".join(map(str, SIZES))} px) and {README_ICON.relative_to(ROOT)}')
