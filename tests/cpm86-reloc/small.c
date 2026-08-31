/* small.c -- CP/M-86 small-model smoke oracle.
 *
 * Memory model: SMALL (-ms -zm).  Separate CODE and DATA groups; all pointers
 * are near (intra-segment), so the linker emits NO P_LOAD fixup records
 * (hdr[0x7F] bit7 == 0).  This exercises the loader's non-relocating path for
 * a two-group CMD: code is loaded at cpm_code_seg:0, data at cpm_data_seg:0,
 * and the DATA group descriptor in the base page gives the program access to
 * msg[] via its near DS-relative address.
 *
 * Prints "OK!\r\n" and exits via BDOS 0 (system reset / warm boot trap).
 * Build: wcc86 small.c -ms -zm -s -os -w4 && wlink format cpm86 name SMALL.CMD
 *        option nodefaultlibs, map file small.obj
 */
extern unsigned bdos(unsigned char func, unsigned dx);
#pragma aux bdos =              \
    "int 0E0h"                  \
    parm [cl] [dx]              \
    value [ax]                  \
    modify [ax bx cx dx es];

char msg[] = "OK!\r\n";         /* anchors the DATA group (non-empty) */

void cpmmain(void)
{
    unsigned char *p;
    for (p = (unsigned char *)msg; *p; p++)
        bdos(2, *p);
    bdos(0, 0);
}
