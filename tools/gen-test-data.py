#!/usr/bin/env python3
#
# Write the load tests' fixtures (tests/data/svg) and the load fuzzer's seed
# corpora (tests/fuzz/corpus/load and tests/fuzz/corpus/style), or with --check, fail unless the committed
# files are exactly what this writes.
#
# The fixtures are written by a program rather than by hand because several are
# byte-level constructions: other encodings, gzip members with every optional
# header field, two members, a damaged CRC, an inflation bomb. Each fixture is
# named for the rule it tests; the tests state the expected outcome, not this
# file.
#
# gzip members are assembled here from the RFC 1952 layout around a raw
# deflate stream (zlib with wbits -15), so every header byte is the one written
# below: mtime 0, no name unless a fixture asks for one.

import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "tests", "data", "svg")
CORPUS = os.path.join(ROOT, "tests", "fuzz", "corpus", "load")
CORPUS_STYLE = os.path.join(ROOT, "tests", "fuzz", "corpus", "style")
CORPUS_LOWER = os.path.join(ROOT, "tests", "fuzz", "corpus", "lower")
LOWER_FIXTURES = os.path.join(ROOT, "tests", "data", "svg-lower")

SVG_NS = "http://www.w3.org/2000/svg"
MINIMAL = ('<svg xmlns="%s" width="10" height="10"/>\n' % SVG_NS).encode()


def gzip_member(payload, name=None, comment=None, extra=None, header_crc=False, crc=None):
    flags = 0
    if header_crc:
        flags |= 0x02
    if extra is not None:
        flags |= 0x04
    if name is not None:
        flags |= 0x08
    if comment is not None:
        flags |= 0x10
    header = bytes([0x1F, 0x8B, 0x08, flags]) + struct.pack("<I", 0) + bytes([0x00, 0xFF])
    if extra is not None:
        header += struct.pack("<H", len(extra)) + extra
    if name is not None:
        header += name + b"\x00"
    if comment is not None:
        header += comment + b"\x00"
    if header_crc:
        header += struct.pack("<H", zlib.crc32(header) & 0xFFFF)
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    body = c.compress(payload) + c.flush()
    if crc is None:
        crc = zlib.crc32(payload) & 0xFFFFFFFF
    return header + body + struct.pack("<II", crc, len(payload) & 0xFFFFFFFF)


