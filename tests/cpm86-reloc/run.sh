#!/bin/bash
# run.sh -- regression for the CP/M-86 loader's P_LOAD relocation (issue #1).
#
# Verifies that emu2 applies the .CMD load-time fixup table (header byte 0x7F
# bit 7 + word 0x7D ch_fixrec, 4-byte records `[grp-nibbles][para:2][byte:1]`,
# `add word, target_group_load_seg`) implemented in src/cpm86.c.
#
# Fixtures are Open Watcom `wlink format cpm86` output (pure loader-relocation,
# NO crt0 self-reloc walker): each embeds a cross-group far reference the linker
# leaves as a group-relative paragraph. They print "OK!" IFF the loader relocated
# that reference to the group's real load segment; without P_LOAD they read
# paragraph 0 and print garbage / fault. So a plain string match on "OK!" is a
# genuine pass/fail for the feature.
#   FARMULTI.CMD  far CALL across coalesced *_TEXT code groups (medium -mm -zm)
#   FARPTR.CMD    far DATA POINTER into a type-3 EXTRA group (compact far data)
# Provenance: built in-workspace (scratch/stageb) from the wlink CP/M-86 writer;
# vendored here (≤768 B each) so this test is self-contained.
#
# NOTE on DR C coexistence (guard-coordinated dual reloc): genuine DR C `.CMD`s
# ALSO set byte-127 bit 7, but their CLEARL crt0 self-relocates UNLESS a guard
# immediate (itself a fixup target) is made nonzero. Because emu2 now applies the
# fixups, that guard is set, so the crt0 walker stands down -> single relocation,
# no double-relocation crash. Verified manually against the DR C oracles in the
# workspace (LL_s/LL_l/MT_l, ~38 KB each -- too large to vendor here).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
EMU2="${EMU2:-$HERE/../../emu2}"
[ -x "$EMU2" ] || { echo "SKIP: emu2 binary not found at $EMU2 (run 'make' first)"; exit 77; }

fail=0
check() {                       # check <cmd-file> <expected-substring>
    local f="$HERE/$1" want="$2" out
    out="$("$EMU2" "$f" 2>/dev/null | tr -d '\r\000')"
    if printf '%s' "$out" | grep -q "$want"; then
        echo "PASS  $1  (relocated, got '$want')"
    else
        echo "FAIL  $1  expected '$want', got: $(printf '%s' "$out" | head -1)"
        fail=1
    fi
}

check FARMULTI.CMD OK!
check FARPTR.CMD   OK!

if [ "$fail" = 0 ]; then
    echo "P_LOAD relocation regression: ALL PASS"
else
    echo "P_LOAD relocation regression: FAILURES"
fi
exit $fail
