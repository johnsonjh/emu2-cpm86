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

// TPA size override from "-m <kb>"; 0 = not set.
extern unsigned cpm86_tpa_kb_cli;

// TPA size in KB: "-m" > EMU2_CPM_TPA env var > ~640K default.
unsigned cpm86_get_tpa_kb(void);

// Poison byte from "-P <byte>"; -1 = not set (fall back to EMU2_CPM_POISON env var).
extern int cpm86_poison_cli;

// "-D" flag: fill free memory with 0xFF before loading; 0 = not set.
extern int cpm86_dirty_cli;

// INT 28h handler for CP/M-86 programs: a keyboard-poll interface (DI=4) used by
// some interpreters (e.g. ZORK).  Invoked from bios_routine() when cpm86_active.
void intr_cpm_int28(void);
