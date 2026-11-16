#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Vector.
"""Write the images of tests/data/images, or with --check fail unless the committed files are what this writes.

    gen-test-images.py [--check]

The images are the lowering's `image` and `feImage` fixtures. The PNG files are built here from the PNG specification (zlib, the
chunk layout, the checks) so that every byte is the one written below: the colour types and depths the decoder reads, an Exif
orientation of each of the eight values, and a picture whose header names more pixels than any limit allows while its data is a few
kilobytes. The JPEG, GIF and WebP files are small encodings pinned as base64 below, so that regenerating them needs no encoder.

Every raster is made of pixels that a test can tell apart: the 3 by 2 picture has red, green, blue on its first row and cyan, magenta,
yellow on its second.
"""
import base64
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tests", "data", "images")

JPEG = (
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFB"
    "QUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAAIAAgDAREAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUF"
    "BAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmq"
    "KjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAEC"
    "AxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqK"
    "mqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDmfhR+yp/qf9D9P4a9XOeMN/eI8NuPfg98/9k="
)
GIF = "R0lGODlhAgACAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQACgAAACwAAAAAAgACAAAIBgABCAQQEAAh+QQBCgABACwAAAAAAgACAIEAAP8AAAAAAAAAAAAIBgABCAQQEAA7"
WEBP = "UklGRjAAAABXRUJQVlA4TCMAAAAvAUAAEB8gEEjeHzqN+RcQFPwfnYCg6LrlImYPwg0YIvofAgA="


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def exif(orientation):
    # A big-endian TIFF header and one IFD entry: Orientation (0x0112), SHORT, count 1.
    return b"MM\x00*\x00\x00\x00\x08" + struct.pack(">H", 1) + struct.pack(">HHI", 0x0112, 3, 1) + struct.pack(">HH", orientation, 0) + b"\x00\x00\x00\x00"


def png(width, height, color_type, depth, rows, palette=None, orientation=None, extra=b""):
    """A PNG; `rows` is the filtered-free scanline bytes of each row (no filter byte)."""
    header = chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, color_type, 0, 0, 0))
    out = b"\x89PNG\r\n\x1a\n" + header
    if palette is not None:
        out += chunk(b"PLTE", b"".join(bytes(c) for c in palette))
    if orientation is not None:
        out += chunk(b"eXIf", exif(orientation))
    out += extra
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    out += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    return out


def rgb_3x2():
    return [[255, 0, 0, 0, 255, 0, 0, 0, 255], [0, 255, 255, 255, 0, 255, 255, 255, 0]]


def sparse(width, height):
    """A one-bit gray picture of all zero: tens of megabytes of scanlines that deflate to a few kilobytes."""
    row = b"\x00" + bytes((width + 7) // 8)
    comp = zlib.compressobj(9)
    data = b""
    for _ in range(height):
        data += comp.compress(row)
    data += comp.flush()
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 1, 0, 0, 0, 0)) + chunk(b"IDAT", data) + chunk(b"IEND", b"")


def files():
    yield "red-blue-2x1.png", png(2, 1, 2, 8, [[255, 0, 0, 0, 0, 255]])
    yield "rgb-3x2.png", png(3, 2, 2, 8, rgb_3x2())
    yield "rgba-3x2.png", png(3, 2, 6, 8, [[255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0], [0, 255, 255, 255, 255, 0, 255, 128, 255, 255, 0, 0]])
    yield "gray-4x2.png", png(4, 2, 0, 8, [[0, 85, 170, 255], [255, 170, 85, 0]])
    yield "indexed-2x2.png", png(2, 2, 3, 8, [[0, 1], [1, 0]], palette=[(255, 0, 0), (0, 0, 255)])
    yield "rgb16-2x1.png", png(2, 1, 2, 16, [[255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255]])
    for o in range(1, 9):
        yield "exif-%d.png" % o, png(3, 2, 2, 8, rgb_3x2(), orientation=o)
    yield "photo-8x8.jpg", base64.b64decode(JPEG)
    yield "animated-2x2.gif", base64.b64decode(GIF)
    yield "lossless-2x2.webp", base64.b64decode(WEBP)
    # The header of the first names 20,000 by 20,000 pixels (4e8, past the default limit of 2^25) and its data is a few kilobytes.
    yield "sparse-20000x20000.png", sparse(20000, 20000)
    yield "empty.bin", b""
    yield "not-an-image.bin", b"this is not an image\n" * 4
    yield "document.svg", b'<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4"><rect width="4" height="4" fill="green"/></svg>\n'
    yield "red-blue-2x1 spaced.png", png(2, 1, 2, 8, [[255, 0, 0, 0, 0, 255]])


def main():
    os.makedirs(OUT, exist_ok=True)
    stale = False
    expected = dict(files())
    for name, data in expected.items():
        path = os.path.join(OUT, name)
        try:
            current = open(path, "rb").read()
        except FileNotFoundError:
            current = None
        if current != data:
            if "--check" in sys.argv:
                print("stale: tests/data/images/" + name)
                stale = True
            else:
                with open(path, "wb") as f:
                    f.write(data)
    if "--check" in sys.argv:
        for name in sorted(os.listdir(OUT)):
            if name not in expected:
                print("unexpected: tests/data/images/" + name)
                stale = True
    sys.exit(1 if stale else 0)


main()
