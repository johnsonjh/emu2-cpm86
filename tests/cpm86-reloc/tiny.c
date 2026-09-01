/* two functions; -mm -zm puts each in its own <func>_TEXT segment.
   main_ makes a FAR call to callee_ across segments. */
int callee_( int x );
int callee_( int x ) { return x + 7; }

int main_( void );
int main_( void ) { return callee_( 35 ); }
