/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Vector.
 *
 * Ghoti.io Vector is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Vector is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The dump of the typed document (L10, extended): one line per element, attribute and
 * text node, canonical and identical on every target.
 */

#include <ghoti.io/vector/macros.h>

#include <stdio.h>
#include <string.h>

#include <ghoti.io/math/decimal.h>

#include "../../core/core_internal.h"
#include "svg_doc.h"

typedef struct writer {
  GVEC_Stream * out;
  char buf[4096];
  size_t used;
  GVEC_Result error;
} writer;

static void flush(writer * w) {
  if (w->used && w->error == GVEC_OK) {
    w->error = gvec_stream_write(w->out, w->buf, w->used);
  }
  w->used = 0;
}

static void put(writer * w, const char * s, size_t n) {
  while (n > 0) {
    size_t room = sizeof w->buf - w->used;
    size_t take = n < room ? n : room;
    memcpy(w->buf + w->used, s, take);
    w->used += take;
    s += take;
    n -= take;
    if (w->used == sizeof w->buf) {
      flush(w);
    }
  }
}

static void puts_(writer * w, const char * s) {
  put(w, s, strlen(s));
}

static void putf(writer * w, const char * format, ...) __attribute__((format(printf, 2, 3)));

#include <stdarg.h>

static void putf(writer * w, const char * format, ...) {
  char tmp[160];
  va_list args;
  va_start(args, format);
  int n = vsnprintf(tmp, sizeof tmp, format, args);
  va_end(args);
  if (n > 0) {
    put(w, tmp, (size_t) n < sizeof tmp ? (size_t) n : sizeof tmp - 1);
  }
}

/* A quoted string: printable ASCII as it is, `\` and `"` and everything else as \xNN. */
static void quoted(writer * w, const char * s, size_t n) {
  put(w, "\"", 1);
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char) s[i];
    if (c >= 0x20 && c <= 0x7e && c != '\\' && c != '"') {
      put(w, (const char *) &c, 1);
    } else {
      char hex[5];
      snprintf(hex, sizeof hex, "\\x%02x", c);
      put(w, hex, 4);
    }
  }
  put(w, "\"", 1);
}

static void quoted_z(writer * w, const char * s) {
  quoted(w, s ? s : "", s ? strlen(s) : 0);
}

/* A double in the shortest spelling that reads back to the same bits: digits, 'e', exponent. */
static void number(writer * w, double x) {
  GMATH_Decimal d;
  if (gmath_decimal_shortest(x, &d) != GMATH_OK) {
    puts_(w, "nan");
    return;
  }
  if (d.negative) {
    put(w, "-", 1);
  }
  putf(w, "%se%d", d.digits, d.exponent);
}

static const char * const k_class[] = {"typed", "invalid", "property", "raw", "unknown", "script"};
static const char * const k_unit[] = {"", "px", "em", "ex", "ch", "rem", "vw", "vh", "vmin", "vmax", "cm", "mm", "q", "in",
    "pt", "pc", "%", "deg", "grad", "rad", "turn"};
static const char * const k_segment[] = {"M", "L", "Q", "C", "A", "Z"};

static void length(writer * w, const GVEC_Svg_Length * l) {
  number(w, l->value);
  if (l->unit != GVEC_SVG_UNIT_NONE) {
    puts_(w, k_unit[l->unit]);
  }
}

