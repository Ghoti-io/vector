# Design

**Status:** the library, the format registry, loading SVG and the typed SVG document are built (rules L1 to L11 and
T1 to T14 below). The conformance harness scores the library against a published corpus (`conformance.md`). The
cascade (the typing of CSS properties and presentation attributes) and the lowering are not built; the lowering entry
points exist and refuse every document at its root.

`vector` reads vector formats and lowers them into `shape-scene`'s two recording forms, an edit of
the retained scene and an immediate-mode list. It is a *front end* of the shape family: it never
renders, never publishes a scene and never links a renderer, so that the scene it produces serves
every sink alike. The pipeline is

```
bytes ─transport─▶ format bytes ─text─▶ XML tree ─build─▶ typed document ─lower─▶ edit or list
       (gzip)                    (XML)             (vector)                (vector)
```

Each stage is separate. The lowering reads no XML and makes no parsing decision; the build pass
turns the tree into a typed document and then frees it. The first three stages exist.

## The rules

The numbers are stable; tests and plants cite them.

**L1 Results.** `GVEC_Result` is the suite's vocabulary, `GVEC_OK` to `GVEC_ERR_INTERNAL`, closed by
`GVEC_RESULT_COUNT`; `gvec_result_string` names each and says "unknown" past the end. Output
parameters come last, are written only on success, and a failure leaves nothing allocated.

**L2 Streams.** A reading stream is a borrowed memory buffer or a file the caller names, read in
binary mode; a writing stream collects bytes in memory. Every reading stream has a lookahead buffer
in front of its source, so `peek` works on one that cannot seek, and `tell` is the source position
less what is buffered. There is no `read_line`: nothing here reads by line, because the XML parser
takes the whole document at once.

**L3 Limits.** `GVEC_Limits` caps input bytes, inflated bytes, elements, nesting depth, attributes
per element, entity expansion, entity nesting, references and diagnostics kept. Every field has a
finite default; zero selects it, and no field means unlimited, because "zero means unlimited" and
"zero means default" are two meanings for one value. Each field also has a ceiling, the limit of the
XML parser it is enforced by, which is also its default: a cap larger than the one enforcing it
would be a promise nothing keeps. A field above its ceiling is `GVEC_ERR_INVALID`, with a
`GVEC_DIAG_LIMIT` entry naming it. The diagnostics cap's default is 4096 and its ceiling 2^20.

Elements are counted by this library, after the parse, because the parser's own count is of nodes
of every kind. Every other cap is the parser's, and the parser's status is mapped back to the field.
The parser's caps that `GVEC_Limits` does not expose (name length, attribute-value length, namespace
declarations, declarations, total work) keep the parser's defaults; reaching one is `GVEC_ERR_LIMIT`
with a `GVEC_DIAG_XML` entry carrying the parser's status.

**L4 Diagnostics.** The caller owns a `GVEC_Diagnostics` and every load and lowering appends to it,
on success and on failure: a failed load has no document to hold them. An entry has a severity, a
stable code, a code-specific `detail`, a line and column in characters of the original encoding, a
byte offset in the parsed UTF-8 form (of the inflated bytes for a gzip transport), the element's
local name (owned by the list) and a static message. Past the cap the first entries are kept and
the rest counted in `dropped`; an entry that could not be stored for lack of memory is counted
there too. Nothing is lost silently.

**L5 The registry.** The codecs are a `const` table in the library, in probe order. A constructor
would fill the table before `main`, but a constructor in a static archive is dropped by the linker
unless the caller links the whole archive, which is a trap for every static user; a table needs no
registration step and is complete however the library is linked. It is never written, so reading it
is safe from any thread. Each codec has a name, capability bits, and callbacks for probe, load,
dump, the two lowerings and a save slot reserved for the writer.

**L6 Transports.** A load buffers the whole input within `max_input_bytes`. If it begins with a gzip
member (`1f 8b 08`), the members are inflated one after another, each through `compress`'s streaming
decoder, and their outputs concatenated (RFC 1952: a gzip file is a series of members). The members
are walked here rather than by `compress`'s own concatenation, because only then is the input
position after the last member known: bytes there that do not begin another member are ignored with
a warning, as gzip(1) does. The output buffer is never larger than `max_inflated_bytes + 1`, so a
small member that would inflate past the cap is stopped at the cap, not after. A damaged member is
`GVEC_ERR_CORRUPT`; inflated bytes that are gzip again are `GVEC_ERR_UNSUPPORTED`. The document
records the transport.

**L7 The probe.** Each codec is asked, in order, whether the bytes are its format. SVG's probe runs
`text`'s pull reader, with L9's options, until the first start tag, and says yes when that element
is `svg` in the SVG namespace. SVG has no magic number, and the root can sit behind a doctype whose
internal subset has no length a fixed window could know, in any of five encodings; only an XML
parser can say where the root is, so the probe asks the parser that will read the file, and the two
cannot disagree. The probe parses the prolog and stops, so a bomb in the content does not reach it.

A probe that says no gives its reason, and a load that no codec claims reports the reasons: the XML
error that stopped the probe, located, or the root element it found. A probe that cannot decide,
because an allocation failed or a limit was reached before the root, passes that result on rather
than saying no: a limit or a lack of memory is not a verdict on the format.

**L8 Loading.** The probe, then the matched codec's load: SVG's parses the bytes with
`gtext_xml_parse`, checks the root again (unreachable after L7, kept against a defect in the probe),
counts the elements against `max_elements`, reports each kept external entity reference where it is
used, and reports once when an external DTD subset or parameter entity was not read. The tree is
freed before the load returns. `GVEC_Load_Options` carries the limits and the document's allocator;
each further input (languages for `switch`, a viewport, a colour scheme, a default font) will be
added by the step that reads it, and none comes from the environment.

**L9 The XML options.** One function writes `text`'s parse options from the limits: the DTD
processed, so internal entities expand in content and attributes; external access denied and no
resolver, so the parser has no way to fetch anything; the policy for an unavailable external general
entity set to keep it as a reference node instead of failing the load; no base URI. Every other
field is the parser's default. A test reads the options field by field.

