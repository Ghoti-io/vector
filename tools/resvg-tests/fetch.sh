#!/bin/sh
#
# Fetch the conformance corpus at the commit tools/resvg-tests/VERSIONS names.
#
# Nothing of it is committed. It lands in third_party/resvg-tests/, which
# .gitignore excludes. The clone is made in a .partial directory and moved into
# place only once the checked-out commit equals the pin, so that an interrupted
# or wrong fetch never leaves something that looks ready.
#
# Usage:  tools/resvg-tests/fetch.sh [DEST]
#
# DEST defaults to third_party/resvg-tests. The URL may be overridden with
# GHOTI_RESVG_TESTS_URL (a mirror, or a local clone).
#
# Copyright 2026 by Corey Pennycuff

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
versions=$root/tools/resvg-tests/VERSIONS
dest=${1:-$root/third_party/resvg-tests}

pin_for() {
  awk -v name="$1" '$1 == name { print $2; exit }' "$versions"
}

url=${GHOTI_RESVG_TESTS_URL:-$(pin_for resvg-test-suite-url)}
commit=$(pin_for resvg-test-suite-commit)

case $commit in
  [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ;;
  *) printf 'fetch: VERSIONS does not hold a 40-character commit for resvg-test-suite-commit\n' >&2; exit 1 ;;
esac

if [ -d "$dest" ] && [ "$(git -C "$dest" rev-parse HEAD 2>/dev/null)" = "$commit" ]; then
  printf 'fetch: %s is already at %s\n' "$dest" "$commit"
  exit 0
fi

rm -rf "$dest.partial"
mkdir -p "$(dirname "$dest")"
git clone --quiet --filter=blob:none --no-checkout "$url" "$dest.partial"
git -C "$dest.partial" checkout --quiet "$commit"
got=$(git -C "$dest.partial" rev-parse HEAD)
if [ "$got" != "$commit" ]; then
  printf 'fetch: checked out %s, VERSIONS pins %s\n' "$got" "$commit" >&2
  rm -rf "$dest.partial"
  exit 1
fi
rm -rf "$dest"
mv "$dest.partial" "$dest"
printf 'fetch: %s at %s\n' "$dest" "$commit"