static void value(writer * w, const GVEC_Svg_Attribute * a) {
  const gvec_svg_value * v = &a->value;
  if (a->cls == GVEC_SVG_ATTR_INVALID) {
    putf(w, "err:%zu:", a->error_offset);
    quoted_z(w, a->error_reason);
    return;
  }
  if (a->cls != GVEC_SVG_ATTR_TYPED) {
    puts_(w, "-");
    return;
  }
  switch ((GVEC_Svg_Value_Kind) a->value_kind) {
  case GVEC_SVG_VALUE_NUMBER:
    puts_(w, "n:");
    number(w, v->number);
    break;
  case GVEC_SVG_VALUE_OFFSET:
    puts_(w, "off:");
    number(w, v->number);
    break;
  case GVEC_SVG_VALUE_INTEGER:
    putf(w, "i:%ld", v->integer);
    break;
  case GVEC_SVG_VALUE_LENGTH:
    puts_(w, "l:");
    length(w, &v->length);
    break;
  case GVEC_SVG_VALUE_NUMBER_LIST:
  case GVEC_SVG_VALUE_POINTS:
    puts_(w, a->value_kind == GVEC_SVG_VALUE_POINTS ? "pts:[" : "ns:[");
    for (size_t i = 0; i < v->numbers.count; i++) {
      if (i) {
        put(w, ",", 1);
      }
      number(w, v->numbers.items[i]);
    }
    put(w, "]", 1);
    break;
  case GVEC_SVG_VALUE_LENGTH_LIST:
    puts_(w, "ls:[");
    for (size_t i = 0; i < v->lengths.count; i++) {
      if (i) {
        put(w, ",", 1);
      }
      length(w, &v->lengths.items[i]);
    }
    put(w, "]", 1);
    break;
  case GVEC_SVG_VALUE_PATH: {
    puts_(w, "p:");
    for (size_t i = 0; i < v->path.command_count; i++) {
      const GVEC_Svg_Path_Command * c = &v->path.commands[i];
      if (i) {
        put(w, ";", 1);
      }
      if (c->implicit) {
        put(w, "+", 1);
      }
      put(w, &c->letter, 1);
      for (int k = 0; k < c->arg_count; k++) {
        put(w, k ? "," : " ", 1);
        number(w, c->args[k]);
      }
    }
    puts_(w, " abs:");
    for (size_t i = 0; i < v->path.segment_count; i++) {
      const GVEC_Svg_Path_Segment * s = &v->path.segments[i];
      if (i) {
        put(w, ";", 1);
      }
      puts_(w, k_segment[s->kind]);
      put(w, " ", 1);
      number(w, s->x0);
      put(w, ",", 1);
      number(w, s->y0);
      switch (s->kind) {
      case GVEC_SVG_SEGMENT_QUAD:
        put(w, ",", 1); number(w, s->x1); put(w, ",", 1); number(w, s->y1);
        break;
      case GVEC_SVG_SEGMENT_CUBIC:
        put(w, ",", 1); number(w, s->x1); put(w, ",", 1); number(w, s->y1);
        put(w, ",", 1); number(w, s->x2); put(w, ",", 1); number(w, s->y2);
        break;
      case GVEC_SVG_SEGMENT_ARC:
        put(w, ",", 1); number(w, s->rx); put(w, ",", 1); number(w, s->ry); put(w, ",", 1); number(w, s->rotation);
        putf(w, ",%u,%u", s->large_arc, s->sweep);
        break;
      default:
        break;
      }
      put(w, ",", 1);
      number(w, s->x);
      put(w, ",", 1);
      number(w, s->y);
    }
    break;
  }
  case GVEC_SVG_VALUE_VIEW_BOX:
    puts_(w, "vb:");
    number(w, v->view_box.x); put(w, ",", 1); number(w, v->view_box.y); put(w, ",", 1);
    number(w, v->view_box.width); put(w, ",", 1); number(w, v->view_box.height);
    break;
  case GVEC_SVG_VALUE_PRESERVE_ASPECT_RATIO:
    putf(w, "par:%d:%u:%u", (int) v->par.align, v->par.slice, v->par.defer);
    break;
  case GVEC_SVG_VALUE_ORIENT:
    putf(w, "o:%d:", (int) v->orient.mode);
    number(w, v->orient.angle);
    break;
  case GVEC_SVG_VALUE_REFERENCE:
    switch (v->reference.kind) {
    case GVEC_SVG_REF_EMPTY: puts_(w, "r:empty"); break;
    case GVEC_SVG_REF_INTERNAL: puts_(w, "r:internal:"); quoted_z(w, v->reference.fragment); break;
    case GVEC_SVG_REF_EXTERNAL: puts_(w, "r:external:"); quoted_z(w, v->reference.url); break;
    case GVEC_SVG_REF_DATA:
      puts_(w, "r:data:");
      quoted_z(w, v->reference.data.media_type);
      put(w, ":", 1);
      quoted_z(w, v->reference.data.parameters);
      putf(w, ":%u:%zu", v->reference.data.base64, v->reference.data.payload_length);
      break;
    }
    break;
  case GVEC_SVG_VALUE_KEYWORD:
    putf(w, "k:%zu:%s", v->keyword.index, v->keyword.word);
    break;
  case GVEC_SVG_VALUE_STRING:
    puts_(w, "s:");
    quoted(w, a->raw, a->raw_length);
    break;
  case GVEC_SVG_VALUE_LANGUAGES:
    puts_(w, "lang:[");
    for (size_t i = 0; i < v->languages.count; i++) {
      if (i) {
        put(w, ",", 1);
      }
      quoted_z(w, v->languages.tags[i]);
    }
    put(w, "]", 1);
    break;
  case GVEC_SVG_VALUE_EXTENSIONS:
    putf(w, "x:%zu", v->count);
    break;
  default:
    puts_(w, "-");
    break;
  }
}

