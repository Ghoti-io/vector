#!/bin/sh
#
# Fetch the texts the animation of a lowering at a time follows (design.md,
# "Animation"), and verify each by its SHA-256. Nothing is built from them;
# the tests cite them. SVG 2 links its animation elements to the SVG
# Animations editor's draft, which changes; these are the fixed W3C texts it
# was made from:
#
#   animate.html          SVG 1.1 Second Edition, chapter 19 (Recommendation, 16 August 2011)
#   tiny12-struct.html    SVG Tiny 1.2, chapter 5, for `discard` (Recommendation, 22 December 2008)
#   smil-timing.html      SMIL 3.0 Timing and Synchronization (Recommendation, 1 December 2008)
#   smil-animation.html   SMIL 3.0 Animation (Recommendation, 1 December 2008)
#
# The pages are W3C documents under the W3C Document License; they are fetched,
# never copied into this repository.
#
# Usage:  tools/svg-animations/fetch.sh [DIR]     (default third_party/svg-animations)
#
# Copyright 2026 by Corey Pennycuff

set -eu
dir=${1:-third_party/svg-animations}
mkdir -p "$dir"

fetch() {
  name=$1
  url=$2
  want=$3
  if [ ! -f "$dir/$name" ]; then
    curl -sSfL -o "$dir/$name.part" "$url"
    mv "$dir/$name.part" "$dir/$name"
  fi
  got=$(sha256sum "$dir/$name" | awk '{print $1}')
  if [ "$got" != "$want" ]; then
    printf 'svg-animations: %s has SHA-256 %s, pinned %s\n' "$name" "$got" "$want" >&2
    exit 1
  fi
  printf 'svg-animations: %s ok\n' "$name"
}

fetch animate.html https://www.w3.org/TR/2011/REC-SVG11-20110816/animate.html ab70a88b357181ba7ca2727f1f211b8ae3b0f7cd0c0257c800fc3e217d235224
fetch tiny12-struct.html https://www.w3.org/TR/2008/REC-SVGTiny12-20081222/struct.html a2f8c153c39031b59197296762a349b134b91091a634f38e974e2ef6921c1e9a
fetch smil-timing.html https://www.w3.org/TR/2008/REC-SMIL3-20081201/smil-timing.html 279caef0ec70ee5adc562434c6b19178be1d68fd99cf4c7f30fc4a67cf625e62
fetch smil-animation.html https://www.w3.org/TR/2008/REC-SMIL3-20081201/smil-animation.html 8e128d2c7593a4af6cf80b60006cbbe595fe2c437742e0022848a9b2497c70bc
