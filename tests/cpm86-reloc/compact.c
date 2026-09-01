/* Pointer-to-code-stub relocation oracle.
   Each stub's body is `return 0xHHLL;` -> Watcom emits `mov ax,0xHHLL; retf`,
   so the stub's first three code bytes are B8 LL HH -- a self-describing magic.
   A DATA table of FAR pointers to the stubs makes each seg word a FIX_BASE
   fixup whose LOCATION is in DATA (record nibble 0x2X, target CODE) -- a path
   the call-based test never exercises.  cpmmain follows each relocated pointer
   and checks the code bytes match the stub's magic; it prints the magic's low
   byte on a match, '?' otherwise.  Correct relocation -> "OK!\n". */
extern unsigned bdos( unsigned char func, unsigned dx );
#pragma aux bdos =              \
    "int 0E0h"                  \
    parm [cl] [dx]              \
    value [ax]                  \
    modify [ax bx cx dx es];

int sO( void );   int sO( void )   { return 0x114F; }   /* 'O' */
int sK( void );   int sK( void )   { return 0x224B; }   /* 'K' */
int sBang(void);  int sBang( void ){ return 0x3321; }   /* '!' */
int sNL( void );  int sNL( void )  { return 0x440A; }   /* '\n' */

/* far pointer table in DATA: each seg word is a load-time fixup (loc=DATA) */
static unsigned char __far * const stubs[4] = {
    (unsigned char __far *)sO,
    (unsigned char __far *)sK,
    (unsigned char __far *)sBang,
    (unsigned char __far *)sNL
};
static const unsigned magic[4] = { 0x114F, 0x224B, 0x3321, 0x440A };

void cpmmain( void )
{
    int i;
    for( i = 0; i < 4; ++i ) {
        unsigned char __far *p = stubs[i];
        unsigned m = magic[i];
        if( p[0] == 0xB8 && p[1] == (unsigned char)m && p[2] == (unsigned char)( m >> 8 ) )
            bdos( 2, (unsigned char)m );        /* expected low byte */
        else
            bdos( 2, '?' );
    }
    bdos( 0, 0 );
}
