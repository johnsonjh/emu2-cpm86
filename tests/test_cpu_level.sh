#!/bin/sh
set -eu

: "${EMU2:?EMU2 must point to the emu2 executable}"

tmp=${TMPDIR:-/tmp}/emu2-cpu-level.$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir "$tmp"

fail()
{
  echo "test_cpu_level: $*" >&2
  exit 1
}

emit()
{
  printf '%b' "$2" > "$tmp/$1"
}

expect_ok()
{
  if ! "$@" > "$tmp/out" 2> "$tmp/err"; then
    cat "$tmp/err" >&2
    fail "command unexpectedly failed: $*"
  fi
}

expect_fail()
{
  if "$@" > "$tmp/out" 2> "$tmp/err"; then
    fail "command unexpectedly succeeded: $*"
  fi
}

# Every generated file is a tiny DOS .COM that exits through INT 21h/AH=4Ch.
emit i186.com '\140\141\270\000\114\315\041'
emit i286.com '\017\006\270\000\114\315\041'
emit prefix186.com '\046\140\141\270\000\114\315\041'
emit rep186.com '\363\140\141\270\000\114\315\041'

# Opcode availability and prefix-aware checking.
expect_fail "$EMU2" -c 8086 "$tmp/i186.com"
grep -q 'requires 80186' "$tmp/err" || fail "missing 80186 diagnostic"
expect_ok "$EMU2" -c 80186 "$tmp/i186.com"
expect_ok "$EMU2" -c 80286 "$tmp/i186.com"

expect_fail "$EMU2" -c 80186 "$tmp/i286.com"
grep -q 'requires 80286' "$tmp/err" || fail "missing 80286 diagnostic"
expect_ok "$EMU2" -c 80286 "$tmp/i286.com"
expect_ok "$EMU2" "$tmp/i286.com" # default is 80286

expect_fail "$EMU2" -c 8086 "$tmp/prefix186.com"
grep -q 'requires 80186' "$tmp/err" || fail "segment prefix bypassed CPU gate"
expect_fail "$EMU2" -c 8086 "$tmp/rep186.com"
grep -q 'requires 80186' "$tmp/err" || fail "REP prefix bypassed CPU gate"

# EMU2_CPU and command-line precedence.
expect_fail env EMU2_CPU=8086 "$EMU2" "$tmp/i186.com"
expect_ok env EMU2_CPU=8086 "$EMU2" -c 80186 "$tmp/i186.com"
expect_ok env EMU2_CPU=bogus "$EMU2" -c 80186 "$tmp/i186.com"
expect_fail env EMU2_CPU=bogus "$EMU2" "$tmp/i186.com"
grep -q 'invalid CPU type' "$tmp/err" || fail "missing invalid EMU2_CPU diagnostic"

# PUSH SP: 8086/80186 push the post-decrement value; 80286 pushes original SP.
emit push-old.com '\274\000\200\124\133\201\373\376\177\165\005\270\000\114\315\041\270\001\114\315\041'
emit push-286.com '\274\000\200\124\133\201\373\000\200\165\005\270\000\114\315\041\270\001\114\315\041'
emit push-old-prefix.com '\274\000\200\046\124\133\201\373\376\177\165\005\270\000\114\315\041\270\001\114\315\041'
expect_ok "$EMU2" -c 8086 "$tmp/push-old.com"
expect_ok "$EMU2" -c 80186 "$tmp/push-old.com"
expect_ok "$EMU2" -c 80286 "$tmp/push-286.com"
expect_ok "$EMU2" "$tmp/push-286.com"
expect_ok "$EMU2" -c 8086 "$tmp/push-old-prefix.com"

