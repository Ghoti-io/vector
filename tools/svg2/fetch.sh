#!/bin/sh
#
# Fetch the SVG 2 Candidate Recommendation pages the element and attribute tables
# are generated from and the grammars are checked against, and verify each by its
# SHA-256. Nothing is built from them at run time: tools/svg2/gen-tables.py writes
# the committed C tables from them, and the tests quote them. They are the
# specification, kept so that a reader can check a rule against the edition this
# library cites (W3C Candidate Recommendation, 4 October 2018).
#
# The pages are W3C documents under the W3C Document License; they are fetched,
# never copied into this repository.
#
# Usage:  tools/svg2/fetch.sh [DIR]     (default third_party/svg2)
#
# Copyright 2026 by Corey Pennycuff

set -eu
dir=${1:-third_party/svg2}
mkdir -p "$dir"
base=https://www.w3.org/TR/2018/CR-SVG2-20181004

fetch() {
  name=$1
  want=$2
  if [ ! -f "$dir/$name" ]; then
    curl -sSfL -o "$dir/$name.part" "$base/$name"
    mv "$dir/$name.part" "$dir/$name"
  fi
  got=$(sha256sum "$dir/$name" | awk '{print $1}')
  if [ "$got" != "$want" ]; then
    printf 'svg2: %s has SHA-256 %s, pinned %s\n' "$name" "$got" "$want" >&2
    exit 1
  fi
  printf 'svg2: %s ok\n' "$name"
}

fetch eltindex.html cdf201e0a6588896b7b1bae739ac80f7bbd44f25a2a76632a2f6762f94d02744
fetch attindex.html d008a1b62808268bfda853659e425554f6fbc9b2e3f415bc2b6aad437712bd50
fetch propidx.html  d80b560f17cfdd16fe2cf5e976fc82d8585492c76aa349a836463cd14228de16
fetch styling.html  66a21db8037944850c05dbd67d4c65162115b415302bd4c641e5f2a65b02ffc5
fetch paths.html    6213e9b47b068443841fc840dde69afdd188a25aa6ea066e284b612b28f44a8d
fetch struct.html   abca334257fb3646e1ae5a5645a08621b4738a44e05c6d470bae03f5fd21ff62
fetch coords.html   79f20813910f1866e40bf1548064ff2e47c095e425931a31063cf58f6c5334eb
