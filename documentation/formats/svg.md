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

## The typed document

* **Tables from the specification.** The element and attribute lists come from the Candidate Recommendation's own
  indexes, not from a reading of them; `elements.md` lists how each element and attribute is read.
* **Properties are not typed by the document.** SVG 2 makes `x`, `y`, `width`, `height`, `cx`, `cy`, `r`, `rx`, `ry`, `transform`
  and the rest properties, parsed as CSS; they are held as written and read through the style layer (below). The `d` attribute of `path` is path data, which
  SVG 2 defines as its own grammar, and is parsed here.
* **Numbers** follow SVG 2's `<number>` and are converted with correct rounding. A value beyond the largest finite
  `double` is invalid; one below the smallest subnormal is zero. Surrounding white space is allowed.
* **Units** are the CSS unit identifiers, ASCII case-insensitively (SVG 2 follows CSS), kept as written.
* **Lists**: `points` also accepts a negative coordinate directly after a number (SVG 2's coordinate pair), as
  browsers do; other lists need a separator.
* **Path data**: a path whose first command is not a moveto renders nothing; on an error the segments before it are
  kept (SVG 2's error rule). Arc flags are single characters, so `A1 1 0 11 5 5` is valid.
* **Conditional processing** follows section 5.7: language matching by equality or prefix and a hyphen,
  `requiredExtensions` always false, `requiredFeatures` ignored; the candidates of a `switch` are the renderable
  elements of its content model (not animation elements or unknown elements).
* **Reference errors** (a missing target, a cycle, an expansion over the bound) do not fail the load: the document is
  drawn without the offending edge, as SVG 2's error rule says to draw what is valid.
* **Ids**: the first element with an id in document order wins; later ones are diagnosed.
* **SVG 1.1 attributes removed from SVG 2** (`version`, `baseProfile`, `contentScriptType`, `contentStyleType`,
  `externalResourcesRequired`, `requiredFeatures`, `xml:base`, `xml:lang`) are kept without a notice.

## Style

* **The cascade is CSS's.** A presentation attribute is a declaration of specificity zero ahead of every rule, `style` is
  the element's own declaration block, and a declaration that is not valid is dropped and reported. A `<style>` element is
  not read yet.
* **Inheritance follows what is drawn.** An element drawn by a `use` inherits from the `use`, not from where it is written
  (SVG 2, 5.6), so a style is read for an *instance* of an element.
* **Lengths** are user units: `1in` is 96, `1cm` is 96 / 2.54, and a percentage is of the width, the height or the
  normalised diagonal of the nearest viewport (its `viewBox` size when it has one), as the property says.
* **`currentColor`** is the element's `color`; when `color` was never specified the paint is the waist's foreground, so a
  host can supply it.
* **Colors** keep their precision and their space; the ones the waist cannot carry are refused and reported, not
  approximated.
* **`transform-origin`** that nobody declared is `0 0`, and `transform-box` is `view-box`, so percentages in a transform
  are of the nearest viewport; with another box they wait for the bounding box.
* **A reference that closes a cycle** through a property is cut and reported; a reference to nothing is the fallback that
  follows it, else `none`, and is reported.
* **Not read:** `d` as a CSS property (the typed attribute is used), the `<style>` element and selectors, 3D transforms,
  `lab()`/`lch()`/`oklab()`/`oklch()`/`color-mix()`, system colors other than `Canvas` and `CanvasText`, `ex`/`ch` from a
  real font, and `filter` and `mask` beyond reading their references.

`properties.md` states where the registry's definition of each property comes from.
