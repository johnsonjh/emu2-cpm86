#pragma once

// DOS-PLUS (CP/M-86 4.1) console emulation for native CP/M-86 programs.
//
// DOS-PLUS presented its console as a small terminal that understood VT52 cursor
// codes, DRI colour codes, and ANSI/VT100 codes.  emu2 has two console sinks:
// the emulated PC text screen (video.c, used once a program touches INT 10h) and,
// for programs that only use the BDOS console, the host terminal directly.  This
// module interprets the console control sequences and applies them to whichever
// sink is active -- driving video.c when it is initialised, otherwise emitting the
// equivalent ANSI to the host terminal.
//
// Feed every console-output byte to cpm_console_putch() (the caller does this only
// while cpm86_active).  Returns 1 when the byte was consumed as part of a control
// sequence, or 0 when it is an ordinary character the caller should print itself.
// Disabled with EMU2_CPM_VT52=0.
int cpm_console_putch(char ch);
