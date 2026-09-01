/* small.c -- CP/M-86 small-model smoke oracle.
 *
 * Memory model: SMALL (-mcmodel=s).  Separate CODE and DATA groups; all
 * pointers are near (intra-segment), so the linker emits NO P_LOAD fixup
 * records (hdr[0x7F] bit7 == 0).  This exercises the loader's non-relocating
 * path for a two-group CMD: code is loaded at cpm_code_seg:0, data at
 * cpm_data_seg:0, and the DATA group descriptor in the base page gives the
 * program access to msg[] via its near DS-relative address.
 *
 * Unlike the medium/compact oracles, small-model codegen references the
 * startup-provided `_small_code_` group symbol, so this one must be linked
 * against the real CP/M-86 C runtime (cstartcpm.obj + clibs.lib) -- hence a
 * `main` entry instead of the bare `cpmmain` the clib-free oracles use.
 *
 * Prints "OK!\r\n"; the C startup performs the warm-boot exit on return.
 * Build (Open Watcom, gcc-like driver):
 *   owcc -bcpm86 -mcmodel=s -fno-stack-check -o SMALL.CMD small.c
 */
extern unsigned bdos(unsigned char func, unsigned dx);
#pragma aux bdos =              \
    "int 0E0h"                  \
    parm [cl] [dx]              \
    value [ax]                  \
    modify [ax bx cx dx es];

char msg[] = "OK!\r\n";         /* anchors the DATA group (non-empty) */

int main(void)
{
    unsigned char *p;
    for (p = (unsigned char *)msg; *p; p++)
        bdos(2, *p);
    return 0;
}