def fixtures():
    f = {}
    f["minimal.svg"] = MINIMAL
    f["prefixed.svg"] = ('<s:svg xmlns:s="%s" width="1" height="1"/>\n' % SVG_NS).encode()
    f["bom.svg"] = b"\xef\xbb\xbf" + MINIMAL
    text = '<?xml version="1.0"?>\n' + MINIMAL.decode()
    f["utf16le.svg"] = b"\xff\xfe" + text.encode("utf-16-le")
    f["utf16be.svg"] = b"\xfe\xff" + text.encode("utf-16-be")
    f["utf32le.svg"] = b"\xff\xfe\x00\x00" + text.encode("utf-32-le")
    f["utf32be.svg"] = b"\x00\x00\xfe\xff" + text.encode("utf-32-be")
    f["latin1.svg"] = (
        b'<?xml version="1.0" encoding="ISO-8859-1"?>\n<!-- caf\xe9 -->\n' + MINIMAL
    )
    f["ascii.svg"] = b'<?xml version="1.0" encoding="US-ASCII"?>\n' + MINIMAL
    # The root on line 7, behind every kind of prolog item.
    f["prolog.svg"] = (
        '<?xml version="1.0" encoding="UTF-8" standalone="no"?>\n'
        "<!-- a comment before the doctype -->\n"
        '<?xml-stylesheet href="style.css" type="text/css"?>\n'
        '<!DOCTYPE svg [\n  <!ENTITY w "10">\n]>\n'
        '<svg xmlns="%s" width="&w;" height="10"/>\n' % SVG_NS
    ).encode()
    # The way illustration programs write their namespaces: as internal entities,
    # used in attributes and in content.
    f["entities.svg"] = (
        "<!DOCTYPE svg [\n"
        '  <!ENTITY ns_svg "%s">\n'
        '  <!ENTITY ns_xlink "http://www.w3.org/1999/xlink">\n'
        '  <!ENTITY w "10">\n'
        "]>\n"
        '<svg xmlns="&ns_svg;" xmlns:xlink="&ns_xlink;" width="&w;" height="&w;"><title>&w;</title></svg>\n'
        % SVG_NS
    ).encode()
    # An internal subset of about 1 MiB before the root.
    decls = "".join('  <!ENTITY e%d "value %d">\n' % (i, i) for i in range(32768))
    f["big-subset.svg"] = ("<!DOCTYPE svg [\n%s]>\n" % decls).encode() + MINIMAL
    f["external-dtd.svg"] = (
        '<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" '
        '"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">\n'
    ).encode() + MINIMAL
    f["external-pe.svg"] = (
        '<!DOCTYPE svg [\n  <!ENTITY % ext SYSTEM "ext.dtd">\n  %ext;\n]>\n'
    ).encode() + MINIMAL
    f["external-ge.svg"] = (
        '<!DOCTYPE svg [\n  <!ENTITY ext SYSTEM "ext.xml">\n]>\n'
        '<svg xmlns="%s">\n  <g>&ext;</g>\n</svg>\n' % SVG_NS
    ).encode()
    f["external-ge-attr.svg"] = (
        '<!DOCTYPE svg [\n  <!ENTITY ext SYSTEM "ext.xml">\n]>\n'
        '<svg xmlns="%s" width="&ext;"/>\n' % SVG_NS
    ).encode()
    f["no-namespace.svg"] = b'<svg width="10" height="10"/>\n'
    f["html.xhtml"] = (
        '<html xmlns="http://www.w3.org/1999/xhtml"><body><svg xmlns="%s"/></body></html>\n' % SVG_NS
    ).encode()
    f["other-namespace.svg"] = b'<svg xmlns="http://example.com/not-svg"/>\n'
    f["plain.txt"] = b"this is not XML at all\n"
    f["png.bin"] = b"\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR"
    f["pdf.bin"] = b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"
    f["empty.bin"] = b""
    # The gzip magic with a compression method other than deflate (8) is not
    # a gzip member (RFC 1952: CM 8 is the only method defined).
    f["gzip-method-7.bin"] = b"\x1f\x8b\x07\x00" + b"\x00" * 16
    # Malformed: the end tag on line 7, column 3, does not match.
    f["mismatched.svg"] = (
        '<svg xmlns="%s">\n  <g>\n    <rect/>\n  </g>\n  <g>\n    <circle/>\n  </x>\n</svg>\n' % SVG_NS
    ).encode()
    f["bad-utf8.svg"] = ('<svg xmlns="%s"><title>' % SVG_NS).encode() + b"\xc3\x28" + b"</title></svg>\n"
    f["truncated.svg"] = ('<svg xmlns="%s"><g><rect' % SVG_NS).encode()
    # Entity bombs.
    laughs = ['<!ENTITY lol0 "lol">']
    for i in range(1, 10):
        laughs.append('<!ENTITY lol%d "%s">' % (i, ("&lol%d;" % (i - 1)) * 10))
    f["billion-laughs.svg"] = (
        "<!DOCTYPE svg [\n%s\n]>\n<svg xmlns=\"%s\">&lol9;</svg>\n" % ("\n".join(laughs), SVG_NS)
    ).encode()
    f["quadratic.svg"] = (
        '<!DOCTYPE svg [\n<!ENTITY a "%s">\n]>\n<svg xmlns="%s">%s</svg>\n'
        % ("x" * 50000, SVG_NS, "&a;" * 2000)
    ).encode()
    deep = ['<!ENTITY d0 "x">']
    for i in range(1, 40):
        deep.append('<!ENTITY d%d "&d%d;">' % (i, i - 1))
    f["deep-entities.svg"] = (
        "<!DOCTYPE svg [\n%s\n]>\n<svg xmlns=\"%s\">&d39;</svg>\n" % ("\n".join(deep), SVG_NS)
    ).encode()
    # Shapes of the limits, at a size a test can set a small cap against.
    f["elements-8.svg"] = ('<svg xmlns="%s">' % SVG_NS + "<g/>" * 7 + "</svg>\n").encode()
    f["depth-8.svg"] = ('<svg xmlns="%s">' % SVG_NS + "<g>" * 7 + "</g>" * 7 + "</svg>\n").encode()
    f["attributes-8.svg"] = (
        '<svg xmlns="%s" %s/>\n' % (SVG_NS, " ".join('a%d="1"' % i for i in range(7)))
    ).encode()
    f["references-8.svg"] = ('<svg xmlns="%s">' % SVG_NS + "&#65;" * 8 + "</svg>\n").encode()
    # gzip transports.
    f["minimal.svgz"] = gzip_member(MINIMAL)
    f["two-members.svgz"] = gzip_member(MINIMAL[:20]) + gzip_member(MINIMAL[20:])
    f["header-fields.svgz"] = gzip_member(
        MINIMAL, name=b"drawing.svg", comment=b"a comment", extra=b"AB\x02\x00xy", header_crc=True
    )
    f["not-svg.svgz"] = gzip_member(b"<html/>\n")
    f["double.svgz"] = gzip_member(gzip_member(MINIMAL))
    whole = gzip_member(MINIMAL)
    f["truncated.svgz"] = whole[: len(whole) - 10]
    f["bad-crc.svgz"] = gzip_member(MINIMAL, crc=0x12345678)
    f["trailing.svgz"] = whole + b"this is not a gzip member"
    # About 2 KiB that inflates to 2 MiB of a comment: past a 1 MiB cap.
    f["bomb.svgz"] = gzip_member(
        ('<svg xmlns="%s"><!--' % SVG_NS).encode() + b" " * (2 * 1024 * 1024) + b"--></svg>\n"
    )
    # The typed document (story 5): one fixture per family, each a requirement of the grammar chapters.
    svg = lambda attrs, body: ('<svg xmlns="%s" xmlns:xlink="http://www.w3.org/1999/xlink" %s>%s</svg>\n' % (SVG_NS, attrs, body)).encode()
    f["doc-kinds.svg"] = svg("", "<g><rect/><circle/><ellipse/><line/><path/><polygon/><polyline/><use/><symbol/><defs/>"
                             "<switch/><text>t<tspan>u</tspan></text><linearGradient/><radialGradient/><stop/><clipPath/>"
                             "<mask/><pattern/><marker/><image/><a/><title>x</title><desc/><metadata><r:RDF xmlns:r='urn:r'/></metadata>"
                             "<style>g{}</style><script>1</script><filter/><feGaussianBlur/><foreignObject/><view/>"
                             "<animate/><set/><blink/></g>")
    f["doc-attributes.svg"] = svg('viewBox="0 0 100 50" preserveAspectRatio="xMidYMid slice" width="100%" version="1.1" '
                                  'xml:space="preserve" data-note="1" foo:bar="x" xmlns:foo="urn:foo"',
                                  '<line id="l" x1="1" y1="2em" x2="3%" y2="4" fill="red" style="stroke:blue" onclick="x" '
                                  'aria-label="a" mystery="1" tabindex="3"/>'
                                  '<linearGradient id="g" gradientUnits="objectBoundingBox" spreadMethod="reflect"><stop offset="25%"/></linearGradient>'
                                  '<marker id="m" orient="auto-start-reverse" refX="center" markerWidth="3" markerUnits="strokeWidth"/>')
    f["doc-paths.svg"] = svg("", '<path id="a" d="M 10 20 L 30 40 Z m 5 5 l 1 1 h 3 v -3 z"/>'
                             '<path id="b" d="M0 0 C 1 1 2 2 3 3 S 5 5 6 6 Q 1 1 2 2 T 4 4 A1 1 0 11 5 5 a 2 3 45 0 1 10 -4"/>'
                             '<path id="c" d="M10-5.5L.5.5 1e2-1e-1"/><path id="d" d="M 1 2 L 3"/><path id="e" d="L 1 1"/>'
                             '<path id="f" d="M 0 0 A 1 1 0 2 0 5 5"/><polygon points="0,0 10,0 10,10"/><polyline points="1 2 3"/>')
    f["doc-references.svg"] = svg("", '<defs><rect id="r"/><g id="a"><use href="#r"/></g></defs><use id="u1" href="#a"/>'
                                  '<use id="u2" xlink:href="#r"/><use id="u3" href="#missing"/><use id="self" href="#self"/>'
                                  '<g id="anc"><use href="#anc"/></g><linearGradient id="g1" href="#g2"/><linearGradient id="g2" href="#g1"/>'
                                  '<image href="data:image/png;base64,iVBORw0KGgo="/><image href="data:,hello%20world"/><rect id="r"/>')
    pyr = "<defs><g id=\"a0\"><rect/></g>"
    for k in range(1, 31):
        pyr += '<g id="a%d"><use href="#a%d"/><use href="#a%d"/></g>' % (k, k - 1, k - 1)
    pyr += '</defs><use href="#a30"/>'
    f["doc-pyramid.svg"] = svg("", pyr)
    f["doc-conditional.svg"] = svg("", '<switch><g systemLanguage="fr"/><g systemLanguage="en-US" id="en"/><g/></switch>'
                                   '<g requiredExtensions="http://example.org/x"/><g systemLanguage=""/><g requiredFeatures="x"/>')
    f["doc-text.svg"] = svg("", "<text>one<tspan>two</tspan>three <b>four</b></text>stray<title>A &amp; B</title>")
    f["doc-lists.svg"] = svg("", '<text x="1 2,3em" y="4" dx="5%" rotate="1 2 3">x</text><polygon points="1,2 3,4 5"/>'
                             '<rect pathLength="-1" id=" bad"/>')
    return f


