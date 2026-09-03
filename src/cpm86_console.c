// DOS-PLUS (CP/M-86 4.1, not MS-DOS) console emulation for native CP/M-86
// programs.  See cpm86_console.h for the overview.
//
// Sequences interpreted:
//   VT52:  ESC A/B/C/D (cursor up/down/right/left), ESC H (home), ESC I (reverse
//          line feed), ESC J (erase to end of screen), ESC K (erase to end of
//          line), ESC Y r c (direct cursor address, each byte + 0x20), ESC E
//          (clear screen + home).
//   DRI:   ESC b <n> (set foreground colour to PC colour index n), ESC c <n>
//          (set background colour), ESC x / ESC y (mode set/reset -- accepted and
//          ignored; they carry no visible effect we model).
//   ANSI:  ESC [ <params> <final>, with final H/f (cursor address), A/B/C/D
//          (cursor move), J (erase: 2 = whole screen, else to end of screen), K
//          (erase to end of line), m (SGR colour/attribute).  Other CSI sequences
//          (e.g. ESC[?7l autowrap) are accepted and ignored.
//
// Output is dual: when video.c is active each sequence drives its cursor/erase/
// colour primitives; otherwise the equivalent ANSI is written to the host
// terminal (and ANSI input is passed straight through, since the host speaks it).

#include "cpm86_console.h"
#include "dos.h"   // dos_console_putc
#include "video.h" // video_active + video_* primitives

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

// Console interpretation is on by default; EMU2_CPM_VT52=0|off|no|false disables it.
static int console_enabled(void)
{
    static int en = -1;
    if(en < 0)
    {
        const char *v = getenv("EMU2_CPM_VT52");
        en = !(v && (!strcasecmp(v, "0") || !strcasecmp(v, "off") ||
                     !strcasecmp(v, "no") || !strcasecmp(v, "false")));
    }
    return en;
}

// PC colour (0..15) -> ANSI SGR foreground / background code (8..15 use the
// bright 90../100.. range), for the host-terminal (video-inactive) path.
static const int pc_fg[16] = {30, 34, 32, 36, 31, 35,  33,  37,
                              90, 94, 92, 96, 91, 95,  93,  97};
static const int pc_bg[16] = {40, 44, 42, 46, 41, 45,  43,  47,
                              100, 104, 102, 106, 101, 105, 103, 107};

static void emit(const char *s)
{
    while(*s)
        dos_console_putc((uint8_t)*s++);
}

// --- console operations: drive video.c when active, else emit ANSI -----------
static void op_goto(unsigned x, unsigned y)
{
    if(video_active())
        video_set_cursor(x, y);
    else
    {
        char b[24];
        snprintf(b, sizeof b, "\x1b[%u;%uH", y + 1, x + 1);
        emit(b);
    }
}
static void op_move(char dir, unsigned n)
{
    if(!n)
        n = 1;
    if(video_active())
    {
        unsigned x, y;
        video_get_cursor(&x, &y);
        switch(dir)
        {
        case 'A': video_set_cursor(x, y > n ? y - n : 0); break;
        case 'B': video_set_cursor(x, y + n); break;
        case 'C': video_set_cursor(x + n, y); break;
        case 'D': video_set_cursor(x > n ? x - n : 0, y); break;
        }
    }
    else
    {
        char b[16];
        snprintf(b, sizeof b, "\x1b[%u%c", n, dir);
        emit(b);
    }
}
static void op_erase_eol(void)
{
    if(video_active())
        video_erase_eol();
    else
        emit("\x1b[K");
}
static void op_erase_eos(void)
{
    if(video_active())
        video_erase_eos();
    else
        emit("\x1b[J");
}
static void op_clear(void)
{
    if(video_active())
        video_clear_screen();
    else
        emit("\x1b[2J\x1b[H");
}
static void op_reverse_lf(void)
{
    if(video_active())
        video_reverse_lf();
    else
        emit("\x1bM");
}
static void op_fg(unsigned c)
{
    c &= 15;
    if(video_active())
        video_set_attr((video_get_attr() & 0xF0) | c);
    else
    {
        char b[16];
        snprintf(b, sizeof b, "\x1b[%dm", pc_fg[c]);
        emit(b);
    }
}
static void op_bg(unsigned c)
{
    c &= 15;
    if(video_active())
        video_set_attr((video_get_attr() & 0x0F) | (c << 4));
    else
    {
        char b[16];
        snprintf(b, sizeof b, "\x1b[%dm", pc_bg[c]);
        emit(b);
    }
}

// ANSI colour index (0..7, the SGR code minus 30/40) -> PC colour index.
static const uint8_t ansi_to_pc[8] = {0, 4, 2, 6, 1, 5, 3, 7};

