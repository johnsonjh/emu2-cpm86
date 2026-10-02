#!/bin/sh
set -eu

: "${root:?root must point to the source tree}"
CC=${CC:-cc}
CFLAGS=${CFLAGS:-}
LDLIBS=${LDLIBS:--lm}

tmp=${TMPDIR:-/tmp}/emu2-timer-clock.$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir "$tmp"

# shellcheck disable=SC2086
"$CC" $CFLAGS -I"$root/src" \
  "$root/tests/test_timer_clock.c" "$root/src/timer.c" \
  -o "$tmp/test_timer_clock" $LDLIBS

"$tmp/test_timer_clock"
