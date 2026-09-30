#!/usr/bin/env python3
# PS5 RetroArch - make the launcher backgrounds (sce_sys/pic0.dds, pic1.dds).
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The backgrounds are drawn here, from nothing but arithmetic, so their origin
# is this script: a dark gradient with a soft glow, 3840x2160, in the format the
# launcher reads (DDS, DX10 header, BC7_UNORM, one level).
#
# Each 4x4 block is one colour, which BC7's mode 6 holds exactly (both
# endpoints that colour, every index 0), so no texture compressor is needed
# and the output is the same on every machine. At 4K a 4x4 step is invisible
# in a gradient this gentle.
#
#   python3 tools/make-title-art.py            write sce_sys/pic0.dds and pic1.dds
#   python3 tools/make-title-art.py --png DIR  also write PNG previews to DIR
import struct
import sys
from pathlib import Path

import numpy as np

WIDTH, HEIGHT = 3840, 2160
ROOT = Path(__file__).resolve().parent.parent


def draw(variant: int) -> np.ndarray:
    """The picture, one RGB colour per 4x4 block (HEIGHT/4 x WIDTH/4 x 3, 0..1)."""
    by, bx = HEIGHT // 4, WIDTH // 4
    y, x = np.mgrid[0:by, 0:bx].astype(np.float64)
    u, v = x / (bx - 1), y / (by - 1)
    # A diagonal from deep navy to a dark violet
    top = np.array([0.035, 0.045, 0.110])
    bottom = np.array([0.095, 0.040, 0.140])
    t = np.clip(0.55 * u + 0.45 * v, 0.0, 1.0)[..., None]
    rgb = top * (1 - t) + bottom * t
    # A soft glow, placed differently in the two pictures
    cx, cy = (0.30, 0.38) if variant == 0 else (0.70, 0.62)
    d2 = ((u - cx) * 16 / 9) ** 2 + (v - cy) ** 2
    glow = np.exp(-d2 / 0.09)[..., None]
    rgb = rgb + glow * np.array([0.10, 0.14, 0.26])
    # A second, fainter glow in a warmer tone
    d2b = ((u - (1 - cx)) * 16 / 9) ** 2 + (v - (1 - cy)) ** 2
    rgb = rgb + np.exp(-d2b / 0.05)[..., None] * np.array([0.09, 0.04, 0.10])
    # A gentle vignette
    r2 = ((u - 0.5) * 16 / 9) ** 2 + (v - 0.5) ** 2
    rgb = rgb * (1.0 - 0.35 * np.clip(r2 / 1.1, 0, 1))[..., None]
    return np.clip(rgb, 0.0, 1.0)


def bc7_mode6_solid(blocks: np.ndarray) -> bytes:
    """BC7 blocks, each one solid colour (8-bit values, odd, alpha 255)."""
    c = np.round(blocks * 255).astype(np.uint32)
    c = c | 1                             # the p-bit (1, for alpha 255) is each value's lowest bit
    c7 = c >> 1                           # the 7 stored bits
    r, g, b = c7[..., 0].ravel(), c7[..., 1].ravel(), c7[..., 2].ravel()
    a = np.full_like(r, 127)
    # Mode 6: bits 0-6 the mode (bit 6 set), then R0 R1 G0 G1 B0 B1 A0 A1 (7 each),
    # P0 P1, then the indices (all 0). Bits count from the low end of the block.
    lo = np.full(r.shape, 1 << 6, dtype=np.uint64)
    hi = np.zeros(r.shape, dtype=np.uint64)
    fields = [r, r, g, g, b, b, a, a]
    pos = 7
    for field in fields:
        for bit in range(7):
            value = ((field >> bit) & 1).astype(np.uint64)
            if pos < 64:
                lo |= value << np.uint64(pos)
            else:
                hi |= value << np.uint64(pos - 64)
            pos += 1
    for _ in range(2):                    # P0, P1
        if pos < 64:
            lo |= np.uint64(1) << np.uint64(pos)
        else:
            hi |= np.uint64(1) << np.uint64(pos - 64)
        pos += 1
    out = np.empty(r.shape[0] * 2, dtype="<u8")
    out[0::2], out[1::2] = lo, hi
    return out.tobytes()


def dds_header() -> bytes:
    """DDS with a DX10 header: BC7_UNORM (98), 2D, one level."""
    DDSD_CAPS, DDSD_HEIGHT, DDSD_WIDTH, DDSD_PIXELFORMAT, DDSD_LINEARSIZE = 0x1, 0x2, 0x4, 0x1000, 0x80000
    flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE
    linear = WIDTH * HEIGHT               # BC7: 16 bytes a 4x4 block, one byte a pixel
    header = struct.pack("<4sIIIIIII", b"DDS ", 124, flags, HEIGHT, WIDTH, linear, 0, 1)
    header += b"\0" * 44                  # reserved
    header += struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    header += struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)   # caps: texture
    header += struct.pack("<IIIII", 98, 3, 0, 1, 0)       # BC7_UNORM, TEXTURE2D, array 1
    assert len(header) == 148
    return header


def main() -> int:
    png_dir = None
    if len(sys.argv) == 3 and sys.argv[1] == "--png":
        png_dir = Path(sys.argv[2])
    elif len(sys.argv) != 1:
        print(__doc__ or "usage: make-title-art.py [--png DIR]", file=sys.stderr)
        return 2
    for variant, name in ((0, "pic0.dds"), (1, "pic1.dds")):
        blocks = draw(variant)
        data = dds_header() + bc7_mode6_solid(blocks)
        (ROOT / "sce_sys" / name).write_bytes(data)
        print(f"sce_sys/{name}: {len(data)} bytes")
        if png_dir:
            from PIL import Image
            png_dir.mkdir(parents=True, exist_ok=True)
            pixels = ((np.round(blocks * 255).astype(np.uint8)) | 1).repeat(4, axis=0).repeat(4, axis=1)
            Image.fromarray(pixels, "RGB").save(png_dir / name.replace(".dds", ".png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