**L10 The dump and hash.** `gvec_doc_dump` writes `vector-doc 1` (the dump format), then `format`
and `transport` lines, then the codec's own lines (none yet). `gvec_doc_hash` is the low 64 bits of
MurmurHash3 x64-128 (cutil's), seed 0, over the dump's bytes, read little-endian, so it is the same
on every target. Before the first release the format stays 1 and is extended in place.

**L11 The lowering entry points.** `gvec_doc_lower_edit` and `gvec_doc_lower_list` walk the document once and
emit the same drawing into an edit of the retained scene or into an immediate-mode list (the lowering, P1 to P10, below).
Neither publishes, renders or touches a sink.

## The typed document (T1 to T14)

`svg.h` is the public face; `src/codec/svg/` holds the build. The markup tree from `text` is built into the document
and freed inside the load, so no pointer into it survives.

**T1 Tables.** The element kinds, the attributes the SVG 2 attribute index defines for each element, the presentation
attributes and the properties are generated by `tools/svg2/gen-tables.py` from the pinned pages of the Candidate
Recommendation (`tools/svg2/fetch.sh`), and `grammars.tsv` assigns a grammar to every (element, attribute) pair of the
index. The generator fails on a pair no rule maps and on a rule that maps no pair; `check-tables` regenerates the
committed tables and fails if they differ, and is shown to fail on its plants. `documentation/elements.md` is the
coverage it prints.

**T2 Ownership.** One arena per document, taken from the caller's allocator and released together. Every accessor
borrows from it; nothing has a lifetime of its own.

**T3 Elements.** One kind per SVG 2 element, `UNKNOWN` for an element in the SVG namespace that SVG 2 does not define and
`FOREIGN` for another namespace; both are kept in place with their children and attributes, and never rendered.
Character data is kept where it is read (`title`, `desc`, `style`, `script`, the text elements, `a`, `foreignObject`,
`metadata`, and under unknown and foreign elements) and, anywhere else, when it is not only white space, exactly as
written and in order among the elements around it.

**T4 Attribute classes.** Every attribute is kept, in source order, with its raw text and where it was, in one class:
`TYPED` (parsed), `INVALID` (its grammar failed; unspecified, with a located diagnostic), `PROPERTY` (a CSS property or
presentation attribute, held as written for the cascade), `RAW` (defined by SVG 2 but not read in this version:
animation, filters, ARIA, `data-*`, SVG 1.1 leftovers), `UNKNOWN` (not defined for the element, or foreign) and
`SCRIPT` (an event handler, never evaluated). Namespace declarations are not attributes. `style` is a property;
`d` of `path` is path data and also a presentation attribute.

**T5 Numbers and lengths.** SVG 2's `<number>` through `math`'s correctly rounded conversion, the same bits on every
target; an overflow is invalid, an underflow is zero. A length is a number and one of the CSS units (ASCII
case-insensitive), kept as written and never converted here (`em` and `ex` need the cascade). Angles likewise.

**T6 Lists.** Numbers or lengths separated by white space and one optional comma, bounded by `max_list_items`; a trailing
comma is an error; a `points` list accepts SVG 2's coordinate followed directly by a negative one, and an odd count
keeps the complete pairs.

**T7 Path data.** The grammar of SVG 2 section 9.3, both forms: the commands as written (letter, arguments, and
whether the letter was an implicit repeat) and the absolute form (every point absolute, `H`/`V` as lines, the
reflected control points of `S`/`T` written out, an arc left in endpoint form for the shape family to convert).
An error keeps the segments before it with the byte offset; a path must begin with a moveto; a coordinate that
overflows is an error at its segment; `max_path_segments` bounds the count.

**T8 Other grammars.** `viewBox` (a negative size is invalid, a zero size is kept), `preserveAspectRatio` (with the SVG 1.1
`defer` accepted and ignored), `orient`, a stop's `offset`, integers, enumerations (case-sensitive, from the CR),
`refX`/`refY` with their keywords, language lists, extension lists, and references.

**T9 References.** `href`, or `xlink:href` when there is none, is an internal reference (`#id`, percent-decoded),
a `data:` URI (RFC 2397, parsed here; decoded by `gvec_svg_data_uri_decode`, through `cutil`'s base64 for the base64
form), or an external reference, kept and never fetched. Ids resolve within the document only; a duplicate keeps the
first in document order and is diagnosed.

**T10 Cycles and expansion.** A `use` is on a cycle when it is in a strongly connected component of the graph in which a
`use` points at every `use` inside its referent's subtree, or is itself inside its referent; every member is marked,
diagnosed once, and its reference cut. Gradient, pattern and filter `href` chains are cut the same way. The number of
elements each `use` instantiates is computed bottom-up without expanding anything, in O(n log n) however large the
ranges are, and compared with `max_reference_expansion` per `use` and for the uses no other `use` contains; a
`use` over a bound is marked and not instantiated, and the document still loads.

**T11 Conditional processing.** `systemLanguage` is true when a language of the caller (`GVEC_Load_Options.languages`,
default `en`) equals one of its tags or is a prefix of one followed by `-`, ASCII case-insensitively; `requiredExtensions`
is false for any value, because no extension is supported; an empty value of either is false; `requiredFeatures`
is not SVG 2 and is ignored. An element whose condition is false is treated as `display: none`, and a `switch` records
the first renderable child whose conditions are true.

**T12 Animation and script** are kept and not applied or evaluated, with one notice per document.

**T13 Keys.** An element's 64-bit key is the hash (Murmur3, little-endian bytes) of its parent's key and either its
`id` or its kind and its place among the siblings of that kind. Two elements that come out equal are told apart
deterministically and the later is flagged. An element with an id keeps its key when elements of other kinds
are inserted around it.

**T14 Dump and hash.** `vector-doc 1` is extended in place with an element line, an attribute line and a text line
per node in document order: keys in hex, numbers in the shortest spelling that reads back to the same bits, strings
with every byte outside printable ASCII, the quote and the backslash escaped. The hash is unchanged.

**Limits** added: `max_path_segments`, `max_list_items`, `max_reference_expansion`, `max_text_bytes`. Character data
past the last is a failed load (a structural bound); the others are located errors on the attribute or the `use`, so
one hostile attribute does not erase the rest of a drawing.

## The style of the document (C1 to C14)

`style.h` is the public face; `src/codec/svg/style/` holds the parts. The engine is the `text` library's: its registry of
properties, its cascade, `var()` and the computed value of every type. This library does not tokenise, parse or cascade
CSS; it supplies the SVG view of the engine, runs it over the document, and resolves the used values only an SVG
renderer can.

**C1 The property set.** `tools/svg2/properties.tsv` names every property of the Candidate Recommendation's property
index and presentation attribute list, and the properties the chapters take from other modules (`transform`, `mask`,
`filter`, `mix-blend-mode`, `isolation`, `paint-order`, `vector-effect`, the geometry properties, the font and text
properties). The generator fails when a property of either page is not in the file, writes `svg_prop.h` (one enumerator
for each) and `documentation/properties.md`, and `check-tables` fails when they differ.

**C2 Registration and overrides.** Each property is `generated` (the pin the registry of `text` is made from defines it
and the definition stands), `registered` (the pin lacks it: `color-rendering`, `glyph-orientation-horizontal`) or
`overridden` (the pin defines it differently from the CR or cannot decide a value for it: `fill` and `stroke`, whose
`<paint>` has no `<color>` in Fill and Stroke 3; `stroke-width`, which that module makes a list; `stop-color` and
`stop-opacity`, which have no initial value; `glyph-orientation-vertical`). An override is never silent:
`gvec_svg_style_context_override_at()` lists them with the sentence they rest on, and building the registry fails if a
`generated` property is not in the pin or a `registered` one is.

**C3 Instances.** An element is drawn once for each place it is drawn, and a style is a property of the element *as
drawn*. The handle is an instance: an element and the instance it inherits from. The root, a child of an instance, and
the referent of a `use` are made by the caller as it walks; the referent's inheritance parent is the `use`, its tree
parent the element it is written in, and every descendant has the clone's root as an inheritance ancestor. A style is
computed the first time an instance is read, from its parent's style, iteratively (a chain of `use` is bounded by memory
and not by the stack), and kept. Nothing expands `use`: the cost is what is drawn.