static void dump_element(writer * w, const GVEC_Svg_Element * e, size_t depth) {
  const char * kind = e->kind == GVEC_SVG_KIND_UNKNOWN ? "unknown" : e->kind == GVEC_SVG_KIND_FOREIGN ? "foreign"
      : gvec_svg_kind_name(e->kind);
  putf(w, "e %zu %016llx %s ", depth, (unsigned long long) e->key, kind);
  quoted_z(w, e->name);
  put(w, " ", 1);
  if (e->ns) {
    quoted_z(w, e->ns);
  } else {
    puts_(w, "-");
  }
  putf(w, " %u:%u", e->node.line, e->node.column);
  bool any = false;
  static const struct {
    uint32_t bit;
    const char * name;
  } flags[] = {{GVEC_SVG_ELEMENT_COND_FALSE, "conditional-false"}, {GVEC_SVG_ELEMENT_ON_CYCLE, "on-cycle"},
      {GVEC_SVG_ELEMENT_EXPANSION_LIMITED, "expansion-limited"}, {GVEC_SVG_ELEMENT_KEY_COLLISION, "key-collision"}};
  for (size_t f = 0; f < 4; f++) {
    if (e->flags & flags[f].bit) {
      putf(w, "%s%s", any ? "," : " ", flags[f].name);
      any = true;
    }
  }
  if (!any) {
    puts_(w, " -");
  }
  if (e->href_target) {
    putf(w, " href=%016llx", (unsigned long long) e->href_target->key);
  }
  if (e->switch_selected) {
    putf(w, " selected=%016llx", (unsigned long long) e->switch_selected->key);
  }
  if (e->kind == GVEC_SVG_KIND_USE) {
    putf(w, " expansion=%llu", (unsigned long long) e->expansion);
  }
  put(w, "\n", 1);
  for (size_t k = 0; k < e->attribute_count; k++) {
    const GVEC_Svg_Attribute * a = &e->attributes[k];
    puts_(w, "a ");
    quoted_z(w, a->name);
    putf(w, " %s %s %u:%u", k_class[a->cls], gvec_svg_grammar_name((GVEC_Svg_Grammar) a->grammar), a->line, a->column);
    putf(w, " %s", a->presentation ? "presentation" : "-");
    if (a->partial) {
      putf(w, ",partial@%zu", a->error_offset);
    }
    put(w, " ", 1);
    if (a->ns) {
      quoted_z(w, a->ns);
      put(w, " ", 1);
    }
    puts_(w, "raw=");
    quoted(w, a->raw, a->raw_length);
    puts_(w, " value=");
    value(w, a);
    put(w, "\n", 1);
  }
}

GVEC_Result gvec_svg_dump_document(const GVEC_Svg_Document * doc, GVEC_Stream * out) {
  writer w;
  w.out = out;
  w.used = 0;
  w.error = GVEC_OK;
  putf(&w, "svg elements %zu text-bytes %zu\n", doc->element_count, doc->text_bytes);
  /* Pre-order over the nodes, so character data sits between the elements it was written between. */
  const GVEC_Svg_Node * n = &doc->root->node;
  size_t depth = 0;
  while (n) {
    if (n->is_element) {
      const GVEC_Svg_Element * e = n->element;
      dump_element(&w, e, depth);
      if (e->first_node) {
        n = e->first_node;
        depth++;
        continue;
      }
    } else {
      putf(&w, "t %zu %u:%u %zu ", depth, n->line, n->column, n->text_length);
      quoted(&w, n->text, n->text_length);
      put(&w, "\n", 1);
    }
    while (n && !n->next) {
      n = n->parent ? &n->parent->node : NULL;
      if (depth > 0) {
        depth--;
      }
    }
    if (n) {
      n = n->next;
    }
  }
  flush(&w);
  return w.error;
}
