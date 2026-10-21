#!/bin/sh
#
# Fail where a front end of the shape family reaches past what a front end may.
#
# A front end reads a format and fills shape-scene's recording forms (an edit
# or a list) for its caller. It never publishes: publishing a version is the
# application's, so shape-scene/scene.h, which holds it, is never included.
# The other half of that rule (no sink library is included or linked) is
# check-edges', because no sink is in EDGES_ALLOWED.
#
# A load or a lowering also depends on nothing but its input and its options:
# no environment variable, no locale and no clock. A locale changes how the C
# library reads and writes a decimal number, and the decimal conversions come
# from math instead. So the archive may not reference a function that reads
# the environment, the locale or the clock, or a C-library decimal reader.
# This is a symbol check over the compiled archive, so a call reached through
# a macro or an inline wrapper is seen, which a source grep would miss.
#
# Usage:
#   check-front-end.sh --includes <root>    scan <root>/src, include and examples
#   check-front-end.sh --symbols <archive>  read the undefined symbols of an archive
# Both may be given. Each mode fails on an empty population.

set -eu

usage='usage: check-front-end.sh [--includes <root>] [--symbols <archive>...]'

FORBIDDEN='getenv secure_getenv setlocale localeconv newlocale uselocale duplocale nl_langinfo
time clock clock_gettime gettimeofday ftime localtime localtime_r gmtime gmtime_r mktime strftime
strtod strtof strtold atof sscanf vsscanf scanf fscanf vscanf vfscanf'

roots="" archives="" mode=""
while [ $# -gt 0 ]; do
  case "$1" in
    --includes) roots="$roots ${2:?$usage}"; shift 2 ;;
    --symbols) mode=symbols; shift ;;
    -*) printf '%s\n' "$usage" >&2; exit 2 ;;
    *)
      if [ "$mode" = symbols ]; then archives="$archives $1"; shift
      else printf '%s\n' "$usage" >&2; exit 2; fi ;;
  esac
done
if [ -z "$roots" ] && [ -z "$archives" ]; then
  printf '%s\n' "$usage" >&2
  exit 2
fi

failed=0

for root in $roots; do
  files=""
  for d in src include examples; do
    [ -d "$root/$d" ] || continue
    files="$files $(find "$root/$d" -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' \) | sort)"
  done
  count=0
  for f in $files; do count=$((count + 1)); done
  if [ "$count" -eq 0 ]; then
    printf 'check-front-end: no sources under %s/src, include or examples; this is measuring nothing\n' "$root" >&2
    exit 1
  fi
  for f in $files; do
    if hits=$(grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]ghoti\.io/shape-scene/scene\.h[>"]' "$f"); then
      printf 'check-front-end: %s includes shape-scene/scene.h (publishing is the application'"'"'s):\n%s\n' "$f" "$hits" >&2
      failed=1
    fi
  done
  [ "$failed" -eq 0 ] && printf 'check-front-end: %d sources, none includes shape-scene/scene.h\n' "$count"
done

for a in $archives; do
  if [ ! -f "$a" ]; then
    printf 'check-front-end: %s is not a file\n' "$a" >&2
    exit 1
  fi
  undefined=$(nm -u "$a" 2>/dev/null | awk 'NF >= 2 && $(NF-1) == "U" { print $NF } NF == 1 && $1 !~ /:$/ { print $1 }' | sed 's/@.*//' | sort -u)
  n=$(printf '%s\n' "$undefined" | grep -c . || true)
  if [ "$n" -eq 0 ]; then
    printf 'check-front-end: %s references no symbol at all; this is measuring nothing\n' "$a" >&2
    exit 1
  fi
  bad=0
  for name in $FORBIDDEN; do
    # glibc renames some readers (__isoc99_sscanf, __isoc23_strtod); the
    # renamed form is the same function.
    if printf '%s\n' "$undefined" | grep -qxE "(__isoc(99|23)_)?$name"; then
      printf 'check-front-end: %s references %s, which reads the environment, the locale or the clock\n' "$a" "$name" >&2
      bad=1
    fi
  done
  if [ "$bad" -ne 0 ]; then
    failed=1
  else
    printf 'check-front-end: %s: %d undefined symbols read, none reads the environment, locale or clock\n' "$a" "$n"
  fi
done

exit "$failed"
