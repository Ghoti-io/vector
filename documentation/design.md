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

**L11 The lowering entry points.** `gvec_doc_lower_edit` and `gvec_doc_lower_list` have their final
signatures, so the conformance harness scores against the API it will keep. Nothing is lowered yet:
each emits nothing, returns `GVEC_OK`, and appends one `GVEC_DIAG_LOWER_REFUSED` entry, reason
`GVEC_LOWER_NOT_YET_LOWERED`, for the root `svg` with its line. A refusal per document is what an
unsupported feature does, so this is a correct, if empty, lowering.

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

## Threads

The registry is read-only after load time and may be read from any thread. Every other object (a
stream, a document, a diagnostics list) is used by one thread at a time; different objects may be
used on different threads at once.

## Gates

| Gate | What it refuses |
| --- | --- |
| `check-fp-contract` | a compile-flag set without `-ffp-contract=off`, or with fast math |
| `check-libm` | any reference to or definition of a libm function |
| `check-no-x87` | an x87 instruction in the objects |
| `check-edges` | an include or a linked library outside `cutil`, `compress`, `text`, `shape-scene`; so no renderer |
| `check-front-end` | an include of `shape-scene/scene.h`; a reference to `getenv`, `setlocale`, `localeconv`, a clock, or a C-library decimal reader |
| `check-gates` | runs each gate above on a control, a planted violation and an empty population |
| `check-test-data` | a fixture, a fuzz seed, a grammar vector or a namespace list that is not what its generator writes |
| `fuzz-replay` | a seed that crashes or breaks an invariant, and a corpus that leaves an outcome unreached |
| `check-planted` | a planted defect in an L-rule or a T-rule that no test catches |
| `check-tables` | committed element and attribute tables that are not what the generator writes from the pinned pages; a generator that does not fail on its plants |
| `check-doc-oracle` | a dump that disagrees with an independent reader of the same documents (values, references, cycles, expansion counts, conditions), or a comparison that does not fail on a planted difference |
| `check-conformance` | a scorer, runner or oracle wrapper that gives a wrong outcome on a synthetic corpus (see `conformance.md`) |
| `check-conformance-planted` | a defect planted in the runner, the scorer or the oracle wrapper that the check above does not catch |

`check-front-end` reads the archive's undefined symbols, not the source, so a call reached through a
macro or an inline wrapper is seen. The decimal conversions come from `math` because a C-library
reader depends on the locale.

## Known limits of the XML parser

Two defects in `text`'s DTD processing were found while building the load, and are reported there:
declaring entities in an internal subset is quadratic in their number (16,384 declarations, about
400 KB, exhaust the parser's work budget), and entity expansion is super-linear (300 KB of expansion
takes seconds), so under the default caps an entity bomb is bounded by time before it reaches
`max_entity_expanded_bytes`. Until `text` fixes them, the tests run the bombs with a small expansion
cap, the fuzz seeds that hold bombs carry one in their options byte, and the internal-subset test
uses 2,048 declarations. Those three are widened to the default caps when the fix lands.

## Coverage

`make coverage` measures line coverage of `src/` with the unit tests and the fuzz replay: 90.2% of 2,975 lines (GCC 16).
The lines not reached are mostly allocation-failure arms that the failing-allocator sweep reaches only for the
documents it uses, and the file-read error arms of the stream. A 600 s libFuzzer run with the grammar-level mutator
(1,273,653 executions) found nothing.
