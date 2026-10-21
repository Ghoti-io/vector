#!/bin/sh
#
# Run every example, and fail on the first that does not exit 0.
#
# An example is a program a developer copies, and one that is only compiled
# rots unseen: its calls still link while what it claims stops being true.  So
# each example checks what it shows and exits non-zero when that does not
# hold, and `make run-examples` (in TEST_GATES) runs them all (story 16 of the
# shape family's first milestone).  An example's output goes to OUT_DIR/NAME.out
# and is printed only when it fails.
#
# Before the real run the runner is seen to fail: on fixture programs made here,
# one that exits 0 must pass; one that exits 3, and one that kills itself with
# a signal, must each fail naming that program; and an empty list must fail
# rather than report success over nothing.
#
#   usage: run-examples.sh OUT_DIR EXAMPLE...
#
# The same text is in math, shape-geometry, shape-scene and shape-cpu;
# suite/tools/xarch/shape.sh fails when the copies differ.

set -u

out_dir="${1:?usage: run-examples.sh OUT_DIR EXAMPLE...}"
shift

# run_all DIR PROGRAM... -> 0 if every program exits 0; names the first that does not.
run_all() {
  dir="$1"
  shift
  if [ "$#" -eq 0 ]; then
    printf 'run-examples: FAIL: no examples: this would measure nothing\n' >&2
    return 1
  fi
  n=0
  for e in "$@"; do
    name="$(basename "$e")"
    if "$e" > "$dir/$name.out" 2>&1; then
      n=$((n + 1))
    else
      rc=$?
      printf 'run-examples: FAIL: %s exited %d\n' "$name" "$rc" >&2
      sed 's/^/  | /' "$dir/$name.out" >&2
      return 1
    fi
  done
  printf 'run-examples: %d examples ran, each exited 0\n' "$n"
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM
printf '#!/bin/sh\necho fine\n' > "$work/control"
printf '#!/bin/sh\necho "claim does not hold"\nexit 3\n' > "$work/planted-exit"
printf '#!/bin/sh\nkill -SEGV $$\n' > "$work/planted-signal"
chmod +x "$work/control" "$work/planted-exit" "$work/planted-signal"
selftest=0
run_all "$work" "$work/control" > /dev/null 2>&1 || { printf 'run-examples: FAIL: the control was refused\n' >&2; selftest=1; }
for planted in planted-exit planted-signal; do
  if msg="$(run_all "$work" "$work/control" "$work/$planted" 2>&1)"; then
    printf 'run-examples: FAIL: %s passed\n' "$planted" >&2
    selftest=1
  elif ! printf '%s' "$msg" | grep -q "FAIL: $planted exited"; then
    printf 'run-examples: FAIL: %s failed without being named:\n%s\n' "$planted" "$msg" >&2
    selftest=1
  fi
done
run_all "$work" > /dev/null 2>&1 && { printf 'run-examples: FAIL: an empty list passed\n' >&2; selftest=1; }
[ "$selftest" -eq 0 ] || exit 1

mkdir -p "$out_dir" || exit 1
run_all "$out_dir" "$@"
