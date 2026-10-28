#!/bin/sh
#
# Prove that check-front-end.sh fails on what it exists to catch: the real
# script against fixtures built here.
#
#   control   a clean source tree and a clean object; the gate must pass them
#   planted   one defect each; the gate must exit non-zero and name it
#   empty     nothing; the gate must fail rather than pass a population of zero
#
# Environment: CC, the C compiler (default cc).

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
G="$HERE/check-front-end.sh"
CC="${CC:-cc}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

failures=0
checks=0

fail() {
  printf 'check-front-end-gate: FAIL: %s\n' "$*" >&2
  failures=$((failures + 1))
}

expect_pass() {
  label="$1"; shift
  checks=$((checks + 1))
  if out="$("$@" 2>&1)"; then
    printf '  ok   %s passes\n' "$label"
  else
    fail "$label should pass but failed:
$out"
  fi
}

expect_fail() {
  label="$1"; needle="$2"; shift 2
  checks=$((checks + 1))
  out="$("$@" 2>&1)"
  rc=$?
  if [ "$rc" -eq 0 ]; then
    fail "$label should fail but exited 0:
$out"
  elif ! printf '%s' "$out" | grep -qF -- "$needle"; then
    fail "$label failed without naming '$needle':
$out"
  else
    printf '  ok   %s fails, naming %s\n' "$label" "$needle"
  fi
}

# Includes.
ctl="$work/ctl"
mkdir -p "$ctl/src" "$ctl/include" "$ctl/examples"
printf '#include <ghoti.io/shape-scene/edit.h>\n#include <ghoti.io/shape-scene/list.h>\n#include <ghoti.io/shape-scene/sink.h>\n' > "$ctl/src/a.c"
cp "$ctl/src/a.c" "$ctl/include/a.h"
cp "$ctl/src/a.c" "$ctl/examples/a.c"
expect_pass 'front-end/includes control (edit.h, list.h, sink.h)' "$G" --includes "$ctl"
mkdir -p "$work/empty"
expect_fail 'front-end/includes empty' 'measuring nothing' "$G" --includes "$work/empty"
for where in src include examples; do
  for spelling in '<ghoti.io/shape-scene/scene.h>' '"ghoti.io/shape-scene/scene.h"' '<ghoti.io/shape-scene/scene.h>  '; do
    t="$work/inc-$where-$checks"
    mkdir -p "$t/src" "$t/include" "$t/examples"
    cp "$ctl/src/a.c" "$t/src/a.c"
    printf '  #  include %s\n' "$spelling" > "$t/$where/planted.c"
    expect_fail "front-end/includes planted (scene.h in $where, $spelling)" "planted.c" "$G" --includes "$t"
  done
done

# Symbols.
printf 'extern int clean_helper(int);\nint clean(int x) { return clean_helper(x); }\n' > "$work/clean.c"
if ! $CC -std=c17 -c -O0 "$work/clean.c" -o "$work/clean.o" 2>"$work/cc.log"; then
  fail "the control object did not compile: $(cat "$work/cc.log")"
