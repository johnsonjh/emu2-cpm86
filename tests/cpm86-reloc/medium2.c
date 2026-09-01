extern unsigned bdos( unsigned char func, unsigned dx );
#pragma aux bdos =              \
    "int 0E0h"                  \
    parm [cl] [dx]              \
    value [ax]                  \
    modify [ax bx cx dx es];

char gv = 5;                 /* forces a non-empty DATA group */

int add7( int x );
int add7( int x ) { return x + 7; }

void cpmmain( void )
{
    bdos( 2, add7( 'A' - 7 + gv - gv ) );
    bdos( 2, add7( '\r' - 7 ) );
    bdos( 2, add7( '\n' - 7 ) );
    bdos( 0, 0 );
}