**C4 Feeding the cascade.** A presentation attribute enters at the engine's presentation-attribute origin (author level,
specificity 0, ahead of every rule) with SVG's attribute-only spellings as options of the engine (a unitless number is a
length in `px`, `transform` in SVG's list syntax); `style` enters as the element's declaration block, with `!important`.
A declaration that is not a value of its property is dropped, as CSS says, and reported
(`GVEC_DIAG_STYLE_INVALID`) once for the element, at the attribute that holds it, with the declaration's byte offset in
the attribute's value in `detail`. The registry's table of presentation attributes is the engine's SVG 2 table with the two
attributes of the CR's list that the served draft dropped.

**C5 Computed values** are the engine's: `em`, `ex`, `ch` and `rem` are absolute, a color is the declared color, `calc()`
is simplified. A document has no fonts, so the metrics a font would give are the fallbacks CSS names (`ex` and `ch` are half
the font size, `cap` seven tenths, `ic` one `em`, a `normal` line height 1.2 `em`).

**C6 Lengths and viewports.** A length is read as a used value in user units: absolute units by CSS's fixed ratios, `vw`
`vh` `vmin` `vmax` against the viewport of the load options (300 by 150 by default), and a percentage against the basis
the property names: the width of the nearest viewport (`x`, `width`, `cx`, `rx`), its height (`y`, `height`, `cy`, `ry`) or
the normalised diagonal `sqrt((w^2 + h^2) / 2)` (`r`, `stroke-width`, `stroke-dasharray` and `stroke-dashoffset`). The
viewport an instance is resolved against is the one its parent establishes: the outermost `svg` is the load options'
viewport sized by its own `width` and `height`; a nested `svg`, a `symbol` drawn by a `use` (which overrides its `width`
and `height`) and a `marker` (`markerWidth` by `markerHeight`) establish their own; and when the element that establishes
one has a usable `viewBox` the basis is the `viewBox` size in user units.

**C7 `currentColor`** is the element's computed `color`, not the host's foreground. When `color` is `CanvasText`, its
initial value, the used value is the waist's foreground, so that a host (an icon, a font's glyph) can supply it; a `color`
that is specified is a concrete color even when it is black. Keyword `currentcolor` is inherited as a keyword, so it is
resolved against the element that uses it.

