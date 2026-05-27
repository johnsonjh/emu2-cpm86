#pragma once

#include "os.h"

#include <stdint.h>
#include <stdio.h>

void init_dos(int argc, char **argv);
int dos_chmod_fcb(int fcb_addr, int make_readonly);
int dos_truncate_fcb(int fcb_addr, unsigned long length);
uint32_t get_static_memory(uint16_t bytes, uint16_t align);
// Raw byte to the console sink (no CP/M-86 console translation); see dos.c.
void dos_console_putc(uint8_t ch);
NORETURN void intr20(void);
void intr21(void);
void intr2f(void);
NORETURN void intr22(void);
void intr28(void);
void intr29(void);
