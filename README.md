# Ghoti.io Vector

Vector formats, read into a document and lowered into the shape family's scene, so that one drawing
serves every renderer: the CPU rasterizer, GPU sinks, writers and toolpaths. SVG comes first. It is
part of the [Ghoti.io suite](https://github.com/Ghoti-io/suite).

```c
#include <ghoti.io/vector/vector.h>

GVEC_Stream * stream = NULL;
GVEC_Diagnostics diagnostics;
GVEC_Doc * doc = NULL;

gvec_stream_create_file("drawing.svg", &stream);
gvec_diagnostics_init(&diagnostics, NULL);
if (gvec_doc_load(stream, NULL, &diagnostics, &doc) == GVEC_OK) {
  /* gvec_doc_lower_list(doc, NULL, list, &diagnostics) fills a shape-scene list */
  gvec_doc_free(doc);
}
for (size_t i = 0; i < diagnostics.count; i++) {
  printf("line %zu: %s\n", diagnostics.items[i].line, diagnostics.items[i].message);
}
gvec_diagnostics_destroy(&diagnostics);
gvec_stream_destroy(stream);
```

`examples/load.c` is the same program, complete, and `make run-examples` runs it.

## What is implemented

This is the first milestone's first step: the library, and loading.

* **Recognising SVG.** A document is SVG when its first element is `svg` in the SVG namespace,
  however the file begins: a byte order mark, any of the encodings the XML parser reads (UTF-8,
  UTF-16 and UTF-32 in either byte order, ISO-8859-1, US-ASCII), comments, processing instructions,
  a doctype with an internal subset. The question is answered by the same XML parser that then
  reads the file, so the two cannot disagree. An `svg` element in no namespace is not SVG.
* **`.svgz`.** A gzip file is a transport, not a format: it is inflated, every member of it, within
  `GVEC_Limits.max_inflated_bytes`, and what is inside is recognised. Bytes after the last member are
  ignored with a warning; gzip inside gzip is refused.
* **Loading.** XML through `text`, with the DTD processed so that files declaring their own entities
  load, and nothing outside the file ever read: no resolver exists, so an external DTD or entity
  cannot be fetched. A reference to an external entity is kept in place and reported where it is
  used.
* **Diagnostics.** Every load reports what it found to a list the caller owns, on success and on
  failure, each entry with a stable code, the line and column, and the element. A file this library
  does not read says why: the XML error that stopped it, or the root element it found.
* **Limits.** Input bytes, inflated bytes, elements, depth, attributes, entity expansion, entity
  nesting, references and diagnostics are each capped, with finite defaults equal to the XML
  parser's own limits. Zero selects the default; nothing is unlimited.
* **Lowering.** `gvec_doc_lower_edit` and `gvec_doc_lower_list` have their final signatures. Nothing
  is lowered yet: each refuses the document at its root, with its line, so that a renderer and a
  conformance harness see a correct, if empty, drawing and the reason.
* **The typed document.** An SVG file is read into a tree of elements, one kind for every element of SVG 2, with
  each attribute parsed by its own grammar (numbers, lengths, lists, points, `viewBox`, path data in the form it
  was written and in absolute form, references, `data:` URIs, conditional processing), its errors located, its
  references and cycles resolved, and a key for every element that is stable across loads (`svg.h`). CSS
  properties and presentation attributes are held as written for the cascade, which comes next.
* **The dump.** `gvec_doc_dump` writes a line-oriented text that is the same on every target,
  and `gvec_doc_hash` is a hash of it.

## Building

```sh
make PREFIX=/your/prefix          # shared and static libraries
make test PREFIX=/your/prefix     # the tests and every gate
make install PREFIX=/your/prefix
```

Dependencies are found by pkg-config only: `cutil`, `compress`, `text` and `shape-scene`, each
installed into a prefix (the suite repository's `install.sh` builds them all in order).
`make help` lists every target.

## The gates

`make test` runs, beside the unit tests:

* `check-fp-contract`, `check-libm`, `check-no-x87`: no fused multiply-add, no libm, no x87, so a
  result is the same bits on every target.
* `check-edges`: this library includes and links only `cutil`, `compress`, `text` and `shape-scene`.
  No renderer is reachable from it; its tests and examples may link one.
* `check-front-end`: no include of `shape-scene/scene.h` (publishing a scene is the application's),
  and no reference to a function that reads the environment, the locale or the clock.
* `check-gates`: plants a violation for each gate above and requires it to fail.
* `check-test-data`: the fixtures and the fuzz seeds are exactly what `tools/gen-test-data.py`
  writes.
* `fuzz-replay`: every seed through the load fuzzer, whose invariants abort on a violation.
* `check-tables`, `check-doc-oracle`: the element and attribute tables are what the pinned specification pages
  generate, and the typed document agrees with an independent reader of the same documents.
* `check-conformance`, `check-conformance-planted`: the conformance harness is shown to produce each outcome
  and to reject a malformed case table, and a defect planted in it is caught.
* `run-examples`: every example, each checking what it shows.

## Conformance

`make conformance` scores the library against the resvg test suite (fetched at a pinned commit, never vendored)
with every case that is refused or not run still in the denominator, and writes the result under `build/`.
`documentation/conformance.md` says what the score means and how the tolerance is measured against two
independent renderers in pinned containers (`make oracle-images`, `make oracle-tolerance`).

## Status

Not released; nothing in the suite is yet. The design is in `documentation/design.md` and the SVG
choices in `documentation/formats/svg.md`.

## License

LGPL-3.0-only. See `COPYING` and `COPYING.LESSER`. Patches are not being accepted at this time; see
`CONTRIBUTING.md`.
