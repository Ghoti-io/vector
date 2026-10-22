#!/usr/bin/env python3
#
# Write the load tests' fixtures (tests/data/svg) and the load fuzzer's seed
# corpus (tests/fuzz/corpus/load), or with --check, fail unless the committed
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
    # The entity bombs carry a 256-byte expansion cap (field 5, high nibble
    # 15): the XML parser's expansion is slow on them under its default caps
    # for now, so a seed with the defaults would stall the replay.
    bombs = {"billion-laughs.svg", "quadratic.svg", "deep-entities.svg"}
    for name, data in fx.items():
        if name == "big-subset.svg":
            continue
        options = b"\xf5\x00" if name in bombs else b"\x0f\x00"
        c["plain-" + name] = options + data
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
        (8, "doc-paths.svg"),
        (9, "doc-lists.svg"),
        (10, "doc-pyramid.svg"),
        (11, "doc-text.svg"),
        (12, "external-ge.svg"),
    ]:
        c["limit-%d-%s" % (field, name)] = bytes([field, 0x00]) + fx[name]
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


def main():
    check = "--check" in sys.argv[1:]
    fx = fixtures()
    bad = write_all(DATA, fx, check) + write_all(CORPUS, corpus(fx), check)
    if check:
        if bad:
            for b in bad:
                print("check-test-data: " + b, file=sys.stderr)
            print("check-test-data: run tools/gen-test-data.py and commit the result", file=sys.stderr)
            return 1
        print("check-test-data: %d fixtures and %d seeds are what the generator writes"
              % (len(fx), len(corpus(fx))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