# D2/D3 shift counts: unmasked on 8086, masked to five bits on 80186+.
emit shift-old.com '\260\001\261\040\322\340\074\000\165\005\270\000\114\315\041\270\001\114\315\041'
emit shift-186.com '\260\001\261\040\322\340\074\001\165\005\270\000\114\315\041\270\001\114\315\041'
emit shift-old-prefix.com '\260\001\261\040\046\322\340\074\000\165\005\270\000\114\315\041\270\001\114\315\041'
expect_ok "$EMU2" -c 8086 "$tmp/shift-old.com"
expect_ok "$EMU2" -c 80186 "$tmp/shift-186.com"
expect_ok "$EMU2" -c 80286 "$tmp/shift-186.com"
expect_ok "$EMU2" -c 8086 "$tmp/shift-old-prefix.com"

# Regression for the old word-SAR CF bug: SAR 0100h by 9 shifts original bit 8
# into CF.  This exercises both legacy and masked shift helpers.
emit sar-cf.com '\270\000\001\261\011\323\370\162\005\270\001\114\315\041\270\000\114\315\041'
expect_ok "$EMU2" -c 8086 "$tmp/sar-cf.com"
expect_ok "$EMU2" -c 80186 "$tmp/sar-cf.com"
expect_ok "$EMU2" -c 80286 "$tmp/sar-cf.com"

# Original 8086 IDIV rejects -128 and -32768 quotients; 80186+ accepts them.
emit idiv8min.com '\270\200\377\273\001\000\366\373\270\000\114\315\041'
emit idiv16min.com '\270\000\200\272\377\377\273\001\000\367\373\270\000\114\315\041'
expect_fail "$EMU2" -c 8086 "$tmp/idiv8min.com"
grep -qi 'divide' "$tmp/err" || fail "8-bit IDIV did not take divide error"
expect_ok "$EMU2" -c 80186 "$tmp/idiv8min.com"
expect_ok "$EMU2" -c 80286 "$tmp/idiv8min.com"
expect_fail "$EMU2" -c 8086 "$tmp/idiv16min.com"
grep -qi 'divide' "$tmp/err" || fail "16-bit IDIV did not take divide error"
expect_ok "$EMU2" -c 80186 "$tmp/idiv16min.com"
expect_ok "$EMU2" -c 80286 "$tmp/idiv16min.com"

# INT 0 saved IP. Both programs install their own handler.  The DIV has an ES:
# prefix so the 80286 test also proves start_ip includes the first prefix byte.
emit divip-old.com '\016\037\272\027\001\270\000\045\315\041\270\001\000\061\333\046\366\363\270\002\114\315\041\136\130\130\201\376\022\001\165\005\270\000\114\315\041\270\001\114\315\041'
emit divip-286.com '\016\037\272\027\001\270\000\045\315\041\270\001\000\061\333\046\366\363\270\002\114\315\041\136\130\130\201\376\017\001\165\005\270\000\114\315\041\270\001\114\315\041'
expect_ok "$EMU2" -c 8086 "$tmp/divip-old.com"
expect_ok "$EMU2" -c 80186 "$tmp/divip-old.com"
expect_ok "$EMU2" -c 80286 "$tmp/divip-286.com"

# DX:AX = INT32_MIN divided by -1 must become guest INT 0, not host-C signed
# overflow/SIGFPE.  The merged default INT 0 handler exits with a diagnostic.
emit idiv-host-overflow.com '\270\000\000\272\000\200\273\377\377\367\373\270\000\114\315\041'
expect_fail "$EMU2" -c 80286 "$tmp/idiv-host-overflow.com"
grep -qi 'divide' "$tmp/err" || fail "host-overflow IDIV did not become guest divide error"

# Aliases and invalid command-line input.
emit exit.com '\270\000\114\315\041'
expect_ok "$EMU2" -c 8088 "$tmp/exit.com"
expect_ok "$EMU2" -c 186 "$tmp/exit.com"
expect_ok "$EMU2" -c 286 "$tmp/exit.com"
expect_fail "$EMU2" -c 386 "$tmp/exit.com"
grep -q 'invalid CPU type' "$tmp/err" || fail "missing invalid -c diagnostic"

echo "CPU-level tests: ALL PASS"
