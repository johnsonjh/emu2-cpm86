/*
 * drc_farptr.c -- pointer-to-code-stub relocation oracle, DR C 1.11 LARGE model.
 *
 * The genuine "how it SHOULD be done" reference for the wlink Stage B test
 * contrib/ravn/test_stageb_farptr.c.  DR C's default LARGE model is exactly
 * far-code/far-data: function pointers are 32-bit FAR, and LINK-86 emits the
 * CP/M-86 fixup records the loader relocates -- the same load-time-relocation
 * contract wlink now targets.  So a genuine-reference-compiler build that
 * follows relocated far pointers into code stubs is the authoritative check
 * that the pointer-memory oracle is sound on real CCP/M-86.
 *
 * K&R C89 (DR C v1.11): old-style params, no mid-block decls, no unsigned-char
 * casts (Error 13 -> plain char), main() is the entry.  putchar() = far libc
 * call (DRC_PUTCHAR).  mame_ok/mame_bad = no-arg far OUT-0x2FE signals
 * (done-far.asm via DRC_MAMEMARK) so MAME stops the instant the checks finish.
 *
 * BOTH oracles are made VISIBLE on the console so the on-screen text is the
 * authoritative result (no reliance on a status word):
 *   loop (a) VALUE oracle : print the char RETURNED by calling through each
 *            relocated far pointer -- "OK!" + newline means all four far calls
 *            landed on the right stub (a mislocated pointer prints a wrong char).
 *   loop (b) MEMORY oracle: print '.' when the first code byte at each far
 *            pointer is a real relocated opcode (non-zero), '?' otherwise --
 *            "...." means all four pointers address real code.
 * Correct relocation -> "OK!\n....\n" and mame_ok (DONE-SIGNAL word 0x0008).
 */

int putchar();
extern void mame_ok();
extern void mame_bad();

int sO()    { return 'O';  }
int sK()    { return 'K';  }
int sBang() { return '!';  }
int sNL()   { return '\n'; }

int (*fns[4])() = { sO, sK, sBang, sNL };
int wants[4]    = { 'O', 'K', '!', '\n' };

main()
{
    int i;
    int (*fp)();
    char *code;
    int c;
    int fail;

    fail = 0;

    for (i = 0; i < 4; i++) {           /* (a) VALUE oracle: call via far ptr */
        fp = fns[i];
        c = (*fp)();
        if (c != wants[i]) fail++;
        putchar(c);                     /* print the RETURNED char */
    }

    for (i = 0; i < 4; i++) {           /* (b) MEMORY oracle: byte at far ptr */
        code = (char *) fns[i];
        if (code[0] != 0) putchar('.');
        else            { putchar('?'); fail++; }
    }
    putchar('\n');

    if (fail == 0) mame_ok();
    else           mame_bad();
    return 0;
}
