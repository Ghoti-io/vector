# Design

**Status:** the library, the format registry and loading SVG are built (rules L1 to L11 below).
The conformance harness scores the library against a published corpus (`conformance.md`). The typed SVG document, its attribute grammars, the cascade and the lowering are not built; the lowering
entry points exist and refuse every document at its root.

`vector` reads vector formats and lowers them into `shape-scene`'s two recording forms, an edit of
the retained scene and an immediate-mode list. It is a *front end* of the shape family: it never
renders, never publishes a scene and never links a renderer, so that the scene it produces serves
every sink alike. The pipeline is

```
bytes ─transport─▶ format bytes ─text─▶ XML tree ─build─▶ typed document ─lower─▶ edit or list
       (gzip)                    (XML)             (vector)                (vector)
```

Each stage is separate. The lowering reads no XML and makes no parsing decision; the build pass
turns the tree into a typed document and then frees it. Only the first two stages exist yet.

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
| `check-test-data` | a fixture or a fuzz seed that is not what `tools/gen-test-data.py` writes |
| `fuzz-replay` | a seed that crashes or breaks an invariant, and a corpus that leaves an outcome unreached |
| `check-planted` | a planted defect in an L-rule that no test catches |
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

`make coverage` measures line coverage of `src/`: 91.5% of 870 lines (GCC 16, the unit tests and the
fuzz replay). By file: `svg_load.c` 85.5%, `stream.c` 88.4%, `transport.c` 91.3%, `codec.c` 93.0%,
`core.c` 96.4%, `svg_codec.c` 98.3%. The lines not reached are allocation-failure arms in code that
runs after the sweep's last failing allocation, and the file-read error arms.
