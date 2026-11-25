#!/usr/bin/env python3
"""Draws the title's icon0.png (512x512): a download arrow over a tray on TorBox-like green. numpy + zlib only.

    make_icon.py OUT.png
"""
import struct
import sys
import zlib

import numpy as np

S = 512
y, x = np.mgrid[0:S, 0:S].astype(np.float32) + 0.5


def rounded_rect(x0, y0, x1, y1, r):
    cx = np.clip(x, x0 + r, x1 - r)
    cy = np.clip(y, y0 + r, y1 - r)
    return np.hypot(x - cx, y - cy) <= r


def capsule(ax, ay, bx, by, w):  # thick segment with round caps
    px, py = x - ax, y - ay
    dx, dy = bx - ax, by - ay
    t = np.clip((px * dx + py * dy) / (dx * dx + dy * dy), 0, 1)
    return np.hypot(px - t * dx, py - t * dy) <= w / 2


top = np.array([0x0b, 0xc9, 0x91], np.float32)
bottom = np.array([0x03, 0x7a, 0x5a], np.float32)
img = top + (bottom - top) * (y[..., None] / S)
white = (capsule(256, 96, 256, 300, 52) | capsule(256, 316, 160, 220, 52) | capsule(256, 316, 352, 220, 52)
         | capsule(120, 404, 392, 404, 52) | capsule(120, 404, 120, 352, 52) | capsule(392, 404, 392, 352, 52))
img[white] = 255
rgb = np.clip(img, 0, 255).astype(np.uint8)
raw = b"".join(b"\0" + rgb[i].tobytes() for i in range(S))


def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)


png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", S, S, 8, 2, 0, 0, 0)) + \
    chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
