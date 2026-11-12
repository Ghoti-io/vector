# Refusals

This file is generated from `tools/lower/reasons.tsv`; do not edit it.

When a document is lowered, an element that the library cannot draw is **refused whole**: nothing of it is drawn, and one
`GVEC_DIAG_LOWER_REFUSED` diagnostic gives its name, its line and column, and one of the reasons below in `detail`
(`gvec_lower_reason_string()` spells it). An element is never half drawn, so the report and the picture say the same
thing, and the conformance harness scores a case with a refusal as refused, never as passed.

A reason retires from this list when the work that draws it lands. Elements that are never rendered by definition
(`defs`, descriptive elements, paint servers, markers, clip paths, masks and filters as definitions, `style`, `script`,
animation, and what SVG 2 does not define) are not refusals.

| Reason | Name | Lifted by | What is refused | Why |
| --- | --- | --- | --- | --- |
| 1 `GVEC_LOWER_GRADIENT` | `gradient` | V4 | A gradient whose stops cannot be read (a stop color the waist cannot carry); the stop is reported with its own reason. | Gradients are lowered, in sRGB or in linear light. |
| 2 `GVEC_LOWER_CLIP_PATH` | `clip-path` | G4 (V4) | a basic shape on a `clipPath`'s shape, a `use` child with a `clip-path`, a group in `objectBoundingBox` units that holds more than shapes, or a shape that has a `clip-path` of its own among several. | A `clipPath` of any number of shapes is lowered, as a union clip; what is refused is a basic shape on a `clipPath`'s shape, a `use` child with a `clip-path` of its own, a group in `objectBoundingBox` units that holds a `use`, text or an image, and a clip on one of several shapes. A reference that names nothing is not a refusal: SVG 2 treats the property as if it were not specified. |
| 3 `GVEC_LOWER_MASK` | `mask` | G5 (V6) | `mask` naming a mask of another document, or a mask in the units of the bounding box of an element that holds more than shapes and groups of them. | The box of such an element is not measured, and a mask placed by a guess would be drawing the wrong picture. |
| 4 `GVEC_LOWER_IMAGE` | `image` | G6 (V6) | An `image` element that would be seen. | Images are lowered in V6. |
| 5 `GVEC_LOWER_FILTER` | `filter` | G10 (V8) | `filter` naming anything, including an element the document does not have. | Filters are lowered in V8. |
| 6 `GVEC_LOWER_TEXT` | `text` | G11 (V7) | A `text` element. | Text is lowered in V7, through the font library's layout and glyph runs. |
| 7 `GVEC_LOWER_CONTEXT_PAINT` | `context-paint` | V3 | `context-fill` or `context-stroke` as a paint. | They name the paint of a marker's or a `use`'s context. Those in a marker or in a `use`'s referent are lowered, a gradient among them placed in the context element's box; one anywhere else is not, and a gradient taken from an outer context is refused, and a pattern is refused under `pattern`. |
| 8 `GVEC_LOWER_UNSUPPORTED_COLOR_SPACE` | `unsupported-color-space` | G9 | A colour the shape waist cannot carry: a space it has no gamut for, or a colour that needs a conversion the waist does not define. | Converting would approximate the colour. The colour is reported where it was read, and the element using it is refused. |
| 9 `GVEC_LOWER_FOREIGN_OBJECT` | `foreign-object` | never | A `foreignObject` that would be seen. | Its content is not SVG; this library does not render it. |
| 10 `GVEC_LOWER_EXTERNAL_REFERENCE` | `external-reference` | never | A `use` of an element of another document, or of a `data:` URI. | Nothing is fetched, ever (the load never reads outside its input). |
| 11 `GVEC_LOWER_REFERENCE_CYCLE` | `reference-cycle` | never | A `use` on a reference cycle. | The cycle was reported when the document was loaded; the use that closes it is not instantiated. |
| 12 `GVEC_LOWER_EXPANSION_LIMIT` | `expansion-limit` | never | A `use` that instantiates more elements than `max_reference_expansion` allows. | Reported when the document was loaded; the use is not instantiated. |
| 13 `GVEC_LOWER_LIMIT` | `limit` | never | A limit of the document, the lowering or `shape-scene` was reached: the drawing nests deeper than `max_depth` once references are expanded, a coordinate is past the waist's range, or a store, list or scene limit was met. | The element is refused rather than truncated. |
| 14 `GVEC_LOWER_STYLE` | `style` | never | A property of the element has no value that can be used (the style layer reports it where it was read). | The element cannot be drawn without its value. |
| 15 `GVEC_LOWER_TRANSFORM_BOX` | `transform-box` | V4 | A `transform` whose percentages are of a bounding box (`transform-box: fill-box` or `stroke-box`) on an element that has a percentage. | The bounding box is computed with the exact primitives. |
| 16 `GVEC_LOWER_EMBEDDED_CONTENT` | `embedded-content` | never | `audio`, `video`, `canvas` or `iframe` that would be seen. | This library renders none of them. |
| 17 `GVEC_LOWER_VECTOR_EFFECT` | `vector-effect` | never | A `vector-effect` other than `none` and `non-scaling-stroke` on an element that would be stroked. | The other values (`fixed-position`, `non-scaling-size`, `non-rotation`, `screen`) are not part of SVG 2's rendering model for a static document. |
| 18 `GVEC_LOWER_STYLESHEET` | `stylesheet` | V5 | A `style` element (of type `text/css`, or of none) whose sheet cannot be applied: it has an `@import`, which is never fetched, or a conditional rule (`@media`, `@supports`, `@container`) for which this library has no evaluator. | The sheet's rules would change elements the lowering draws; drawing without them would be drawing the wrong picture. |
