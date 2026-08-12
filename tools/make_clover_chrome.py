#!/usr/bin/env python3
"""Build home_clover_chrome.png from home_clover_round.png.

Removes baked white app icons while keeping the round asset's petal gradients.
Uses inpainting (not flat color fill) so slots do not turn into dark disks.
"""
from __future__ import annotations

import os

import numpy as np
from PIL import Image, ImageFilter

ROOT = os.path.join(os.path.dirname(__file__), "..", "main", "xingzhi-assets")
SIZE = 360

PETAL_SLOTS = [
    (180, 60, 52),
    (298, 178, 52),
    (180, 282, 52),
    (62, 178, 52),
]
CENTER_WAKE = (180, 178, 34)


def smoothstep(t: np.ndarray) -> np.ndarray:
    t = np.clip(t, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def stroke_mask(src: np.ndarray, cx: int, cy: int, radius: float,
                lum_min: float, chroma_max: float) -> np.ndarray:
    yy, xx = np.ogrid[:SIZE, :SIZE]
    dist = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
    region = dist <= radius
    rgb = src[..., :3]
    lum = 0.299 * rgb[..., 0] + 0.587 * rgb[..., 1] + 0.114 * rgb[..., 2]
    chroma = np.max(rgb, axis=2) - np.min(rgb, axis=2)
    return region & (lum > lum_min) & (chroma < chroma_max) & (src[..., 3] > 40.0)


def inpaint_mask(px: np.ndarray, mask: np.ndarray, passes: int = 80) -> None:
    work = mask.copy()
    for _ in range(passes):
        if not work.any():
            return
        new_px = px.copy()
        ys, xs = np.where(work)
        filled: list[tuple[int, int]] = []
        for y, x in zip(ys, xs):
            neighbors = []
            for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                ny, nx = y + dy, x + dx
                if 0 <= ny < SIZE and 0 <= nx < SIZE and not work[ny, nx]:
                    neighbors.append(px[ny, nx, :3])
            if neighbors:
                color = np.mean(neighbors, axis=0)
                new_px[y, x, 0] = color[0]
                new_px[y, x, 1] = color[1]
                new_px[y, x, 2] = color[2]
                new_px[y, x, 3] = 255.0
                filled.append((y, x))
        px[:] = new_px
        for y, x in filled:
            work[y, x] = False


def soften_hotspot(px: np.ndarray, cx: int, cy: int, radius: float,
                     strength: float = 0.38) -> None:
    """Mildly flatten the bright icon halo; strength must stay low to avoid blobs."""
    pad = int(radius) + 16
    x0 = max(0, cx - pad)
    y0 = max(0, cy - pad)
    x1 = min(SIZE, cx + pad)
    y1 = min(SIZE, cy + pad)
    if x1 <= x0 or y1 <= y0:
        return

    patch = px[y0:y1, x0:x1, :3].astype(np.float32)
    blurred = np.array(
        Image.fromarray(patch.astype(np.uint8)).filter(
            ImageFilter.GaussianBlur(radius=12)),
        dtype=np.float32,
    )

    hh, ww = patch.shape[:2]
    yy, xx = np.ogrid[:hh, :ww]
    dist = np.sqrt((xx + x0 - cx) ** 2 + (yy + y0 - cy) ** 2)
    blend = smoothstep(1.0 - dist / radius) * strength
    blend = blend[..., None]
    px[y0:y1, x0:x1, :3] = patch * (1.0 - blend) + blurred * blend


def clean_slot(px: np.ndarray, original: np.ndarray, cx: int, cy: int,
               radius: float) -> None:
    for lum_min, chroma_max in ((168.0, 55.0), (150.0, 65.0)):
        inpaint_mask(px, stroke_mask(original, cx, cy, radius, lum_min, chroma_max))
    soften_hotspot(px, cx, cy, radius - 2.0)


def main() -> None:
    src = os.path.join(ROOT, "home_clover_round.png")
    dst = os.path.join(ROOT, "home_clover_chrome.png")
    original = np.array(Image.open(src).convert("RGBA"), dtype=np.float32)
    px = original.copy()

    for cx, cy, radius in PETAL_SLOTS:
        clean_slot(px, original, cx, cy, radius)

    cx, cy, radius = CENTER_WAKE
    inpaint_mask(px, stroke_mask(original, cx, cy, radius, 168.0, 55.0))

    out = Image.fromarray(np.clip(px, 0, 255).astype(np.uint8), mode="RGBA")
    out.save(dst)
    print("wrote", os.path.abspath(dst))


if __name__ == "__main__":
    main()