else
  ar rcs "$work/clean.a" "$work/clean.o"
  expect_pass 'front-end/symbols control' "$G" --symbols "$work/clean.a"
  printf 'int nothing;\n' > "$work/none.c"
  $CC -std=c17 -c "$work/none.c" -o "$work/none.o"
  ar rcs "$work/none.a" "$work/none.o"
  expect_fail 'front-end/symbols empty (no undefined symbol)' 'measuring nothing' "$G" --symbols "$work/none.a"
  expect_fail 'front-end/symbols missing archive' 'is not a file' "$G" --symbols "$work/absent.a"
  for name in getenv setlocale localeconv time clock_gettime gettimeofday strtod strtof atof sscanf; do
    # A prototype of its own, not the C library's: what is planted is the
    # reference to the name, and an explicit parameter list means the same in
    # every C standard (an empty one declares no parameters from C23 on).
    printf 'extern int %s(void *, void *, void *);\nint planted(void) { return %s(0, 0, 0) != 0; }\n' "$name" "$name" > "$work/p-$name.c"
    if $CC -std=c17 -c -O0 -fno-builtin -w "$work/p-$name.c" -o "$work/p-$name.o" 2>>"$work/cc.log"; then
      ar rcs "$work/p-$name.a" "$work/clean.o" "$work/p-$name.o"
      expect_fail "front-end/symbols planted ($name)" "references $name" "$G" --symbols "$work/p-$name.a"
    else
      fail "planted object for $name did not compile: $(cat "$work/cc.log")"
    fi
  done
  # glibc's renamed reader is the same function.
  printf 'extern double __isoc23_strtod(const char *, char **);\nint planted(void) { return __isoc23_strtod(0, 0) != 0; }\n' > "$work/p-iso.c"
  $CC -std=c17 -c -O0 "$work/p-iso.c" -o "$work/p-iso.o" && ar rcs "$work/p-iso.a" "$work/p-iso.o"
  expect_fail 'front-end/symbols planted (__isoc23_strtod)' 'references strtod' "$G" --symbols "$work/p-iso.a"
  # A name that merely contains a forbidden one is not it.
  printf 'extern int my_getenv_like(void);\nint fine(void) { return my_getenv_like(); }\n' > "$work/p-near.c"
  $CC -std=c17 -c -O0 "$work/p-near.c" -o "$work/p-near.o" && ar rcs "$work/p-near.a" "$work/p-near.o"
  expect_pass 'front-end/symbols control (my_getenv_like is not getenv)' "$G" --symbols "$work/p-near.a"
fi

# The lowering's objects reach neither the text library nor the loader.
if [ -f "$work/clean.o" ]; then
  mkdir -p "$work/low-ok" "$work/low-empty" "$work/low-bad1" "$work/low-bad2" "$work/low-bad3"
  printf 'extern int gvec_svg_get_like(int);\nint lowering_part(int x) { return gvec_svg_get_like(x); }\n' > "$work/low.c"
  $CC -std=c17 -c -O0 "$work/low.c" -o "$work/low-ok/low.o"
  expect_pass 'front-end/lowering control (a style accessor is fine)' "$G" --lowering "$work/low-ok"
  expect_fail 'front-end/lowering empty' 'measuring nothing' "$G" --lowering "$work/low-empty"
  for plant in "gtext_xml_reader_create" "ghotiio_text_0_gtext_css_parse" "ghotiio_vector_0_gvec_svg_load" "gvec_svg_probe" "ghotiio_vector_0_gvec_svg_build"; do
    d="$work/low-p-$plant"
    mkdir -p "$d"
    cp "$work/low-ok/low.o" "$d/low.o"
    printf 'extern int %s(void);\nint planted(void) { return %s(); }\n' "$plant" "$plant" > "$work/lp.c"
    $CC -std=c17 -c -O0 "$work/lp.c" -o "$d/planted.o"
    expect_fail "front-end/lowering planted ($plant)" "$plant" "$G" --lowering "$d"
  done
  # A name that merely resembles them is not them.
  d="$work/low-near"
  mkdir -p "$d"
  printf 'extern int gvec_svg_loaded_like(void);\nint near(void) { return gvec_svg_loaded_like(); }\n' > "$work/ln.c"
  $CC -std=c17 -c -O0 "$work/ln.c" -o "$d/near.o"
  expect_pass 'front-end/lowering control (gvec_svg_loaded_like is not the loader)' "$G" --lowering "$d"
fi

if [ "$failures" -ne 0 ]; then
  printf 'check-front-end-gate: %d of %d checks failed\n' "$failures" "$checks" >&2
  exit 1
fi
printf 'check-front-end-gate: all %d checks passed; the gate fails on every planted violation\n' "$checks"
