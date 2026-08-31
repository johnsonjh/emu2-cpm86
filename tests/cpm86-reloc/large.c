/* large.c -- CP/M-86 large-model far-pointer relocation oracle.
 *
 * Memory model: LARGE (-ml -zm).  Both code and data are FAR: function
 * pointers are 32-bit (seg:off), and the linker emits P_LOAD fixup records
 * (hdr[0x7F] bit7 == 1) so that the loader patches each far code pointer with
 * the actual load segment.  Calling through the relocated pointer verifies
 * end-to-end large-model load-time relocation.
 *
 * Prints "OK!\r\n" and exits via BDOS 0.
 * Build: wcc86 large.c -ml -zm -s -os -w4 && wlink format cpm86 name LARGE.CMD
 *        option nodefaultlibs, map file large.obj
 */
extern unsigned bdos(unsigned char func, unsigned dx);
#pragma aux bdos =              \
    "int 0E0h"                  \
    parm [cl] [dx]              \
    value [ax]                  \
    modify [ax bx cx dx es];

/* Three tiny stubs in separate _TEXT segments (-zm): each returns one char.
 * In large model, function pointers are FAR (seg:off), so the linker writes
 * the group-relative segment and emits a P_LOAD fixup for each entry in fns[].
 * The loader must relocate these before the far calls can land on the right stub. */
static int fO(void);    static int fO(void)    { return 'O'; }
static int fK(void);    static int fK(void)    { return 'K'; }
static int fBang(void); static int fBang(void) { return '!'; }

static int (* __far fns[3])(void) = { fO, fK, fBang };

void cpmmain(void)
{
    int i;
    for (i = 0; i < 3; i++)
        bdos(2, fns[i]());          /* far call through relocated pointer */
    bdos(2, '\r');
    bdos(2, '\n');
    bdos(0, 0);
}
