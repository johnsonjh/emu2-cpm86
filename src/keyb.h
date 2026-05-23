#pragma once
#include <stdint.h>

void update_keyb(void);
int getch(int detect_brk);
int kbhit(void);
// Raw serial-console input (EMU2_SERIAL_CONSOLE): untranslated terminal bytes.
int serial_con_status(void); // 0xFF if a byte is ready, else 0
int serial_con_getc(void);   // one raw byte (blocking), -1 on EOF
void intr16(void);
uint8_t keyb_read_port(unsigned port);
void keyb_write_port(unsigned port, uint8_t value);
void suspend_keyboard(void);
// Disable throttling the next keyboard calls
void keyb_wakeup(void);
void keyb_handle_irq(void);
