/* Stage B end-to-end far-call relocation oracle.
 *
 * Four leaf functions, each in its OWN <func>_TEXT segment under -mm -zm, of
 * DELIBERATELY different sizes: fK contains a loop so it spans >1 paragraph.
 * cpmmain (itself multi-paragraph) far-calls all four and writes each return
 * byte via BDOS C_WRITE.  The expected console output is exactly "OK!\r\n".
 *
 * This is a CORRECTNESS oracle, not a "did it run" check: each fN returns a
 * DISTINCT byte, so if wlink mislocates any far target's paragraph (the exact
 * bug fixed 2026-08-19 -- image paragraph derived from the packed layout, not
 * from wlink's grp_addr.seg frame numbers, which increment by 1 per segment
 * regardless of size) the call lands in the wrong function and a WRONG byte
 * (or a hang) results.  fK's multi-paragraph body guarantees the targets after
 * it sit at image paragraphs > 1, re-triggering that bug on any regression. */
extern unsigned bdos( unsigned char func, unsigned dx );
#pragma aux bdos =              \
    "int 0E0h"                  \
    parm [cl] [dx]              \
    value [ax]                  \
    modify [ax bx cx dx es];

char anchor = 1;                /* forces a DATA group (runner enters CS:0000) */

int fO( int x );
int fO( int x ) { return x + 'O'; }

int fK( int x );                /* loop body -> spans multiple paragraphs */
int fK( int x )
{
    int i;
    int s = 0;
    for( i = 0; i < x; ++i )
        s += 1;
    return s + ( 'K' - 4 );
}

int fBang( int x );
int fBang( int x ) { return x + '!'; }

int fNL( int x );
int fNL( int x ) { return ( x ^ 0 ) + '\n' - 1; }

void cpmmain( void )
{
    bdos( 2, fO( 0 ) );         /* 'O' */
    bdos( 2, fK( 4 ) );         /* 4 + ('K'-4) = 'K' */
    bdos( 2, fBang( 0 ) );      /* '!' */
    bdos( 2, 13 );              /* '\r' literal */
    bdos( 2, fNL( 1 ) );        /* 1 + '\n' - 1 = '\n' */
    bdos( 0, 0 );
}