**C8 Colors** are straight `double` components and the color space of the waist, never 8 bits. sRGB, linear sRGB, Display P3,
linear Display P3 and Adobe RGB (1998) are carried as they are; Rec. 2020 and ProPhoto RGB, which the waist cannot carry as
written, are converted with the `color` library's matrices to linear Rec. 2020; `hsl()` and `hwb()` are converted in
`double`. `lab()`, `lch()`, `oklab()`, `oklch()`, `color(xyz ...)`, `color-mix()`, relative colors and system colors other
than `Canvas` and `CanvasText` are kept by the engine and refused here: reading one is `GVEC_ERR_UNSUPPORTED` and a
`GVEC_DIAG_UNSUPPORTED_COLOR_SPACE` with the element's location, because converting them would approximate a color the
waist cannot yet carry. `Canvas`, `CanvasText` and `light-dark()` follow the element's used color scheme: `color-scheme:
normal` is light, and the preference of the load options decides only for an element that supports both.

**C9 Other used values.** `transform` is the list of its functions, the origin and the box; with the initial
`transform-box: view-box` every percentage is of the nearest viewport and the map is final, with another box a percentage
depends on a bounding box that only the lowering has and `gvec_svg_transform_resolve()` makes the map from it. The maps
come from the shape family's constructors (a quarter turn is exact). An undeclared `transform-origin` is `0 0`, the used
value of the initial value of an SVG element without a layout box. A dash array has an even number of entries (an odd
list is repeated) and is `none` when its entries sum to zero. A paint is `none`, a color, a paint server with its
fallback, `context-fill` or `context-stroke`; a `url()` with no target, or whose target is no paint server, is the
fallback and otherwise `none`, and is reported. `clip-path`, `mask`, `marker-*` and `filter` are read as lists of
references.

**C10 Reference cycles through properties.** `gvec_svg_style_resolve_references()` adds the references of properties
to the typed document's graph (`href`, containment and the properties) and finds its strongly connected components with
Tarjan's algorithm, without recursion. A component that holds a reference of a property is a cycle: every element in it
is reported once, and the reference that closes it is read as cut. A cycle that exists only inside a `use` clone is the
lowering's, which holds the stack of the elements it is instantiating.

**C11 Limits.** `max_style_bytes` (a longer `style` attribute is dropped and reported), `max_declarations` (a list with
more is dropped and reported), `max_custom_expansion` (a value that expands past it is invalid at computed-value time,
and reported), `max_gradient_stops` (a gradient with more stops is refused with `limit`). The engine's other bounds are its defaults.

**C12 The accessor.** `gvec_svg_get()` is one generic read of a typed value over the generated enumeration, and
`gvec_svg_get_specified()` the computed value as text with where the cascade got it. A value is owned by the context.

**C13 The dump** of an instance's style is the engine's canonical text; the hash covers every element of the document in
its own context. The document's dump is independent of styles and does not change when the cascade does.

**The oracle of style.** Two browsers are the independent readers. `tools/doc-oracle/style_dump.c` writes the used value and
the computed value of every property of every element with an id of a fixture (`tests/data/svg-style/`), and the same
properties are asked of a pinned Chromium and a pinned Firefox (`tools/oracle/containers/browser`) for the same document,
compared as typed values. A browser does not expose the style of the clone a `use` makes, so the fixture is first rewritten
with every `use` replaced by a group that carries its attributes and holds a copy of its referent, which is what SVG 2
defines a `use` to be; the rewriting shares no code with the library. Every difference is classified with the sentence of the
text that decides it, and a difference of class `ours` is refused. The lengths, percentages, colors and rotations are also
checked against vectors computed outside the library in exact arithmetic (`tools/gen-style-vectors.py`): a length is within
one unit in the last place of the exact conversion, a percentage of an axis within one, of the diagonal within two.

**C14 The engine is shared.** Building the registry and the initial values costs milliseconds, about forty times what
loading an icon costs, and depends only on the viewport, the color scheme and the bound of `var()` expansion. Contexts made
with the default allocator share engines kept for each environment (eight at most), which are immutable and safe on any
thread; `gvec_svg_style_cache_clear()` releases them. A context with another allocator builds its own engine through it.

## The lowering (P1 to P10)

`lower.h` is the public face; `src/codec/svg/lower/` holds the walk and the two emitters. The lowering reads the typed
document and the used values of the style layer and nothing else: no XML and no CSS text, and `check-front-end` fails if
an object of the lowering references the `text` library or the loader.

**P1 One walk, two emitters.** The walk (`lower_walk.c`) visits every element in the context it is drawn in and builds a
drawing tree: fills, opacity groups, clips, and `use` nodes that hold a picture's content. It owns every decision: what is
drawn, what is refused and why, the maps, the paints and the paths. The list emitter records the tree in a `GSHSCENE_List`;
the edit emitter makes keyed nodes of it. An emitter decides nothing, so a decision cannot be made twice, once for each
form. Every map is composed once and normalised (composed with the identity, which is what a retained scene does to the
groups above an item), so the recorded list and the list a published version emits are equal in every bit.

**P2 The equality gate.** For every document the list a published edit emits equals the list recorded directly
(`gshscene_list_equal`, and the same hash), and the two render to the same raster. The unit tests check the first on every
document they lower; `make check-lower-gate` checks both on the fixtures of `tests/data/svg-lower`, and against the golden
file of each (`tests/golden/lower`: the hashes, the diagnostics and the list's dump, which `make update-lower-goldens`
rewrites for review). The load fuzzer lowers every document it loads both ways and requires the same, and `fuzz_lower` does it
under each structural limit of the scene shrunk by its input, then renders with a sink that refuses most fills and maps every
entry of the report back through the trace.

**P3 The root and the viewports.** The outermost `svg` is laid out in `GVEC_Load_Options.viewport`: a missing `width` or
`height` is 100% of it. A `viewBox` and `preserveAspectRatio` make the map from the box to the viewport (all nineteen forms:
nine alignments with `meet` and with `slice`, and `none`). The scale is the correctly rounded quotient of the viewport's size
and the box's; the translation is the exact value of `a * size - scale * (origin + a * box)` (`a` being 0, 1/2 or 1) summed
as a non-overlapping expansion and rounded once, so no fused multiply-add is needed and the result is the same on every target.
`tools/gen-lower-vectors.py` computes the same entries in rational arithmetic from the decimal text of the numbers, and the
test reads them: they are an independent reference. A viewport or a `viewBox` of zero size disables the rendering of its
element (`GVEC_DIAG_RENDERING_DISABLED`, a notice, not a refusal); a negative `viewBox` is an invalid attribute and is ignored.

**P4 Nested viewports.** A nested `svg`, and a `symbol` or `svg` drawn by a `use`, is a viewport at `x`, `y`, `width` and
`height` (the `use`'s own size overrides the referent's). `overflow` decides the clip: `visible` and `auto` do not clip, any
other value clips to the viewport rectangle, which is a four-line path (exact for an axis-aligned rectangle) placed in the
parent's space, outside the viewBox map.

**P5 Composition without groups.** `transform` composes into the map each item carries. A group exists for `opacity` below 1
over painted content (it carries the opacity) and for a viewport's clip; a plain `g`, a `use` with only a transform and a
nested `svg` with only a viewBox make none and cost no node. `fill-opacity` is the alpha of the paint and is no group.
`opacity` over nothing painted makes no group, and `opacity: 0` draws nothing.

**P6 What is drawn, and what is refused.** `display: none` removes an element and its subtree (a `use` of it draws
nothing); `visibility: hidden` removes an element's own painting and not its descendants' that are `visible`; a `switch`
draws the child the loader selected. Elements that are never rendered by definition (`defs`, descriptive elements, paint
servers, markers, clip paths, masks and filters as definitions, `style`, `script`, animation, unknown and foreign elements)
draw nothing and are no refusals. An element that is drawn and uses what is not lowered yet is **refused whole**: one
`GVEC_DIAG_LOWER_REFUSED` with its name, line, column and a reason (`refusals.md`), and none of it drawn, so the report and
the picture say the same thing. A rectangle, circle, ellipse or arc is refused, not approximated: they wait for the shape
family's exact primitives.

**P7 Solid fills.** `path`, `polygon` and `polyline` become a model path (move, line, quadratic and cubic, close), interned
by content; a `line` has no area and draws nothing. A path whose segments have no length draws nothing. Path data that ends
in an error draws its valid prefix. The paint is straight `double` components in their color space with the alpha multiplied
by `fill-opacity`; `currentColor` is the waist's foreground until the document sets a `color` (`GVEC_Lower_Options` can set the
foreground of the list or the edit); `fill: none` and an alpha of zero emit nothing.

**P8 `use` and `symbol`.** A `use` draws its referent in the clone's context into a picture (a list of its own, interned by
content, so equal drawings share one) and is one instance with the map `transform` then `translate(x, y)`. In an edit the
picture is a hidden group beside the instance node that draws it. A `use` that is on a cycle, that expands past the limit or
that names a document that is never fetched is refused with its own reason; one that names nothing draws nothing.

**P9 Keys, and the trace.** An edit's nodes are keyed in one domain of this library, by the element's key, the chain of `use`
elements above it and the node's role, so a lowering again finds its nodes: it sets each to the properties it has (which
changes nothing), adds the new ones, moves the ones that are out of place and removes the ones the document no longer has.
All of them are under one group of the library's, placed under `GVEC_Lower_Options.parent`, so that removing what is stale
cannot reach a node of the caller's. A `GVEC_Lower_Trace` records which element every item came from, so an entry of a sink's
report prints as `line 40, path: ...`: for a list through the chain of indexes (an instance and then an item of its picture),
for an edit through the version, the node and its key. An entry it cannot place is returned as unplaced, not dropped.

**P10 The scene's limits.** The walk holds the drawing to the structural limits of the scene it is told of
(`GVEC_Lower_Options.scene_limits`, else the defaults): the levels of nodes, the clips and groups open in one list, the
pictures nested in pictures, the items of a list, the items once every instance is expanded and the nodes of a version. An
element that does not fit is refused whole with the reason `limit`, in both forms, so a hostile document cannot make the
caller's publish fail. The walk's model of each is tested against the scene's own: the deepest drawing it accepts is the
deepest the scene publishes. (A retained scene counts the occurrences of its nodes against `max_expansion`, a use being its
instance and the target it draws, and a list counts items, a group being a push and a pop; the walk checks both.) The depth of the
walk itself is bounded by `max_depth`.

**K1 Strokes.** A stroke is an item of the shape waist beside the fill: this library names the style (`stroke-width`,
`stroke-linecap`, `stroke-linejoin`, `stroke-miterlimit`, `stroke-dasharray`, `stroke-dashoffset`) and the paint, and the
shape family outlines, dashes and joins. Nothing here approximates a join or a dash. A width, a dash or an offset outside
the range the waist carries refuses the element with `limit`; a miter limit above the waist's bound is clamped to it, the edge
a join that sharp cannot be told from a bevel. `miter-clip` and `arcs` are joins of the waist. A stroke that cannot be seen
(a width or an opacity of zero, or `none`) draws nothing.

**K2 A pair, whole.** A shape's fill and stroke are appended together, both or neither, under the one opacity group of the
element: a refusal of either (a gradient, a paint outside the waist's colors, a value past a limit) refuses the element and
nothing of it is half drawn. A `polyline` is stroked open and filled closed; a `polygon` is closed for both; a `line` has a
stroke and no area; a subpath of no length is kept for a stroke, whose cap draws it.

**K3 Paint.** A stroke's paint is read as a fill's, with `stroke-opacity` as the alpha. `context-fill` and `context-stroke`
are the paints of the element a marker is drawn for (K8) or of the `use` whose referent they are in (P8); anywhere else they are refused with `context-paint`.

**K4 `vector-effect`.** `non-scaling-stroke` is the waist's device width space. Any other value is refused with
`vector-effect`: the others change what the picture is, not how a stroke is drawn.

**K5 `paint-order`.** `normal`, or the keywords `fill`, `stroke` and `markers` in the order listed with the ones left out
following in that order, decide the order of the fill, the stroke and the marker instances within the element, in the list and
in the edit's child order.

**K6 The geometry of a stroke and of a marker are the same path.** The centerline is the element's path as written (a polyline
open, a polygon closed); the vertices markers go on are read from it.

**K7 Marker positions.** `marker-start`, `marker-mid` and `marker-end` (and the `marker` shorthand) apply to `path`, `line`,
`polyline` and `polygon` and to no other shape. The start marker is at the first vertex of the whole path, the end marker at the
last, and a mid marker at every other vertex, including the starts and the ends of the other subpaths; the vertex of a close is
a vertex. The vertices, and the direction at each (the incoming and the outgoing, the bisector, and the rule for a segment of no
length and for the corner at the start of a closed subpath), are the geometry library's: this library computes no tangent. A
reference to nothing draws no marker, and a reference to an element that is no marker draws none and is said.

By default a closed path's first and last vertex take the direction the geometry library gives, the bisector of the segment before and
the one after. SVG 2 (13.7.4) does not say whether the closing segment is the one before the start, and says nothing of a reversal or a
segment of no length, and the renderers differ: the browsers bisect; `resvg`, Batik and Inkscape point the start marker along the first
segment and the end marker along the last. `GVEC_LOWER_MARKER_CLOSED_ENDS_OPEN` selects the second, taking the incoming and outgoing
directions of the vertex from the same library; the vertices in the middle of a path are unchanged. The conformance runner sets it,
because the reference images are `resvg`'s.

**K8 The marker, and where it goes.** A marker is a viewport of `markerWidth` by `markerHeight` (3 by default), with a `viewBox`
and `preserveAspectRatio` as any other, and its content is **a picture drawn once for each vertex** under the map of the shape
(its `transform`), the vertex, the rotation, the scale and the reference point. `orient` is an angle in degrees, `auto` (the
vertex's direction, built from the unit direction itself, so the quarter turns are exact), or `auto-start-reverse` (`auto` turned
half way round at the start). `markerUnits` is `strokeWidth` (the used `stroke-width` of the shape, whether or not it is
painted; a width of zero draws no marker) or `userSpaceOnUse`. `refX` and `refY` are in the content's coordinates; their
keywords are 0%, 50% and 100% of the viewBox from its origin (of the viewport without one). `overflow` other than `visible`
and `auto` clips the content to the marker's viewport, a rectangle in the picture's own coordinates. A marker of no size draws
nothing and is said. The content inherits from where the marker is in the document (the instance it is drawn in is made from the
root down through the marker's ancestors) and not from the shape, and the percentages in it are of the marker's viewport.

**K9 Context paint, and the bound.** Inside a marker `context-fill` and `context-stroke` are the paints the shape has, so a
marker used by two shapes with different paints is two pictures, and by two with the same paints, one. The instances are counted
over the whole lowering, those of markers inside markers too, against `GVEC_Lower_Options.max_marker_instances` (by default and
at most half the nodes a version of the scene may hold); the element that would pass it is refused whole with `limit`. A picture
nested past the scene's depth is refused where it is. A marker whose content refers back to it is cut by the document's
reference graph, which says so when it loads: nothing recurses.

**K10 Which shapes have markers.** Every shape does, a `rect`, a `circle` and an `ellipse` included (SVG 2, 13.7: markers are painted along "any shape element"); the vertices are those of the shape's path, so the start and end markers of a closed shape are at its first vertex. A marker on a path with an
elliptical arc is placed like any other: an arc is one segment that ends one vertex, at its authoritative end.

**X1 Arcs.** Path data holds an arc in endpoint form and the lowering gives it, as written, to the geometry library's
endpoint constructor with the current point, the end, the radii, the rotation in degrees and the two flags. The library owns
SVG 2's radius correction (a zero radius is a line, a negative radius its absolute value, radii too small scaled up) and the
degenerate cases (equal ends add nothing). Nothing in this library computes a centre, an angle or a conic, and a path with
arcs stays a path with arcs all the way to the sink. The reference vectors of the tests are made by a script that shares no
code with either library (`tools/lower/gen-arc-vectors.py`, 80-digit decimal arithmetic).

**X2 `rect`.** The style layer gives `x`, `y`, `width`, `height`, `rx` and `ry` as used lengths in user units (a percentage
is already of its basis). A width or height that is zero, negative or `auto` is not rendered. `rx` and `ry` take each
other's value when one is `auto` (a negative value is an error and is dropped by the style layer, so it is `auto`); both
`auto` is square; each is then clamped to half its side; a zero radius is a plain rectangle. The waist is handed the resolved
radii, equal in the four corners, because the rule that clamps each radius alone is SVG's, not the waist's.

**X3 `circle` and `ellipse`.** `r` (or `rx` and `ry`, each taking the other's value when `auto`) of zero or less is not
rendered; otherwise the ellipse primitive, which starts at three o'clock and turns the positive way, as SVG 2 says, so dashes
and markers begin where a browser's do. A circle is an ellipse of equal radii.

**X4 A shape is its tag.** The path of a `rect`, `circle` or `ellipse` is interned through the waist's primitive constructor,
so the resource carries the tag a document writer or an analytic sink reads; the renderer ignores it and draws the path. A
stroke of one of them is a stroke of the tagged path.

**H1 Gradients.** A `linearGradient` or `radialGradient` paint is one gradient resource of the waist (interned, so equal content is
one handle however many shapes it paints), with a map from gradient space to the element's own space. With `objectBoundingBox`
units, the default, the map is the box of the element's fill geometry (the stroke left out, from the geometry library's bounds;
a line's is its extent) composed with `gradientTransform`, and a coordinate is a fraction of the box; with `userSpaceOnUse` the
map is `gradientTransform` and a percentage is of the viewport (of its normalized diagonal for a radius). A box with no width or no
height takes the paint after the `url()` when the document gave one (a color, or `none`), and is `none` with a notice when it did not; a
`gradientTransform` with no inverse and a negative radius make the paint `none` with a notice, as SVG 2 says a paint server
that cannot be rendered is not. `spreadMethod` is the waist's extend mode.

**H2 Stops and inheritance.** The stops are the `stop` children of the first gradient of an `href` chain that has any; each
offset is clamped to `[0, 1]` and never goes below the one before it. A stop's color is its `stop-color` with `stop-opacity`
multiplied in, and `currentColor` is the `color` of the stop as the cascade gives it (the waist's foreground when none is set). No
stops is `none`; one stop, a radius of zero and a gradient with both ends at one point are the last stop's color as a solid paint.
The attributes the two kinds share (`gradientUnits`, `gradientTransform`, `spreadMethod`) come from the first gradient of the chain
that has them, and the geometry (`x1`, `cx`, `fr` and the rest) from the first of the same kind; a chain that comes back to itself is
followed until it repeats, through `gvec_svg_element_href_target_uncut()`, because the document cuts the edges of a cycle and a
gradient on one still has what the chain gave it before it returned. What of a gradient does not depend on the element painted
with it (its stops, transform, units and spread) is read once.

**H3 Radial, and what is not lowered.** A radial gradient is the waist's two circles, the focal point and `fr` the start and the
center and `r` the end, with the focal point defaulting to the center; where they do not contain one another the waist's two-circle
rule decides, not a clamp in this library. A gradient with `color-interpolation: linearRGB` names linear-light sRGB as the waist's interpolation space; its stops stay the
document's sRGB colors and the waist converts each, so the ramp between them is walked in linear light (other renderers ignore the
property on a gradient and walk sRGB; this follows the property). A gradient named by `context-fill` or `context-stroke` is placed in the box
of the context element, not the element drawn (H13). Colors are
interpolated straight, as the waist's `premultiplied` field says. A gradient with more stops than `max_gradient_stops` (65,535,
the most the scene carries) is refused with `limit`; the option can lower it.

**H9 Clips.** `clip-path` naming a `clipPath` is a clip of the waist around what the element draws (never an alpha mask): one clip
push, then the element's fill, stroke, markers and opacity group inside it, then the pop. The clip's map is the element's own space
(its frame and its `transform`), then the `clipPath`'s `transform`, then, for `clipPathUnits="objectBoundingBox"`, the box of the
element's fill geometry (a group has no box here and is refused; a box with no width or height gives nothing to place the clip in,
so the element is not rendered), then the child's `transform`. A shape of `display: none`, `visibility: hidden` or no area adds
nothing to a clip, and a clip of nothing lets nothing through. `clip-rule` is the child's own. A reference to nothing, or to what is
no `clipPath`, draws the element unclipped.

**H10 Clips that are clipped.** A `clip-path` on the `clipPath`, or on its one shape, is an intersection: a nested push, outermost
first. A chain of references that comes back to a `clipPath` it has already made is followed until it repeats, through the same
uncut reference as a gradient's, and stops there.

**H11 Several shapes.** The shapes of a `clipPath` with more than one that draws are one union clip of the waist: its members are
the shapes' paths, each with its own `transform` and `clip-rule`, and the clip's map places them as a whole, as H9 says. The region
is the union of the members, quantized once, so abutting shapes leave no seam; overlapping members are the sink's work. A
`clip-path` on one shape of several clips that shape alone, which an intersection around the union cannot say, so it is refused,
as is text in a `clipPath`, with `text`. A `use` child that names a shape is that shape, placed by the use's `transform` and its `x` and
`y` (the shape's own `transform` comes after), and one that names anything else (a group, a `symbol`, nothing) is not a valid child and adds
nothing, so a clip of only those lets nothing through; a `use` that has a `clip-path` of its own is refused.

**H12 Basic shapes.** `clip-path: inset() | circle() | ellipse() | polygon() | path()`, with an optional geometry box, on an element
is one clip region in that element's space (its `transform` moves it). The box is the fill box, the stroke box (the default, the
fill box widened by half the stroke width; refused for geometry that is not a rectangle, circle or ellipse, whose stroke bounds are
not exact), or the view box; an element that is not a shape, or a view box whose origin is not the viewport's, is refused. A
circle with no radius is `closest-side` of the fill box and a percentage is of the normalized diagonal, as CSS Shapes says. `inset()`
radii that overlap on a side are all scaled by one factor, as CSS Backgrounds does. A shape with no area lets nothing through. A
basic shape on a `clipPath`'s shape is still refused.

**H13 Bounding boxes of groups, and context gradients.** A `clipPath` in `objectBoundingBox` units on a group is placed in the box of what
the group draws: the union of the fill boxes of the shapes in it (and in the `g` and `a` inside it), each moved by its transforms; a
rotated or skewed shape is boxed by its segments, one with an arc by its box, `display` takes a shape out and `visibility` and `opacity`
do not. A group that holds a `use`, text, an image or a nested viewport is refused with `clip-path`, as is a transform that needs a box of
its own. A gradient that `context-fill` or `context-stroke` names is placed in the box of the context element, in that element's user
space: for a marker, the shape the marker is on (the picture's placement and the drawn shape's own map are undone), for a `use`, what its
referent draws, moved by `x` and `y`. A paint a shape takes from an outer context stays refused, and so does a pattern one.

The same box sizes a document that gives no size. With no `viewBox`, a missing `width` is the right edge of the drawing and a missing `height`
its bottom edge, from the origin, with each stroke reaching half its width past its shape (a join or cap that reaches further is not counted):
that is what `resvg` does and what the reference images show. A document that holds anything the box does not take in (a `use`, text, an image)
or draws nothing is the load's viewport, as before. A negative size is not treated this way (H16).

**H14 Shape rendering.** `shape-rendering` of `crispEdges` or `optimizeSpeed` asks the fill and the stroke of a shape for no antialiasing:
each item is recorded with the waist's no-antialiasing mode, so a pixel is covered whole when its center is inside the shape and not at all
otherwise. `auto` and `geometricPrecision` leave the default. The property is inherited, so it is read on each shape, and a group has none
of its own. A sink that cannot draw the mode draws the item antialiased and the plan reports it approximated.

**H15 A gradient with no stops.** Its paint is `none` (SVG 2, 13.2.3), and the paint after the `url()` is not used: the fallback is for a
reference that cannot be resolved. `resvg` and Batik use it, the browsers do not, so `GVEC_LOWER_GRADIENT_NO_STOPS_USES_FALLBACK` draws the
fallback, when the paint has one. A paint with no fallback is `none` either way.

**H16 A negative size on the root.** A negative `width` or `height` of the root `svg` is an error and nothing is drawn, with the notice that
rendering is disabled, as for a size of zero. SVG 1.1 says it is an error, SVG 2 is silent on a negative one, and the browsers and `resvg`
all draw nothing, so there is no flag. A negative size on a nested `svg` or a shape is as it was.

**H17 Patterns.** A `pattern` paint on a fill or a stroke is the waist's pattern resource. The cell is `x`, `y`, `width` and `height` (read through the
`href` chain, as are `patternUnits`, `patternContentUnits`, `patternTransform`, `viewBox` and `preserveAspectRatio`), the steps are the cell's size, both
axes repeat, and `patternTransform` is the map from pattern space to the user space of the element painted. With `patternUnits` of `objectBoundingBox`
(the default) the cell is a fraction of the element's box, a percentage being that over 100; in `userSpaceOnUse` a percentage is of the viewport. The
tile is a picture of the content of the first pattern in the chain that has a child element, walked as the content of a picture, under the move to the
cell's corner and then the viewBox's map into the cell, or the scale of the box when `patternContentUnits` is `objectBoundingBox` and there is no viewBox. A
pattern of no width or height, of a transform with no inverse, or of a box with no area (with no fallback) is not rendered, with a notice, and paints
nothing; a pattern with no content paints nothing and no notice. A pattern whose content paints with the pattern itself, at any depth, is a reference cycle: the style layer cuts the reference that closes it (a notice) and
the paint reads as its fallback or none; the lowering also refuses to nest a pattern in its own tile, and patterns nest to eight deep and to the scene's picture depth. `overflow` of `visible` or `auto` asks the scene for the unclipped tile (SVG 2),
which no reference engine draws; no case of the suite has content outside its cell, so none tells the two apart. A `context-fill` or `context-stroke` that names a pattern is the pattern of the context element, placed in that element's box (as a gradient is). The pattern's items are not traced: a refusal inside a tile is reported for the element that has it.

**H18 Style sheets.** The `style` elements of the document (of type `text/css` or of none, in document order, the text and CDATA of each) are parsed and
compiled by `text` and held, once for the document, in one frozen rule index; the rules are matched against the document's own tree through the element
interface (the parent, siblings, `id`, the words of `class`, attributes, order), so a clone made by `use` is matched as the element it clones, and
`defs > path` matches the path whichever way it is drawn. The cascade is `text`'s: a rule beats a presentation attribute, the `style` attribute beats a
rule, and importance, specificity and order of appearance decide between rules. A rule or declaration that is not valid is dropped and reported as a
warning at its `style` element. A sheet that cannot be applied whole is not applied, and the lowering refuses its `style` element (GVEC_LOWER_STYLESHEET)
so that the document is not drawn half styled: an `@import` (the library never fetches) and a conditional rule (`@media`, `@supports`, `@container`,
`@when`), for which this layer has no evaluator of the environment. Geometry given in style (`height: 160px` on a `rect`) is read as SVG 2 reads it.

**H19 Masks.** A `mask` naming a `mask` element is a group of the waist with a soft mask. The mask's picture is the `mask` element's content drawn in the
masked element's own user space (its frame and its `transform`), under a clip to the region `x`, `y`, `width` and `height` (default `-10%`, `-10%`,
`120%`, `120%`, of the box with `maskUnits` of `objectBoundingBox`, the default, or of the viewport in user units), or in the box's units when
`maskContentUnits` is `objectBoundingBox`. The mode is luminance, or alpha when `mask-type` says so, and `color-interpolation: linearRGB` is the space
the luminance is taken in while the picture is blended as it is, which is what the reference engines do. A box with no area, a region of no size and a
mask with nothing in it hide the element. A `mask` on the `mask` element is a mask of its picture; if the `mask` properties followed from a mask come back
to it, that mask has no mask of its own (as in the browsers and `resvg`), and a child of a mask that names a mask being made is drawn unmasked. A `mask`
naming nothing, or what is no `mask`, is as if it were not specified. A mask in the units of the box of an element that holds more than shapes and groups
of them, and a mask of another document, are refused (GVEC_LOWER_MASK). Equal masks of equal boxes are one resource.

**H20 Blend modes and isolation.** `mix-blend-mode` (read in a style, never as a presentation attribute) is the composite operator of the group the
element is drawn in: one group carries the element's opacity and its blend, so the element is blended as a whole (fill, stroke and markers together),
and where the element is masked the mask's group carries the blend, because a mask is an isolated group and the element would blend with nothing
inside it. `normal` and a value that is not one of the fifteen modes make no group. A group that is not a stacking context (a `g` with no opacity,
mask, blend or isolation) makes no group, so its children blend with what was painted before it; `isolation: isolate` makes one, with opacity 1 and
the operator source-over. The waist drops a group node that is all defaults (it is a container, and a list would push a layer for it where an edit
would not), so the isolated group names the space it blends in, sRGB-encoded as CSS blends, which keeps it a group in both.

**H21 Text: fonts, chunks and glyph runs.** The caller gives the fonts (`GVEC_Fonts` over a `GFNT_FontSet`; the library scans nothing and reads no
environment, so a drawing of text is a function of the document and the fonts). The content of a `text` element is read in tree order into
characters: a text node's, with the white space of `xml:space` (stripped at the ends, newlines removed, runs collapsed, the collapse running on
across spans and after a preserved space), and each `tspan` and `a` (a span that is not displayed, or whose conditions fail, adds nothing). Each
character knows the span it is in, and the `x`, `y`, `dx` and `dy` lists of the elements apply to the characters of their own content in order,
an inner list after an outer one so that it wins. A character with an absolute `x` or `y` begins a **text chunk** (as does the start of the text),
at that position, the other coordinate continuing from the pen. A chunk is shaped in stretches of consecutive characters that ask for one font,
whatever their fill, so that kerning and joining run across a span boundary that changes only the paint; the shaper is `gfnt_faces_shape_bidi`
over the faces the stretch's characters need: for each character, the first name of the `font-family` list that has a face mapping it (the generic
families are the set's aliases), else the set's `serif`, else any face that maps it, in the weight, style and width the cascade gives. A glyph moves
the pen by its advance; `dx` and `dy` of its character move the pen before it, and stay. The whole chunk is then moved by its `text-anchor` (the
first character's) times its whole advance. A glyph run of the waist has one face and one size, so the glyphs are cut into runs where the span
(its paint) or the face changes, each filled with the paint of its span; spans that are not visible keep their room and draw nothing. The runs are
unhinted (the references draw outlines), the size is `font-size` in user units, and the glyphs carry their absolute positions in the element's
user space. The face is interned in the scene through `font`'s glyph provider, so the `GVEC_Fonts` must outlive the scene. Whatever a later story
of milestone 3 lowers (`rotate`, a writing mode, a direction, `font-size-adjust`, text on a path, `tref`) refuses the element whole.

**H22 Paint on text.** Each run is filled and stroked with the paint of the span it is of, in that span's `paint-order` (the scene strokes a glyph run's
centerline, so a stroke is an item over the same run); the opacity of a span other than the text element is a group around that span's runs; and
a gradient or a pattern in the units of the bounding box is placed in the box of the whole text, whichever span is painted: the union of its
glyphs' cells, each an advance wide and the face's ascent and descent high (the references measure a text's height by the font, not by the
outlines). `text-rendering: optimizeSpeed` on the text element asks for no antialiasing; `shape-rendering` does not affect text, and
`text-rendering` on a span is ignored.

**H23 Spacing and adjustment.** After a character (the last glyph of its cluster) the pen moves by the span's `letter-spacing`, except for a
character of a cursive script (Arabic and its neighbors), which is not spaced; after a space it moves by `word-spacing` too. A percentage is of the
viewport's width (the references take it so). Letters that are spaced apart are shaped with `liga` and `clig` off. `font-kerning: none` (read from a
style only, and reset by a `font` shorthand nearer the text) and `kerning="0"` turn `kern` off; `font-variant: small-caps` turns `smcp` on, so the
small capitals are the face's. A `textLength` is applied to the glyphs of its element's characters in one chunk, after the chunk is laid out and before
it is anchored: `spacing` spreads the extra length over the gaps between the glyphs and moves what follows; `spacingAndGlyphs` leaves the positions
and stretches the run's map about the start of the element's glyphs, so the outlines are stretched with them. A length of zero or less is ignored,
and one over several chunks is refused.

**H24 Baselines.** A span is set on the baseline its `dominant-baseline` names, or the one its `alignment-baseline` aligns it by, and lifted by
its `baseline-shift`s and its ancestors'. The positions of the baselines come from the face: `central` is half the sum of the ascent and the
descent, `middle` half the x-height, `hanging` four fifths of the ascent, `mathematical` half of it, `ideographic` and `after-edge` the
descent, `before-edge` the ascent. `sub` and `super` lift by the face's OS/2 offsets, and by a fifth and a third of the font size when it has
none. The shift of the `text` element itself is ignored; the values of SVG 1.1 that the cascade drops are read from the attribute.

**H25 Rotation and decoration.** A `rotate` list turns each character about the origin of its glyph; a mark turns about the base of its
cluster, so that it keeps its place on it. `text-decoration` is drawn for each span from the face of the element that declared it: an
underline at the face's underline position, a line-through at its strikeout position, an overline at the ascent, each as wide as the span's
glyphs and as thick as the face says. An element outside the text that declares one draws it with the paint of the text. A decoration over
glyphs that are rotated, or whose baseline moves with `dy` or `y`, is refused.

**H26 Direction.** `direction` is the base direction of the paragraph the bidirectional algorithm is run over, so right-to-left text is
shaped, joined and reordered as the font library does it; the glyphs of a chunk are laid from its position in visual order. `start` and
`end` of `text-anchor` are those of the line, so a right-to-left chunk that starts at `x` ends there. `unicode-bidi: bidi-override` gives
every character the direction of `direction` and no reordering: the text is cut into its scripts, each is shaped in that direction, and the
stretches are laid in that direction's order. `unicode-bidi: embed` and `isolate` change nothing a single line shows.


The registry is read-only after load time and may be read from any thread. Every other object (a
stream, a document, a diagnostics list) is used by one thread at a time; different objects may be
used on different threads at once.

## Gates

| Gate | What it refuses |
| --- | --- |
| `check-fp-contract` | a compile-flag set without `-ffp-contract=off`, or with fast math |
| `check-libm` | any reference to or definition of a libm function |
| `check-no-x87` | an x87 instruction in the objects |
| `check-edges` | an include or a linked library outside `cutil`, `compress`, `text`, `shape-scene`, `shape-geometry`, `color`, `math`; so no renderer |
| `check-front-end` | an include of `shape-scene/scene.h`; a reference to `getenv`, `setlocale`, `localeconv`, a clock, or a C-library decimal reader; an object of the lowering that references the `text` library or the loader |
| `check-gates` | runs each gate above on a control, a planted violation and an empty population |
| `check-test-data` | a fixture, a fuzz seed, a vector, a reason table or a namespace list that is not what its generator writes |
| `fuzz-replay` | a seed that crashes or breaks an invariant (the load, style and lowering fuzzers), and a corpus that leaves an outcome unreached |
| `check-planted` | a planted defect in an L-rule, a T-rule, a C-rule or a P-rule that no test catches |
| `check-lower-gate` | a fixture whose recorded list and published edit's list differ or render differently, or that differs from its golden file |
| `check-tables` | committed element and attribute tables that are not what the generator writes from the pinned pages; a generator that does not fail on its plants |
| `check-doc-oracle` | a dump that disagrees with an independent reader of the same documents (values, references, cycles, expansion counts, conditions), or a comparison that does not fail on a planted difference |
| `check-style-oracle` | a computed style that disagrees with Chromium and Firefox in a way `tools/doc-oracle/style_divergences.txt` does not classify (with the sentence that decides it), a classified difference that no longer occurs, or a comparison that does not fail on a planted difference; the browsers are a pinned image (`make oracle-images`), and without it the check is skipped, counted, and fails under `GHOTI_ORACLE_REQUIRED=1` |
| `check-conformance` | a scorer, runner or oracle wrapper that gives a wrong outcome on a synthetic corpus (see `conformance.md`) |
| `check-conformance-planted` | a defect planted in the runner, the scorer or the oracle wrapper that the check above does not catch |

`check-front-end` reads the archive's undefined symbols, not the source, so a call reached through a
macro or an inline wrapper is seen. The decimal conversions come from `math` because a C-library
reader depends on the locale.

## Entity bombs

A document that declares nested entities can ask for far more text than it contains. Four bounds stop it, each named
in the diagnostic: `max_entity_expanded_bytes` (the total replacement text), `max_entity_depth`,
`max_entity_references`, and `max_entity_amplification`, the factor by which the expanded text may exceed the input
once it is past 8 MiB. A billion laughs under the defaults is refused at the amplification bound in a few
milliseconds, and the tests run the bombs and a 1 MiB internal subset under the default caps.

## Coverage

`make coverage` measures line coverage of `src/` with the unit tests and the fuzz replay: 88.5% of 6,178 lines (GCC 16). The
lines not reached are mostly allocation-failure arms that the failing-allocator sweeps reach only for the documents they
use (the lowering's are swept for every allocation of its own, and for a spread of the style engine's), and the file-read
error arms of the stream. A 600 s libFuzzer run of each of the three targets (load, style, lowering; 9,324, 19,463 and
9,334 executions, the load and lowering fuzzers holding `max_reference_expansion` to 3,000 because a style is computed for
every element a `use` instantiates) found nothing.