// Apply one ANSI SGR parameter to the emulated screen's attribute (video path).
static void sgr_apply(unsigned p)
{
    uint8_t a = video_get_attr();
    if(p == 0)
        a = 0x07; // reset to light-grey on black
    else if(p == 1)
        a |= 0x08; // bold -> bright foreground
    else if(p == 7)
        a = ((a & 0x0F) << 4) | ((a >> 4) & 0x0F); // reverse
    else if(p >= 30 && p <= 37)
        a = (a & 0xF8) | ansi_to_pc[p - 30];
    else if(p >= 90 && p <= 97)
        a = (a & 0xF0) | 0x08 | ansi_to_pc[p - 90];
    else if(p >= 40 && p <= 47)
        a = (a & 0x8F) | (ansi_to_pc[p - 40] << 4);
    else if(p >= 100 && p <= 107)
        a = (a & 0x0F) | 0x80 | (ansi_to_pc[p - 100] << 4);
    video_set_attr(a);
}

// --- parser state ------------------------------------------------------------
static enum cons_state
{
    C_NORMAL, // not in a sequence
    C_ESC,    // saw ESC
    C_VY_ROW, // ESC Y, awaiting row byte
    C_VY_COL, // ESC Y <row>, awaiting column byte
    C_DRI_FG, // ESC b, awaiting colour byte
    C_DRI_BG, // ESC c, awaiting colour byte
    C_CSI     // ESC [, collecting parameters
} state = C_NORMAL;

static char csi[32]; // raw CSI bytes from '[' onward (for host passthrough)
static unsigned csi_len;

// Interpret a finished CSI sequence on the emulated screen (video path).
static void csi_apply(char final)
{
    // Parse up to a few decimal parameters from csi[] (skip a leading '?').
    unsigned p[4] = {0, 0, 0, 0}, np = 0;
    int have = 0;
    for(unsigned i = 0; i < csi_len; i++)
    {
        char c = csi[i];
        if(c >= '0' && c <= '9')
        {
            p[np] = p[np] * 10 + (c - '0');
            have = 1;
        }
        else if(c == ';')
        {
            if(np < 3)
                np++;
            have = 0;
        }
        else if(c == '?')
            return; // private mode (e.g. ?7l autowrap): ignore on the screen
    }
    if(have || np)
        np++;
    switch(final)
    {
    case 'H':
    case 'f': op_goto(p[1] ? p[1] - 1 : 0, p[0] ? p[0] - 1 : 0); break;
    case 'A':
    case 'B':
    case 'C':
    case 'D': op_move(final, p[0]); break;
    case 'J': p[0] == 2 ? op_clear() : op_erase_eos(); break;
    case 'K': op_erase_eol(); break;
    case 'm':
        if(np == 0)
            sgr_apply(0);
        else
            for(unsigned i = 0; i < np; i++)
                sgr_apply(p[i]);
        break;
    default: break; // unhandled CSI: ignore
    }
}

int cpm_console_putch(char ch)
{
    uint8_t c = (uint8_t)ch;

    if(!console_enabled())
        return 0;

    switch(state)
    {
    case C_NORMAL:
        if(c == 0x1B)
        {
            state = C_ESC;
            return 1;
        }
        return 0; // ordinary character -> caller prints it

    case C_ESC:
        state = C_NORMAL;
        switch(c)
        {
        case '[': state = C_CSI; csi_len = 0; return 1;
        case 'Y': state = C_VY_ROW; return 1;
        case 'b': state = C_DRI_FG; return 1;
        case 'c': state = C_DRI_BG; return 1;
        case 'A':
        case 'B':
        case 'C':
        case 'D': op_move(c, 1); return 1;
        case 'H': op_goto(0, 0); return 1;
        case 'I':
        case 'M': op_reverse_lf(); return 1;
        case 'J': op_erase_eos(); return 1;
        case 'K': op_erase_eol(); return 1;
        case 'E': op_clear(); return 1;
        case 'x':
        case 'y': return 1; // DRI mode set/reset: accepted, no modelled effect
        default:
            // Unknown ESC <c>: pass through to the host when it owns the screen.
            if(!video_active())
            {
                dos_console_putc(0x1B);
                dos_console_putc(c);
            }
            return 1;
        }

    case C_VY_ROW:
        // VT52 row = byte - 0x20; stash it (reuse csi[0..1] as scratch).
        csi[0] = (c >= 0x20) ? (c - 0x20) : 0;
        state = C_VY_COL;
        return 1;
    case C_VY_COL:
        op_goto((c >= 0x20) ? (unsigned)(c - 0x20) : 0, (uint8_t)csi[0]);
        state = C_NORMAL;
        return 1;

    case C_DRI_FG:
        op_fg(c);
        state = C_NORMAL;
        return 1;
    case C_DRI_BG:
        op_bg(c);
        state = C_NORMAL;
        return 1;

    case C_CSI:
        if(csi_len < sizeof csi)
            csi[csi_len++] = (char)c;
        // A final byte (0x40..0x7E) terminates the CSI sequence.
        if(c >= 0x40 && c <= 0x7E)
        {
            if(video_active())
            {
                csi_len--; // drop the final byte from the parameter buffer
                csi_apply((char)c);
            }
            else
            {
                // Host speaks ANSI: pass the whole sequence through verbatim.
                dos_console_putc(0x1B);
                dos_console_putc('[');
                for(unsigned i = 0; i < csi_len; i++)
                    dos_console_putc((uint8_t)csi[i]);
            }
            state = C_NORMAL;
        }
        return 1;
    }
    return 1;
}