def corpus(fx):
    # Each seed is two option bytes and an input. 0x0f shrinks no limit; an
    # even second byte reads in one piece.
    c = {}
    # The entity bombs run under the default caps now: the XML parser stops them quickly.
    for name, data in fx.items():
        if name == "big-subset.svg":
            continue  # a megabyte: the unit tests load it; a seed that size only slows the fuzzer
        c["plain-" + name] = b"\x0f\x00" + data
    c["capped-billion-laughs.svg"] = b"\xf5\x00" + fx["billion-laughs.svg"]
    c["amplification-billion-laughs.svg"] = b"\x08\x00" + fx["billion-laughs.svg"]
    c["chunked-minimal.svg"] = b"\x0f\x05" + fx["minimal.svg"]
    c["chunked-minimal.svgz"] = b"\x0f\x0b" + fx["minimal.svgz"]
    # Each limit driven by the options byte, at its smallest value.
    for field, name in [
        (0, "minimal.svg"),
        (1, "minimal.svgz"),
        (2, "elements-8.svg"),
        (3, "depth-8.svg"),
        (4, "attributes-8.svg"),
        (5, "entities.svg"),
        (6, "deep-entities.svg"),
        (7, "references-8.svg"),
        (8, "billion-laughs.svg"),
        (9, "doc-paths.svg"),
        (10, "doc-lists.svg"),
        (11, "doc-pyramid.svg"),
        (12, "doc-text.svg"),
        (13, "external-ge.svg"),
    ]:
        c["limit-%d-%s" % (field, name)] = bytes([field, 0x00]) + fx[name]
    return c


