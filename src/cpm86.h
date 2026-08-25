#pragma once
// Native CP/M-86 support for emu2: .CMD loader + BDOS (INT 0E0h) dispatcher.
//
// CP/M-86 programs are loaded from .CMD files (a header of group descriptors
// describing code/data/extra/stack groups) and call the operating system
// through INT 0E0h with the function number in CL.  This lets emu2 run .cmd
// programs directly, without the cpm86.exe DOS shim.

#include <stdio.h>

// Returns non-zero if (f, name) looks like a CP/M-86 .CMD program.  Rewinds f.
int cpm86_detect(FILE *f, const char *name);

// Loads a CP/M-86 .CMD program and sets up CPU state to enter it.
// Returns non-zero on success.
int cpm86_load_cmd(FILE *f, const char *cmdline);

// BDOS entry point, invoked from bios_routine() on guest INT 0E0h.
void intr_cpm_bdos(void);

// Non-zero while a CP/M-86 program is loaded (vs a DOS program).
extern int cpm86_active;

// Command-line override (in KB) for the CP/M-86 TPA size, set by main.c's
// "-m" option (0 = not given; falls back to CPM86_TPA_KB env var, then the
// built-in default). Read via cpm86_get_tpa_kb().
extern unsigned cpm86_tpa_kb_cli;

// Single source of truth for the CP/M-86 TPA size in KB: "-m" CLI option >
// CPM86_TPA_KB env var > built-in default (calibrated to match real MAME).
// Used both by cpm86_load_cmd()'s group grant and dos.c's init_dos() MCB-pool
// sizing, so they always agree on the same ceiling.
unsigned cpm86_get_tpa_kb(void);

// INT 28h handler for CP/M-86 programs: a keyboard-poll interface (DI=4) used by
// some interpreters (e.g. ZORK).  Invoked from bios_routine() when cpm86_active.
void intr_cpm_int28(void);
