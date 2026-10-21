# SVG

**Normative reference:** Scalable Vector Graphics (SVG) 2, W3C Candidate Recommendation,
4 October 2018. Where SVG 2 is silent, SVG 1.1 (Second Edition), W3C Recommendation, 16 August 2011.

This page records each choice this library makes where the specifications leave room, or where
implementations disagree, with the reason. It grows with each part of SVG that is read.

## Recognising a document

* **The root decides.** A document is SVG when its root element is `svg` in the namespace
  `http://www.w3.org/2000/svg` (SVG 2; Namespaces in XML 1.0). A prefix is allowed
  (`<s:svg xmlns:s="...">`). An `svg` element in no namespace is not SVG: it is XML that happens to
  use the name, and is refused as unsupported, with the element and its line.
* **SVG inside another vocabulary** (inline SVG in XHTML) is not an SVG document; the root is
  `html`. It is the HTML parser's to read.
* **Encodings** are the XML parser's: UTF-8 with or without a byte order mark, UTF-16 and UTF-32 in
  either byte order, and documents declaring `ISO-8859-1` or `US-ASCII`. Line and column numbers in
  diagnostics count characters of the original encoding.

## The document type declaration

* **Internal subsets are read.** Illustration programs declare their namespaces and other strings
  as internal entities (`<!ENTITY ns_svg "http://www.w3.org/2000/svg">`) and use them in
  attributes; such files load.
* **Nothing external is read.** An external DTD subset (the SVG 1.1 doctype names one) and an
  external parameter entity are not fetched: the load continues with what the file itself declares,
  and reports once that DTD knowledge is incomplete. A reference to an external general entity in
  content is kept unresolved and reported, as a warning, at the element where it is used; the
  document loads. A reference to an external entity in an attribute value is not well-formed XML
  (XML 1.0, "No External Entity References"), so the file is refused.
* **Entity expansion is bounded** by `GVEC_Limits`: total replacement text, nesting, and the number
  of references.

## Compressed documents

* **`.svgz` is gzip** (RFC 1952) around an SVG document. It is recognised by the gzip member's
  first bytes, not by a file name.
* **Every member is read.** A gzip file is a series of members, and their contents are
  concatenated.
* **Trailing bytes** after the last member that do not begin another member are ignored with a
  warning, as gzip(1) ignores them.
* **One level.** A gzip file whose content is gzip again is refused.

## Not read

Script, SMIL and CSS animation, `foreignObject`, and anything outside the file (external
references, fonts, images, style sheets) are not read. Each will be refused with its element and
line when the document is built.