def style_corpus():
    """The style fuzzer's seeds: an options byte, then seven pieces separated by line feeds: the style of the root, of the clip
    path's rect, of the symbol's rect and of the group, and the group's fill, transform and stroke-dasharray attributes."""
    def seed(option=0x0f, root="", clip="", symbol="", group="", fill="", transform="", dash=""):
        return bytes([option]) + "\n".join([root, clip, symbol, group, fill, transform, dash]).encode()

    c = {}
    c["empty"] = seed()
    c["colors"] = seed(group="fill: red; stroke: rgb(1 2 3 / 50%); color: hsl(10 20% 30%)")
    c["named-and-hex"] = seed(group="fill: rebeccapurple; stroke: #f80; color: #ff880080; stop-color: transparent")
    c["wide-gamuts"] = seed(group="fill: color(display-p3 1 0 0); stroke: color(prophoto-rgb .5 .5 .5); color: color(rec2020 0.1 0.2 0.3)")
    c["unsupported-color"] = seed(group="fill: lab(50% 10 10); stroke: color-mix(in srgb, red, blue)")
    c["system-colors"] = seed(root="color-scheme: light dark", group="fill: Canvas; stroke: CanvasText; color: ButtonFace")
    c["invalid"] = seed(group="fill: rgb(1; stroke-width: abc; a:;b:c; opacity: 2")
    c["important"] = seed(group="fill: blue !important; stroke: red !IMPORTANT", fill="green")
    c["var"] = seed(group="--c: red; --w: 3; fill: var(--c); stroke-width: var(--w); stroke: var(--none, blue)")
    c["var-cycle"] = seed(group="--a: var(--b); --b: var(--a); fill: var(--a, green)")
    c["var-bomb"] = seed(option=0x02, group="--a: 1 1 1 1 1 1 1 1; --b: var(--a) var(--a) var(--a) var(--a); --c: var(--b) var(--b) var(--b) var(--b); stroke-width: calc(1px * 2); stroke-dasharray: var(--c)")
    c["units"] = seed(group="font-size: 12px; stroke-width: 2em; stroke-dashoffset: 3ex; letter-spacing: 1ch; word-spacing: 1rem; opacity: 50%")
    c["viewport-units"] = seed(group="stroke-width: 10vw; stroke-dashoffset: 5vmin; font-size: 2vh")
    c["percentages"] = seed(symbol="width: 50%; height: 25%; x: 10%; r: 10%; stroke-width: 20%")
    c["dash"] = seed(dash="5 10 15")
    c["dash-percent"] = seed(dash="10%, 2em 3")
    c["dash-invalid"] = seed(dash="-1 2")
    c["dash-zero"] = seed(dash="0 0")
    c["paint-servers"] = seed(fill="url(#g) red", group="stroke: url(#g) none")
    c["paint-missing"] = seed(fill="url(#missing) blue", group="stroke: url(#missing)")
    c["paint-not-a-server"] = seed(fill="url(#c)", group="stroke: url(#m) red")
    c["context-paints"] = seed(fill="context-fill", group="stroke: context-stroke")
    c["transform-attribute"] = seed(transform="rotate(45 10 10) translate(1 2) scale(2) skewX(30) matrix(1 2 3 4 5 6)")
    c["transform-style"] = seed(group="transform: translate(50%, 10%) rotate(0.25turn); transform-origin: 10% 20px")
    c["transform-fill-box"] = seed(group="transform: translate(50%, 10%); transform-origin: 50% 50%; transform-box: fill-box")
    c["transform-3d"] = seed(group="transform: rotateX(30deg) perspective(100px)")
    c["transform-skew"] = seed(group="transform: skew(10deg, 20deg) skewY(5deg) scale(-1, 2)")
    c["transform-huge"] = seed(group="transform: scale(1e300) scale(1e300)")
    c["transform-quarter"] = seed(group="transform: rotate(1e30deg)", transform="rotate(90)")
    c["references"] = seed(group="clip-path: url(#c); mask: url(#c); marker: url(#m); filter: url(#nothere) blur(2px)")
    c["reference-cycle"] = seed(clip="clip-path: url(#c)", group="clip-path: url(#c)")
    c["pattern-cycle"] = seed(clip="fill: url(#g)", group="fill: url(#g)")
    c["use-inheritance"] = seed(group="fill: red; stroke-width: 5", symbol="fill: inherit; stroke-width: 50%")
    c["geometry"] = seed(group="x: 10; y: 5%; width: calc(10% + 2px); height: auto; rx: 1; ry: auto; cx: 1; cy: 2; r: 3")
    c["text-properties"] = seed(group="font: italic bold 12px/1.5 serif; text-decoration: underline; white-space: pre; writing-mode: vertical-rl")
    c["shorthands"] = seed(group="marker: none; overflow: hidden scroll; mask: none; font-variant: small-caps")
    c["limit-style-bytes"] = seed(option=0x00, group="fill: red; stroke: blue; stroke-width: 4; opacity: 0.5")
    c["limit-declarations"] = seed(option=0x01, group="fill: red; stroke: blue; stroke-width: 4; opacity: 0.5")
    c["limit-expansion"] = seed(option=0x02, group="--a: 1 2 3 4 5 6 7 8; --b: var(--a) var(--a) var(--a); fill: var(--b, red)")
    c["limit-references"] = seed(option=0x03, group="fill: red")
    c["limit-attributes"] = seed(option=0x04, group="fill: red")
    c["xml-markup"] = seed(group='fill: red"/><evil/>', fill="&amp;<>", transform="\"")
    c["binary"] = (bytes([0x0f]) + bytes(range(1, 128)).replace(b"\n", b" "))
    return c


