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

remote=$root/tools/resvg-tests/REMOTE
remote_dest=${GHOTI_RESVG_REMOTE_DIR:-$(dirname "$dest")/resvg-remote}

# The files the cases name by URL (tools/resvg-tests/REMOTE), each kept only if its SHA-256 is the pin: a download that differs is refused,
# and a file already there that differs is replaced by a fresh download that must match.
sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{ print $1 }'; else shasum -a 256 "$1" | awk '{ print $1 }'; fi
}
fetch_remote() {
  [ -f "$remote" ] || return 0
  mkdir -p "$remote_dest"
  grep -v '^#' "$remote" | grep -v '^$' | while IFS="$(printf '\t')" read -r url sum bytes file; do
    if [ -f "$remote_dest/$file" ] && [ "$(sha256_of "$remote_dest/$file")" = "$sum" ]; then
      printf 'fetch: %s is already %s\n' "$remote_dest/$file" "$sum"
      continue
    fi
    rm -f "$remote_dest/$file.partial"
    if command -v curl >/dev/null 2>&1; then
      curl --fail --silent --show-error --location --max-time 120 --user-agent 'ghoti-corpus-fetch/1.0' --output "$remote_dest/$file.partial" "$url" || { rm -f "$remote_dest/$file.partial"; printf 'fetch: cannot download %s\n' "$url" >&2; exit 1; }
    else
      wget --quiet --timeout=120 --output-document="$remote_dest/$file.partial" "$url" || { rm -f "$remote_dest/$file.partial"; printf 'fetch: cannot download %s\n' "$url" >&2; exit 1; }
    fi
    got=$(sha256_of "$remote_dest/$file.partial")
    if [ "$got" != "$sum" ]; then
      printf 'fetch: %s has SHA-256 %s, REMOTE pins %s\n' "$url" "$got" "$sum" >&2
      rm -f "$remote_dest/$file.partial"
      exit 1
    fi
    mv "$remote_dest/$file.partial" "$remote_dest/$file"
    printf 'fetch: %s at %s\n' "$remote_dest/$file" "$sum"
  done
}

if [ -d "$dest" ] && [ "$(git -C "$dest" rev-parse HEAD 2>/dev/null)" = "$commit" ]; then
  printf 'fetch: %s is already at %s\n' "$dest" "$commit"
  fetch_remote
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
fetch_remote
