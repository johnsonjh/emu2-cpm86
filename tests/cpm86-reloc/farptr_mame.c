/* farptr_mame.c -- wlink Stage B (-mm -zm) pointer-to-code-stub relocation
 * oracle, instrumented to run headless on real CCP/M-86 in MAME rc759.
 *
 * Same checks as the committed contrib/ravn/test_stageb_farptr.c, but built by
 * the NATIVE wlink path (wcc -mm -zm + wlink format cpm86) so it exercises
 * WLINK'S OWN emitted CP/M-86 fixup records on real hardware -- not DR C's.
 * Entry/stack/base-page come from crt759.asm (which calls this cpmmain).
 *
 * Both oracles are made visible on the console (BDOS C_WRITE) so the screen is
 * the authoritative result, and the pass/fail is also signalled to the MAME
 * host on the undecoded port 0x2FE (mame-tests/done_signal.lua):
 *   (a) VALUE : print the char RETURNED by calling through each relocated far
 *       pointer -- "OK!" means all four far calls landed on the right stub.
 *   (b) MEMORY: print '.' when the first code byte at each far pointer is a real
 *       relocated opcode (0xB8 = `mov ax,imm16`, the stub prologue) -- "...."
 *       means all four pointers address the exact expected relocated code.
 * Correct relocation -> "OK!\r\n....\r\n" and DONE-SIGNAL word 0x0008.
 */
extern unsigned bdos( unsigned char func, unsigned dx );
#pragma aux bdos =              \
    "int 0E0h"                  \
    parm [cl] [dx]              \
    value [ax]                  \
    modify [ax bx cx dx es];

extern void mame_done( unsigned status );
#pragma aux mame_done =         \
    "mov dx,02FEh"              \
    "out dx,ax"                 \
    parm [ax]                   \
    modify [dx];

int fO( void );    int fO( void )    { return 0x114F; }   /* B8 4F 11 -> 'O'  */
int fK( void );    int fK( void )    { return 0x224B; }   /* B8 4B 22 -> 'K'  */
int fBang( void ); int fBang( void ) { return 0x3321; }   /* B8 21 33 -> '!'  */
int fCR( void );   int fCR( void )   { return 0x440D; }   /* B8 0D 44 -> '\r' */

static unsigned char __far * const stubs[4] = {
    (unsigned char __far *)fO,
    (unsigned char __far *)fK,
    (unsigned char __far *)fBang,
    (unsigned char __far *)fCR
};
static int (* const fns[4])( void ) = { fO, fK, fBang, fCR };
static const unsigned magic[4] = { 0x114F, 0x224B, 0x3321, 0x440D };

void cpmmain( void );
void cpmmain( void )
{
    int i;
    int fail = 0;

    for( i = 0; i < 4; ++i ) {                  /* (a) VALUE: call via far ptr */
        int c = fns[i]();
        if( c != (int)magic[i] ) ++fail;
        bdos( 2, (unsigned char)( magic[i] & 0xFF ) );   /* O K ! CR */
    }
    bdos( 2, 10 );                              /* '\n' */

    for( i = 0; i < 4; ++i ) {                  /* (b) MEMORY: byte at far ptr */
        unsigned char __far *p = stubs[i];
        unsigned m = magic[i];
        if( p[0] == 0xB8
         && p[1] == (unsigned char)m
         && p[2] == (unsigned char)( m >> 8 ) ) {
            bdos( 2, '.' );
        } else {
            bdos( 2, '?' );
            ++fail;
        }
    }
    bdos( 2, 13 ); bdos( 2, 10 );               /* '\r\n' */

    mame_done( fail == 0 ? 0x0008 : 0x00FF );   /* MAME host signal (last) */
}
