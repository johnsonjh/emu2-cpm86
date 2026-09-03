#pragma once

#include <stdint.h>
#include <stdio.h>

// EXE loader
uint16_t create_PSP(const char *cmdline, const char *environment, uint16_t env_size,
                    const char *progname);
// Parse a command tail into two default FCBs (drive + 11-char name).
void cmdline_to_fcb(const char *cmd_line, uint8_t *fcb1, uint8_t *fcb2);
unsigned get_current_PSP(void);
void set_current_PSP(uint16_t psp_seg);

// DOS Memory handling
uint16_t mem_resize_segment(uint16_t seg, uint16_t size);
void mem_free_segment(uint16_t seg);
uint16_t mem_alloc_segment(uint16_t size, uint16_t *max);
uint8_t mem_get_alloc_strategy(void);
void mem_set_alloc_strategy(uint8_t s);

// Init internal memory handling
void mcb_init(uint16_t mem_start, uint16_t mem_end);

// Fill all free MCB regions with a poison byte (EMU2_CPM_POISON).
void mem_poison_free(uint8_t val);

// Loaders
int dos_load_exe(FILE *f, uint16_t psp_mcb);
int dos_read_overlay(FILE *f, uint16_t load_seg, uint16_t reloc_seg);
