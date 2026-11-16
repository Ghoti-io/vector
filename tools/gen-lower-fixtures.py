#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Vector.
"""Write the fixtures of tests/data/svg-lower that are many of one shape.

    gen-lower-fixtures.py [--check]

The images of tests/data/images (tools/gen-test-images.py) in the fixtures that draw them: the elements are written here so that the data: URIs
are the bytes of those files. The nineteen forms of preserveAspectRatio (the nine alignments with `meet` and
with `slice`, and `none`), each mapping a 20 by 10 viewBox onto a 64 by 40
viewport with three markers at known corners, so that the golden raster shows
where the box landed. The other fixtures are written by hand. With --check
nothing is written; the exit status is 1 when a file differs.
"""
import base64
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "tests", "data", "svg-lower")
ALIGNS = ["none", "xMinYMin", "xMidYMin", "xMaxYMin", "xMinYMid", "xMidYMid", "xMaxYMid", "xMinYMax", "xMidYMax", "xMaxYMax"]

TEMPLATE = """<?xml version="1.0"?>
<!-- preserveAspectRatio="%(par)s" maps a 20 by 10 viewBox onto a 64 by 40 viewport (P3, SVG 2 section 8.2); written by tools/gen-lower-fixtures.py. -->
<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40" viewBox="2 3 20 10" preserveAspectRatio="%(par)s">
  <path d="M2 3H22V13H2Z" fill="#ecf0f1"/>
  <path d="M2 3H8V9H2Z" fill="#c0392b"/>
  <path d="M16 7H22V13H16Z" fill="#2980b9"/>
  <path d="M10 5H14V11H10Z" fill="#27ae60"/>
</svg>
"""


IMAGES = os.path.join(os.path.dirname(OUT), "images")


def data_uri(name, mime="image/png"):
    with open(os.path.join(IMAGES, name), "rb") as f:
        return "data:%s;base64,%s" % (mime, base64.b64encode(f.read()).decode())


IMAGES_SVG = """<?xml version="1.0"?>
<!-- Raster images (P3, SVG 2 section 5.3; written by tools/gen-lower-fixtures.py from tests/data/images): every format and color type, preserveAspectRatio
     meet and slice, an Exif orientation, opacity and a transform, a clip, image-rendering, and an image drawn through a use. -->
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="64" height="64" viewBox="0 0 64 64">
  <rect width="64" height="64" fill="#ecf0f1"/>
  <image x="2" y="2" width="28" height="20" image-rendering="optimizeSpeed" href="%(rgb)s"/>
  <image x="34" y="2" width="28" height="20" preserveAspectRatio="xMinYMin slice" image-rendering="optimizeSpeed" href="%(rgba)s"/>
  <image x="2" y="24" width="14" height="20" image-rendering="optimizeSpeed" href="%(exif)s"/>
  <image x="20" y="26" width="16" height="8" opacity="0.5" transform="rotate(10 28 30)" image-rendering="optimizeSpeed" href="%(gray)s"/>
  <clipPath id="c"><circle cx="50" cy="34" r="9"/></clipPath>
  <image x="40" y="24" width="20" height="20" preserveAspectRatio="none" clip-path="url(#c)" image-rendering="smooth" href="%(indexed)s"/>
  <image x="2" y="48" width="24" height="12" image-rendering="pixelated" href="%(rgb16)s"/>
  <defs><image id="again" width="10" height="10" image-rendering="optimizeSpeed" href="%(webp)s"/></defs>
  <image x="30" y="48" width="12" height="12" href="%(jpg)s"/>
  <use href="#again" x="44" y="48"/>
  <image x="54" y="48" width="8" height="8" image-rendering="optimizeSpeed" href="%(gif)s"/>
</svg>
"""

FILES_SVG = """<?xml version="1.0"?>
<!-- Images by file (the resource policy; written by tools/gen-lower-fixtures.py): a file beside the images directory is read, and what leaves the
     directory the gate allows, names a network, or is not there is not. -->
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="64" height="64" viewBox="0 0 64 64">
  <rect width="64" height="64" fill="#ecf0f1"/>
  <image x="2" y="2" width="28" height="20" image-rendering="optimizeSpeed" href="../images/red-blue-2x1.png"/>
  <image x="34" y="2" width="28" height="20" image-rendering="optimizeSpeed" href="../images/red-blue-2x1%%20spaced.png"/>
  <image x="2" y="30" width="28" height="12" image-rendering="optimizeSpeed" xlink:href="../images/rgba-3x2.png"/>
  <image x="34" y="30" width="28" height="12" href="../../secret.png"/>
  <image x="2" y="48" width="12" height="12" href="https://example.com/a.png"/>
  <image x="20" y="48" width="12" height="12" href="../images/missing.png"/>
  <image x="38" y="48" width="12" height="12" href="../images/document.svg"/>
</svg>
"""

FILTER_SVG = """<?xml version="1.0"?>
<!-- feImage (FL17; written by tools/gen-lower-fixtures.py from tests/data/images): an image fitted into the filter region, into a subregion, sliced,
     and under the transform of its element. -->
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="64" height="64" viewBox="0 0 64 64">
  <rect width="64" height="64" fill="#ecf0f1"/>
  <filter id="fit" x="0" y="0" width="1" height="1"><feImage image-rendering="optimizeSpeed" href="%(rgb)s"/></filter>
  <filter id="part" x="0" y="0" width="1" height="1"><feImage x="36" y="4" width="8" height="6" preserveAspectRatio="none" image-rendering="optimizeSpeed" href="%(rgba)s"/></filter>
  <filter id="slice" x="0" y="0" width="1" height="1"><feImage preserveAspectRatio="xMinYMin slice" image-rendering="optimizeSpeed" href="%(rgb)s"/></filter>
  <rect x="2" y="2" width="28" height="20" fill="red" filter="url(#fit)"/>
  <rect x="34" y="2" width="12" height="12" fill="red" filter="url(#part)" transform="translate(-2 0)"/>
  <rect x="2" y="30" width="20" height="14" fill="red" filter="url(#slice)"/>
  <rect x="34" y="30" width="20" height="14" fill="red" filter="url(#fit)" transform="translate(4 4) scale(0.8)"/>
</svg>
"""


def images_text(template):
    return template % {
        "rgb": data_uri("rgb-3x2.png"),
        "rgba": data_uri("rgba-3x2.png"),
        "exif": data_uri("exif-6.png"),
        "gray": data_uri("gray-4x2.png"),
        "indexed": data_uri("indexed-2x2.png"),
        "rgb16": data_uri("rgb16-2x1.png"),
        "webp": data_uri("lossless-2x2.webp", "image/webp"),
        "jpg": data_uri("photo-8x8.jpg", "image/jpeg"),
        "gif": data_uri("animated-2x2.gif", "image/gif"),
    }


def files():
    yield "images.svg", images_text(IMAGES_SVG)
    yield "images-files.svg", FILES_SVG % {}
    yield "images-filter.svg", images_text(FILTER_SVG)
    for align in ALIGNS:
        for mode in ([""] if align == "none" else ["meet", "slice"]):
            par = (align + " " + mode).strip()
            name = "par-" + align.lower() + ("-" + mode if mode else "") + ".svg"
            yield name, TEMPLATE % {"par": par}


def main():
    stale = False
    for name, text in files():
        path = os.path.join(OUT, name)
        try:
            current = open(path, encoding="utf-8").read()
        except FileNotFoundError:
            current = None
        if "--check" in sys.argv:
            if current != text:
                print("stale: tests/data/svg-lower/" + name)
                stale = True
        elif current != text:
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
    sys.exit(1 if stale else 0)


main()
