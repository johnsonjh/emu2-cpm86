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
# Oracles are named for the CP/M-86 memory model they exercise:
#   MEDIUM.CMD   far CALL across coalesced *_TEXT code groups (medium -mm -zm)
#   MEDIUM2.CMD  minimal single far-call smoke (medium -mm -zm)
#   COMPACT.CMD  far DATA POINTER into a type-3 EXTRA group (compact far data)
#   TINY.CMD     8080 single-group (data==0) load-smoke
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

# Read one byte from a file at a given offset; print as lowercase hex (no 0x).
byte_at() { od -An -tx1 -j "$1" -N1 "$2" | tr -d ' \n'; }

# check_header <cmd-file> <g0-type> <g1-type> <g1-has-data: 0|1> <reloc-bit: 0|1>
# Checks CMD header group types and the relocation flag in hdr[0x7F].
check_header() {
    local f="$HERE/$1" g0_want="$2" g1_want="$3" data_want="$4" reloc_want="$5"
    local ok=1

    g0=$(byte_at 0 "$f")
    if [ "$g0" != "$g0_want" ]; then
        echo "FAIL  $1  group[0].type: got $g0, want $g0_want (CODE=01)"
        ok=0
    fi

    g1=$(byte_at 9 "$f")
    if [ "$g1" != "$g1_want" ]; then
        echo "FAIL  $1  group[1].type: got $g1, want $g1_want (DATA=02)"
        ok=0
    fi

    # group[1].length at bytes 10-11 (LE): non-zero iff the DATA group has content
    g1lo=$(byte_at 10 "$f"); g1hi=$(byte_at 11 "$f")
    has_data=$(( 0x$g1lo != 0 || 0x$g1hi != 0 ))
    if [ "$has_data" != "$data_want" ]; then
        echo "FAIL  $1  group[1] data: has_data=$has_data, want $data_want"
        ok=0
    fi

    flags=$(byte_at 127 "$f")
    reloc=$(( (0x$flags & 0x80) != 0 ? 1 : 0 ))
    if [ "$reloc" != "$reloc_want" ]; then
        echo "FAIL  $1  hdr[0x7F] reloc bit: got $reloc, want $reloc_want (flags=$flags)"
        ok=0
    fi

    if [ "$ok" = 1 ]; then
        echo "PASS  $1  header (g0=$g0 g1=$g1 data=$has_data reloc=$reloc)"
    else
        fail=1
    fi
}

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

check_load() {                  # check_load <cmd-file> -- load-smoke (no output expected)
    local f="$HERE/$1"
    "$EMU2" "$f" 2>/dev/null
    local rc=$?
    if [ "$rc" -le 128 ]; then
        echo "PASS  $1  (loaded, exit rc=$rc)"
    else
        echo "FAIL  $1  crashed (rc=$rc)"
        fail=1
    fi
}

# MEDIUM:  CODE group + DATA group (1-byte anchor), relocation table present
check_header MEDIUM.CMD   01  02  1  1
# COMPACT: CODE group + DATA group (far-pointer table), relocation table present
check_header COMPACT.CMD  01  02  1  1
# MEDIUM2: CODE group + DATA group (1-byte anchor), relocation table present
check_header MEDIUM2.CMD  01  02  1  1
# SMALL: CODE + DATA groups, near/near -- NO P_LOAD fixups (reloc bit 0). Exercises
# the loader's non-relocating two-group path.
check_header SMALL.CMD    01  02  1  0
# TINY: CODE-only (medium-model, no DATA group, data==0), relocation table present.
# The loader takes the data==0 path (model_8080), which still exercises that code path.
# Note: TINY has no bdos(0,0) call; exit code is implementation-defined (not checked).
check_header TINY.CMD     01  00  0  1

check MEDIUM.CMD  OK!
check COMPACT.CMD OK!
check MEDIUM2.CMD A
check SMALL.CMD   OK!
check_load TINY.CMD

if [ "$fail" = 0 ]; then
    echo "P_LOAD relocation regression: ALL PASS"
else
    echo "P_LOAD relocation regression: FAILURES"
fi
exit $fail
