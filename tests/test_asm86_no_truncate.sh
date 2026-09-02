#!/bin/sh
# Test for johnsonjh/emu2-cpm86#23: ASM86 must not truncate input files

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
EMU2="${EMU2:-$HERE/../emu2}"
[ -x "$EMU2" ] || {
    printf '%s\n' "SKIP: emu2 binary not found at $EMU2 (run 'make' first)"
    exit 77
}

# Need asm86.cmd in share/cpm
if [ ! -f "$root/share/cpm/asm86.cmd" ]; then

    if [ -f "$HOME/cpm86-crossdev/share/cpm/asm86.cmd" ]; then
        root="$HOME/cpm86-crossdev"
    fi

    if [ -f "$HOME/src/cpm86-crossdev/share/cpm/asm86.cmd" ]; then
        root="$HOME/src/cpm86-crossdev"
    fi
fi

if [ -z "$root" ] || [ ! -f "$root/share/cpm/asm86.cmd" ]; then
    printf '%s\n' "SKIP: asm86.cmd not found"
    exit 0
fi

printf '%s\n' "Running ASM86 truncation regression..."

# Test file content from the GitHub issue
cat > /tmp/test_asm86_trunc.a86 <<'EOF'
M	EQU	Byte Ptr 0[BX]
; hello80.asm - Hello World in 8080 assembly
; Converted to 8086 with XLT86, then assembled with asm86
	ORG	100h
start:
	MOV	DX,(Offset msg)
	MOV	CL,9
	INT	224
	MOV	CL,0
	INT	224
msg	DB	'Hello from 8080','$'
	END	start
EOF

# Save original size
orig_size=$(wc -c < /tmp/test_asm86_trunc.a86)

# Run asm86 through emu2
EMU2_DRIVE_D="$root/share/cpm" EMU2_PROGNAME="d:\asm86.cmd" \
    "${EMU2:?}" "$root/share/cpm/asm86.cmd" /tmp/test_asm86_trunc.a86 > /dev/null 2>&1

# Check file was not truncated
final_size=$(wc -c < /tmp/test_asm86_trunc.a86)

if [ "$orig_size" -eq "$final_size" ]; then
    printf '%s\n' "PASS: Input file not truncated ($orig_size bytes)"
else
    printf '%s\n' "FAIL: Input file truncated from $orig_size to $final_size bytes"
    exit 1
fi

# Cleanup
rm -f /tmp/test_asm86_trunc.a86 /tmp/test_asm86_trunc.h86 /tmp/test_asm86_trunc.lst /tmp/test_asm86_trunc.sym

exit 0