def write_all(directory, files, check):
    bad = []
    if not check:
        os.makedirs(directory, exist_ok=True)
        for old in os.listdir(directory):
            if old not in files:
                os.remove(os.path.join(directory, old))
    present = set(os.listdir(directory)) if os.path.isdir(directory) else set()
    for name, data in sorted(files.items()):
        path = os.path.join(directory, name)
        if check:
            if name not in present:
                bad.append("missing " + path)
                continue
            with open(path, "rb") as h:
                if h.read() != data:
                    bad.append("differs " + path)
        else:
            with open(path, "wb") as h:
                h.write(data)
    if check:
        for extra in sorted(present - set(files)):
            bad.append("not generated " + os.path.join(directory, extra))
    return bad


def lower_corpus():
    """The lowering fuzzer's seeds: an options byte (the low nibble names the structural limit of the scene to shrink, the high
    nibble how far), a flags byte (bit 0 a foreground, bits 1 and 2 the viewport), then the document. Every fixture of the
    lowering with no limit shrunk, the structural ones with each limit shrunk, and documents that press on each limit."""
    ns = 'xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"'
    c = {}

    def seed(doc, which=0x0f, value=0, flags=0):
        if isinstance(doc, str):
            doc = doc.encode()
        return bytes([(value << 4) | which, flags]) + doc

    fixtures = {}
    for name in sorted(os.listdir(LOWER_FIXTURES)):
        if name.endswith(".svg"):
            with open(os.path.join(LOWER_FIXTURES, name), "rb") as h:
                fixtures[name[:-4]] = h.read()
    for name, doc in fixtures.items():
        c["fixture-" + name] = seed(doc)
    for name in ("use-pictures", "opacity-groups", "nested-svg-overflow", "use-symbol-svg", "partial-refusals", "structure-transforms"):
        for which in range(1, 7):
            for value in (0, 1, 3):
                c["limit-%d-%d-%s" % (which, value, name)] = seed(fixtures[name], which, value)
    c["foreground"] = seed(fixtures["paints"], flags=1)
    c["viewport-small"] = seed('<svg %s viewBox="0 0 10 10"><path d="M0 0H4V4Z"/></svg>' % ns, flags=2)
    c["viewport-wide"] = seed('<svg %s viewBox="0 0 10 10" preserveAspectRatio="xMaxYMin slice"><path d="M0 0H4V4Z"/></svg>' % ns, flags=6)
    nested = "".join('<g opacity=".5">' for _ in range(30)) + '<path d="M0 0H4V4Z"/>' + "</g>" * 30
    for which in (1, 2, 6):
        for value in (0, 4, 15):
            c["deep-opacity-%d-%d" % (which, value)] = seed('<svg %s>%s</svg>' % (ns, nested), which, value)
    chain = '<defs><path id="u0" d="M0 0H5V5Z"/>' + "".join('<use id="u%d" href="#u%d"/>' % (i, i - 1) for i in range(1, 40)) + '</defs><use href="#u39"/>'
    for which in (1, 3, 5):
        for value in (0, 7):
            c["use-chain-%d-%d" % (which, value)] = seed('<svg %s>%s</svg>' % (ns, chain), which, value)
    pyramid = '<defs><path id="p0" d="M0 0H1V1Z"/>' + "".join('<g id="p%d"><use href="#p%d"/><use href="#p%d"/><use href="#p%d"/></g>' % (i, i - 1, i - 1, i - 1) for i in range(1, 8)) + '</defs><use href="#p7"/>'
    c["use-pyramid"] = seed('<svg %s>%s</svg>' % (ns, pyramid))
    c["use-pyramid-expansion"] = seed('<svg %s>%s</svg>' % (ns, pyramid), 5, 15)
    c["many-shapes-list-items"] = seed('<svg %s>%s</svg>' % (ns, '<path d="M0 0H4V4Z"/>' * 40), 4, 3)
    c["many-shapes-nodes"] = seed('<svg %s>%s</svg>' % (ns, '<path d="M0 0H4V4Z"/>' * 40), 6, 3)
    c["huge-coordinates"] = seed('<svg %s><path d="M 1e308 0 l 1e308 0 l 0 1e308 z"/><svg x="1e308" width="1e308" height="10"><path d="M0 0H4V4Z"/></svg><g transform="scale(1e30)"><g transform="scale(1e30)"><g transform="scale(1e30)"><path d="M0 0H1V1Z"/></g></g></g></svg>' % ns)
    c["refusals"] = seed('<svg %s><path d="M0 0A5 5 0 0 1 9 9"/><rect width="4" height="4"/><path d="M0 0H4V4Z" stroke="red"/><text>t</text><image/><foreignObject/><video/><use href="other.svg#x"/><use id="u" href="#u"/><path d="M0 0H4V4Z" filter="url(#f)" mask="url(#m)" clip-path="url(#c)" style="mix-blend-mode: multiply"/></svg>' % ns)
    c["not-svg"] = seed(b"this is not xml")
    c["short"] = bytes([0x0f])
    return c


def main():
    check = "--check" in sys.argv[1:]
    fx = fixtures()
    bad = (write_all(DATA, fx, check) + write_all(CORPUS, corpus(fx), check) + write_all(CORPUS_STYLE, style_corpus(), check) +
           write_all(CORPUS_LOWER, lower_corpus(), check))
    if check:
        if bad:
            for b in bad:
                print("check-test-data: " + b, file=sys.stderr)
            print("check-test-data: run tools/gen-test-data.py and commit the result", file=sys.stderr)
            return 1
        print("check-test-data: %d fixtures and %d + %d + %d seeds are what the generator writes"
              % (len(fx), len(corpus(fx)), len(style_corpus()), len(lower_corpus())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
