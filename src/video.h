#pragma once

#include <stdint.h>

void intr10(void);
// Redraws terminal screen
void check_screen(void);
// Returns 1 if video emulation is active.
int video_active(void);
// Writes a character to the video screen
void video_putch(char ch);
// Get current column in current page
int video_get_col(void);
// CRTC port read/write
uint8_t video_crtc_read(int port);
void video_crtc_write(int port, uint8_t value);
// Initializes emulated video memory and tables
void video_init_mem(void);

// Text-screen cursor/erase access used by the VT52 console layer (cpm86_vt52.c):
void video_get_cursor(unsigned *x, unsigned *y); // cursor position (active page)
void video_set_cursor(unsigned x, unsigned y);   // set cursor, clamped to screen
void video_get_size(unsigned *cols, unsigned *rows);
void video_erase_eol(void);    // blank from the cursor to the end of the line
void video_erase_eos(void);    // blank from the cursor to the end of the screen
void video_reverse_lf(void);   // cursor up one line, scrolling down at the top
uint8_t video_get_attr(void);  // current text attribute (PC colour byte)
void video_set_attr(uint8_t attr);
void video_clear_screen(void); // blank the whole screen and home the cursor
