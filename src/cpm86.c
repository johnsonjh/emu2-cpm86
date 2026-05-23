#define _GNU_SOURCE

#include "cpm86.h"
#include "dbg.h"
#include "dos.h"
#include "emu.h"
#include "keyb.h"
#include "loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>

// ===========================================================================
// CP/M-86 .CMD format
// ===========================================================================
// A .CMD file starts with a 128-byte header of up to 8 "group descriptors",
// 9 bytes each, describing the program's memory groups.  Group data (for the
// code and data groups) follows the header in the file, in group order.
//
//   Group Descriptor (9 bytes), all sizes in 16-byte paragraphs:
//     +0  g_type   1=code 2=data 3=extra 4=stack 5..8=aux 9=escape (0=unused)
//     +1  g_length length of group as stored in the file
//     +3  a_base   absolute base paragraph (for absolute groups)
//     +5  g_min    minimum group size to allocate
//     +7  g_max    maximum group size

#define CMD_HDR_SIZE 128
#define CMD_NGROUPS 8
#define GD_SIZE 9

enum cpm_gtype
{
    GT_UNUSED = 0,
    GT_CODE = 1,
    GT_DATA = 2,
    GT_EXTRA = 3,
    GT_STACK = 4,
    GT_ESCAPE = 9
};

struct cpm_group
{
    uint8_t type;
    uint16_t length; // paragraphs stored in file
    uint16_t base;   // absolute base paragraph
    uint16_t min;    // paragraphs to allocate (min)
    uint16_t max;    // paragraphs to allocate (max)
    uint32_t file_off; // byte offset of this group's data in the file
};

// Set once a CP/M-86 program is loaded (see cpm86.h).
int cpm86_active = 0;

// Parsed program image (segments are emu2 paragraph addresses).
static uint16_t cpm_code_seg, cpm_data_seg, cpm_base_seg;
// CP/M-86 DMA (disk transfer) address is segment:offset: BDOS 26 sets the
// offset, BDOS 51 the segment.  Defaults to base page 0x80.
static uint16_t cpm_dma_seg, cpm_dma_off;

// ---------------------------------------------------------------------------
// Header parsing
// ---------------------------------------------------------------------------
static int parse_header(const uint8_t *hdr, struct cpm_group *g, int *ngroups)
{
    uint32_t fpos = CMD_HDR_SIZE;
    int n = 0;
    for(int i = 0; i < CMD_NGROUPS; i++)
    {
        const uint8_t *d = hdr + i * GD_SIZE;
        uint8_t type = d[0];
        if(type == GT_UNUSED)
            continue;
        if(type > GT_ESCAPE)
            return 0; // not a valid CMD
        g[n].type = type;
        g[n].length = d[1] | (d[2] << 8);
        g[n].base = d[3] | (d[4] << 8);
        g[n].min = d[5] | (d[6] << 8);
        g[n].max = d[7] | (d[8] << 8);
        // Groups carry their stored content (g_length paragraphs) in group
        // order; advance past it whether or not the group is code/data.
        g[n].file_off = fpos;
        fpos += (uint32_t)g[n].length * 16;
        n++;
    }
    *ngroups = n;
    return n > 0;
}

// ---------------------------------------------------------------------------
// Detection: extension ".cmd", or a clean group-descriptor header.
// ---------------------------------------------------------------------------
int cpm86_detect(FILE *f, const char *name)
{
    uint8_t hdr[CMD_HDR_SIZE];
    int is_cmd = 0;

    if(fread(hdr, 1, CMD_HDR_SIZE, f) == CMD_HDR_SIZE)
    {
        // Reject DOS MZ executables outright.
        if(hdr[0] != 'M' || hdr[1] != 'Z')
        {
            // First descriptor must be a code group with a non-zero length,
            // and the header tail (after the 8 descriptors) must be zero.
            int ok = (hdr[0] == GT_CODE) && (hdr[1] || hdr[2]);
            for(int i = CMD_NGROUPS * GD_SIZE; ok && i < CMD_HDR_SIZE; i++)
                if(hdr[i] != 0)
                    ok = 0;
            is_cmd = ok;
        }
    }
    rewind(f);

    // A ".cmd" extension is an explicit, trusted signal.
    if(name)
    {
        size_t l = strlen(name);
        if(l >= 4 && (name[l - 4] == '.') && (name[l - 3] | 0x20) == 'c' &&
           (name[l - 2] | 0x20) == 'm' && (name[l - 1] | 0x20) == 'd')
            return 1;
    }
    return is_cmd;
}

// ---------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------
int cpm86_load_cmd(FILE *f, const char *cmdline)
{
    uint8_t hdr[CMD_HDR_SIZE];
    struct cpm_group g[CMD_NGROUPS];
    int ng = 0;

    if(fread(hdr, 1, CMD_HDR_SIZE, f) != CMD_HDR_SIZE)
        return 0;
    if(!parse_header(hdr, g, &ng))
        return 0;

    // Locate code and (optional) data/extra/stack groups.
    struct cpm_group *code = 0, *data = 0, *extra = 0, *stack = 0;
    for(int i = 0; i < ng; i++)
    {
        if(g[i].type == GT_CODE && !code)
            code = &g[i];
        else if(g[i].type == GT_DATA && !data)
            data = &g[i];
        else if(g[i].type == GT_EXTRA && !extra)
            extra = &g[i];
        else if(g[i].type == GT_STACK && !stack)
            stack = &g[i];
    }
    if(!code)
        return 0;

    // --- Allocate memory for the groups (paragraphs) ------------------------
    int model_8080 = (data == 0);
    uint16_t code_par = code->max ? code->max : code->length;
    if(code_par < code->length)
        code_par = code->length;

    // CP/M-86 gives the program a full 64K data/stack group; the runtime sets
    // SP from base-page word 6 (= top of that group).  TODO: 8080 model packs
    // code+data into one group; handle that base-page layout separately.
    uint16_t data_par = 0x1000; // 64K
    if(data && data->max && data->max >= data->length && data->max < 0x1000)
        data_par = data->max;

    uint16_t gotmax = 0;
    if(model_8080)
    {
        if(code_par < 0x1000)
            code_par = 0x1000;
        cpm_code_seg = cpm_data_seg = cpm_base_seg = mem_alloc_segment(code_par, &gotmax);
        if(!cpm_code_seg)
            return 0;
    }
    else
    {
        cpm_code_seg = mem_alloc_segment(code_par, &gotmax);
        cpm_data_seg = cpm_base_seg = mem_alloc_segment(data_par, &gotmax);
        if(!cpm_code_seg || !cpm_data_seg)
            return 0;
    }

    // Extra (ES) and stack (SS) groups: allocate as much as the program asks
    // for (up to g_max), falling back to the largest available block, but at
    // least g_min.  Programs such as ARC86 keep large working buffers (its LZW
    // table) in the extra group and read its base/length from the base page.
    uint16_t extra_seg = 0, extra_par = 0, stack_seg = 0, stack_par = 0;
    if(extra && extra->min)
    {
        uint16_t want = extra->max > extra->min ? extra->max : extra->min;
        uint16_t avail = 0;
        extra_seg = mem_alloc_segment(want, &avail);
        if(!extra_seg && avail >= extra->min)
        {
            uint16_t a2;
            want = avail;
            extra_seg = mem_alloc_segment(want, &a2);
        }
        if(extra_seg)
            extra_par = want;
    }
    if(stack && stack->min)
    {
        uint16_t want = stack->max > stack->min ? stack->max : stack->min;
        uint16_t avail = 0;
        stack_seg = mem_alloc_segment(want, &avail);
        if(!stack_seg && avail >= stack->min)
        {
            uint16_t a2;
            want = avail;
            stack_seg = mem_alloc_segment(want, &a2);
        }
        if(stack_seg)
            stack_par = want;
    }

    // --- Load group images --------------------------------------------------
    fseek(f, code->file_off, SEEK_SET);
    if(fread(memory + (uint32_t)cpm_code_seg * 16, 1, (uint32_t)code->length * 16, f) == 0)
        return 0;
    if(data)
    {
        fseek(f, data->file_off, SEEK_SET);
        fread(memory + (uint32_t)cpm_data_seg * 16, 1, (uint32_t)data->length * 16, f);
    }
    if(extra_seg)
    {
        memset(memory + (uint32_t)extra_seg * 16, 0, (uint32_t)extra_par * 16);
        if(extra->length)
        {
            fseek(f, extra->file_off, SEEK_SET);
            fread(memory + (uint32_t)extra_seg * 16, 1, (uint32_t)extra->length * 16, f);
        }
    }
    if(stack_seg)
    {
        memset(memory + (uint32_t)stack_seg * 16, 0, (uint32_t)stack_par * 16);
        if(stack->length)
        {
            fseek(f, stack->file_off, SEEK_SET);
            fread(memory + (uint32_t)stack_seg * 16, 1, (uint32_t)stack->length * 16, f);
        }
    }

    // --- Base page (256 bytes at DS:0) --------------------------------------
    // CP/M-86 lays out the base page as 6-byte group descriptors, one per group
    // in the order code, data, extra, stack (then auxiliary groups):
    //   +0  length of the group in BYTES (24-bit, 3 bytes)
    //   +3  segment (paragraph) address of the group (word)
    //   +5  flag byte (set for the 8080 memory model)
    // Confirmed against the DRI System Guide layout and a base-page dump from
    // cpm86.exe.  Runtimes rely on this: Turbo Pascal, for instance, reads the
    // extra-group descriptor at 0x0C to compute SS (= extra_seg + extra_paras -
    // 64K) and to place its heap at the extra segment.  The data-group length
    // doubles as the "size of available memory" field.  (The previous
    // {segment,length}-word layout fed programs garbage.)
    uint16_t sp_top = (data_par >= 0x1000) ? 0xFFF0 : (uint16_t)(data_par * 16 - 16);
    uint32_t bp = (uint32_t)cpm_base_seg * 16;
    uint8_t mflag = model_8080 ? 0x01 : 0x00;
    memset(memory + bp, 0, 0x100);
#define CPM_GDESC(o, len_bytes, seg)                                               \
    do                                                                             \
    {                                                                              \
        uint32_t l_ = (len_bytes);                                                 \
        memory[bp + (o) + 0] = l_ & 0xFF;                                          \
        memory[bp + (o) + 1] = (l_ >> 8) & 0xFF;                                   \
        memory[bp + (o) + 2] = (l_ >> 16) & 0xFF;                                  \
        put16(bp + (o) + 3, (seg));                                                \
        memory[bp + (o) + 5] = mflag;                                              \
    } while(0)
    CPM_GDESC(0x00, (uint32_t)code_par << 4, cpm_code_seg); // code group
    CPM_GDESC(0x06, sp_top, cpm_data_seg);                  // data group (len = SP top)
    CPM_GDESC(0x0C, (uint32_t)extra_par << 4, extra_seg);   // extra group (0 if none)
    CPM_GDESC(0x12, (uint32_t)stack_par << 4, stack_seg);   // stack group (0 if none)
#undef CPM_GDESC

    // Command tail at base page 0x80: <len><chars><CR>.  Like the CP/M CCP,
    // upper-case it and prefix the leading space (the delimiter that follows
    // the command name) so parsers that expect that format work.
    char tail[130];
    unsigned tl = 0;
    tail[tl++] = ' ';
    for(const char *s = cmdline ? cmdline : ""; *s && tl < 0x7F; s++)
    {
        char c = *s;
        if(c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        tail[tl++] = c;
    }
    tail[tl] = 0;
    memory[bp + 0x80] = tl;
    memcpy(memory + bp + 0x81, tail, tl);
    memory[bp + 0x81 + tl] = 0x0d;
    cpm_dma_seg = cpm_base_seg; // default DMA = base page 0x80
    cpm_dma_off = 0x80;

    // Parse the command tail's first/second filename args into the default
    // FCBs at base page 0x5C/0x6C, as the CCP does (programs read them there).
    cmdline_to_fcb(tail, memory + bp + 0x5C, memory + bp + 0x6C);

    // --- Enter the program (matches the cpm86.exe hand-off) -----------------
    // The runtime's startup pops a far "exit" address off the entry stack and
    // jumps to it on termination; point it at the PSP, whose first bytes are an
    // INT 20h that terminates cleanly through emu2's existing handler.
    uint16_t psp = get_current_PSP();
    cpuSetCS(cpm_code_seg);
    // 8080 model is CP/M-80-style: base page at seg:0, code entry at seg:0x100.
    // Separate-group (small/compact) models enter at code:0.
    cpuSetIP(model_8080 ? 0x100 : 0);
    cpuSetDS(cpm_base_seg);
    cpuSetES(cpm_base_seg);
    cpuSetSS(cpm_base_seg);
    cpuSetSP(sp_top);
    cpuPushWord(psp); // exit address segment
    cpuPushWord(0);   // exit address offset -> PSP:0000 = INT 20h
    cpuSetAX(0);
    cpuSetBX(0);
    cpuSetCX(0);
    cpuSetDX(0x80); // base-page command tail / default DMA offset

    debug(debug_dos,
          "CP/M-86 load: %d groups code=%04x(+%u) data=%04x extra=%04x(+%u) sp=%04x "
          "model=%s\n",
          ng, cpm_code_seg, code->length, cpm_data_seg, extra_seg, extra_par, sp_top,
          model_8080 ? "8080" : "small");
    cpm86_active = 1;
    return 1;
}

// ===========================================================================
// BDOS dispatcher (INT 0E0h): function in CL, parameter in DX.
// Byte results are returned in AL (and copied to BL); word results in BX/AX.
// ===========================================================================

static void bdos_ret(uint16_t v)
{
    // CP/M-86 returns the result in AL/BL (byte) and BX/AX (word).
    cpuSetAX(v);
    cpuSetBX(v);
}

// Delegate a CP/M FCB function to emu2's matching DOS INT 21h handler.  The FCB
// is already at DS:DX (CP/M and DOS share that convention), so we just set AH
// and reuse the dos.c implementation, returning its AL result.
static unsigned bdos_via_dos(unsigned ah)
{
    cpuSetAX((cpuGetAX() & 0x00FF) | (ah << 8));
    intr21();
    return cpuGetAX() & 0xFF;
}

// CP/M read returns 0 even for a partial final record (the byte count lives in
// the FCB); DOS returns 3 ("read partial record, zero-padded").  Map 3 -> 0 so
// CP/M runtimes accept the last record instead of treating it as EOF/error.
static unsigned bdos_read(unsigned ah)
{
    unsigned r = bdos_via_dos(ah);
    return (r == 3) ? 0 : r;
}

// CP/M sequential read/write (BDOS 20/21).  Byte 0x0D of the FCB is S1, which
// CP/M reserves and the application must not rely on -- but emu2's DOS layer
// derives the sequential block number from the 16-bit field at 0x0C (EX | S1<<8).
// Some CP/M programs (e.g. VEDIT PLUS) leave a value in S1 -- the last-record
// byte count -- which would make the block number huge and seek past EOF, so the
// first read returns no data and the program treats the file as empty.  Clear S1
// before delegating so only EX (byte 0x0C) drives the file position.
static unsigned bdos_seq(unsigned ah)
{
    memory[cpuGetAddrDS(cpuGetDX()) + 0x0D] = 0;
    return (ah == 0x14) ? bdos_read(ah) : bdos_via_dos(ah);
}

// Apply the current CP/M DMA segment:offset as emu2's DTA (via DOS Set-DTA),
// preserving the guest's DS:DX.
static void cpm_set_dta(void)
{
    unsigned sds = cpuGetDS(), sdx = cpuGetDX();
    cpuSetDS(cpm_dma_seg);
    cpuSetDX(cpm_dma_off);
    bdos_via_dos(0x1A); // DOS Set DTA = DS:DX
    cpuSetDS(sds);
    cpuSetDX(sdx);
}

// CP/M Search First/Next (BDOS 17/18).  emu2's DOS find writes a DOS FCB to the
// DTA (drive at +0, name at +1..11, attr/time/size from +0x0C); CP/M programs
// expect a directory entry (user# at +0, name at +1..11, EX/S1/S2/RC at +12..15,
// allocation map at +16..31) at DTA + dir_code*32.  The name position already
// matches, so reformat byte 0 and 12..31 and return directory code 0 (the entry
// sits at DTA+0), or 0xFF at end of directory.
static unsigned cpm_search(unsigned ah)
{
    unsigned r = bdos_via_dos(ah);
    if(r == 0xFF)
        return 0xFF;
    uint32_t dta = (uint32_t)cpm_dma_seg * 16 + cpm_dma_off;
    debug(debug_dos, "CP/M search(%02x): '%.11s' -> dir code 0\n", ah,
          (char *)(memory + dta + 1));
    unsigned long sz = get32(dta + 0x1D); // file size DOS wrote (read before overwrite)
    unsigned long recs = (sz + 127) / 128;
    memory[dta + 0x00] = 0;                            // user number (was DOS drive)
    memory[dta + 0x0C] = 0;                            // EX (extent)
    memory[dta + 0x0D] = 0;                            // S1
    memory[dta + 0x0E] = 0;                            // S2
    memory[dta + 0x0F] = recs > 128 ? 128 : recs;      // RC (record count)
    for(int i = 0x10; i < 0x20; i++)
        memory[dta + i] = 0;
    memory[dta + 0x10] = 1;   // a non-zero allocation block, so the entry is "used"
    memory[dta + 0x20] = 0xE5; // slots 1-3 of the 128-byte directory record: empty
    memory[dta + 0x40] = 0xE5;
    memory[dta + 0x60] = 0xE5;
    return 0;               // directory code 0 -> matched entry is at DTA+0
}

extern int dos_serial_console;

// Console status (0xFF if a char is ready) and blocking console input.  In
// serial-console mode these read the terminal RAW (no PC-keyboard translation)
// so ANSI programs get bare ESC, key escape sequences and query replies intact;
// otherwise they use emu2's INT 21h keyboard handlers.
static unsigned cpm_con_status(void)
{
    return dos_serial_console ? (unsigned)serial_con_status() : bdos_via_dos(0x0B);
}
static unsigned cpm_con_getc(void)
{
    if(dos_serial_console)
    {
        int c = serial_con_getc();
        return c < 0 ? 0x1A : (unsigned)(c & 0xFF);
    }
    return bdos_via_dos(0x07); // direct console input, no echo
}

void intr_cpm_bdos(void)
{
    unsigned func = cpuGetCX() & 0xFF;
    unsigned dx = cpuGetDX();

    switch(func)
    {
    case 0: // System Reset / program termination
        debug(debug_dos, "CP/M BDOS 0: system reset\n");
        exit(0);

    // Output (2,9) and buffered input (10) delegate to emu2's INT 21h handlers
    // (output honors serial mode via dos_putchar).  Single-char input + status
    // (1,6,11) use cpm_con_* so they read the terminal raw in serial mode.
    case 1: // console input, echoed
    {
        unsigned c = cpm_con_getc();
        cpuSetDX(c); // echo it back (dos_putchar honors serial mode)
        bdos_via_dos(0x02);
        bdos_ret(c);
        break;
    }
    case 2: bdos_ret(bdos_via_dos(0x02)); break; // console output (DL)
    case 6: // Direct Console I/O -- CP/M-86 has more sub-functions than DOS:
        switch(dx & 0xFF)
        {
        case 0xFF: bdos_ret(cpm_con_status() ? cpm_con_getc() : 0); break; // input, no wait
        case 0xFE: bdos_ret(cpm_con_status()); break;                      // console status
        case 0xFD: bdos_ret(cpm_con_getc()); break;                        // input, wait
        default: bdos_ret(bdos_via_dos(0x06)); break;                      // output the char
        }
        break;
    case 9: bdos_ret(bdos_via_dos(0x09)); break; // print $-terminated string
    case 10: bdos_ret(bdos_via_dos(0x0A)); break; // read console buffer
    case 11: bdos_ret(cpm_con_status()); break;   // console status

    case 12: // Return Version Number (0x0022 = CP/M-86 2.2)
        bdos_ret(0x0022);
        break;

    case 26: // Set DMA Offset (DX); segment stays as last set by func 51
        cpm_dma_off = dx;
        cpm_set_dta();
        bdos_ret(0);
        break;

    case 51: // Set DMA Segment (DX) -- CP/M-86 DMA is segment:offset
        cpm_dma_seg = dx;
        cpm_set_dta();
        bdos_ret(0);
        break;

    // --- Disk / FCB functions ---------------------------------------------
    // CP/M-86 passes the FCB in DS:DX exactly like the DOS FCB calls (which
    // descend from CP/M), so each maps to the matching DOS INT 21h function.
    case 13: bdos_ret(bdos_via_dos(0x0D)); break; // reset disk system
    case 14: bdos_ret(bdos_via_dos(0x0E)); break; // select disk
    case 16: bdos_ret(bdos_via_dos(0x10)); break; // close file
    case 17: bdos_ret(cpm_search(0x11)); break; // search for first (CP/M dir entry)
    case 18: bdos_ret(cpm_search(0x12)); break; // search for next
    case 19: bdos_ret(bdos_via_dos(0x13)); break; // delete file
    case 20: bdos_ret(bdos_seq(0x14)); break; // read sequential
    case 21: bdos_ret(bdos_seq(0x15)); break; // write sequential
    case 22: bdos_ret(bdos_via_dos(0x16)); break; // make file
    case 23: bdos_ret(bdos_via_dos(0x17)); break; // rename file
    case 33: bdos_ret(bdos_read(0x21)); break; // read random
    case 34: bdos_ret(bdos_via_dos(0x22)); break; // write random
    case 35: bdos_ret(bdos_via_dos(0x23)); break; // compute file size
    case 36: bdos_ret(bdos_via_dos(0x24)); break; // set random record

    case 25: // Return current default drive (0 = A:)
        bdos_ret(bdos_via_dos(0x19));
        break;

    case 32: // Set/Get User Code: DL=0xFF gets it (return 0), else set (ignore)
        bdos_ret(0);
        break;

    case 50: // S_BIOS: direct BIOS call.  16-bit CP/M-86 allows only character
    {        // I/O.  Parameter block at DS:DX: +0 func, +1 CL, +2 CH, +3 DL, +4 DH.
        uint32_t pb = cpuGetAddrDS(dx);
        unsigned biosfn = memory[pb + 0];
        unsigned cl = memory[pb + 1];
        switch(biosfn)
        {
        case 2: bdos_ret(cpm_con_status()); break; // CONST: status (0xFF/0)
        case 3: bdos_ret(cpm_con_getc()); break;   // CONIN: input, no echo
        case 4: // CONOUT: write the character in CL
        {
            unsigned sdx = cpuGetDX();
            cpuSetDX(cl);
            bdos_via_dos(0x02);
            cpuSetDX(sdx);
            bdos_ret(0);
            break;
        }
        default: bdos_ret(0); break; // other BIOS calls: ignore
        }
        break;
    }

    case 15: // open file (+ CP/M-86 LRBC byte count in the S1 field)
    {
        uint32_t fcb = cpuGetAddrDS(dx);
        unsigned r = bdos_via_dos(0x0F);
        if(r != 0xFF)
        {
            // S1 (fcb[13]) = bytes in the last record = file size mod 128.
            unsigned long sz = get32(fcb + 0x10);
            memory[fcb + 0x0D] = (uint8_t)(sz & 0x7F);
        }
        bdos_ret(r);
        break;
    }

    // --- CP/M-86 memory management ----------------------------------------
    // MCB at DS:DX: +0 segment, +2 length (paragraphs), +4 ext.  Return
    // AX=0/0xFFFF and CX=error code (0 ok, 3 out of memory).
    case 53: // MC_MAX:   allocate up to MCB length (largest available <= want)
    case 55: // MC_ALLOC: allocate exactly MCB length
    {
        uint32_t mcb = cpuGetAddrDS(dx);
        uint16_t want = get16(mcb + 2);
        uint16_t avail = 0;
        uint16_t seg = mem_alloc_segment(want, &avail);
        uint16_t got = want;
        if(!seg && func == 53 && avail) // MC_MAX: take the largest block we can
        {
            uint16_t a2 = 0;
            seg = mem_alloc_segment(avail, &a2);
            got = avail;
        }
        // MC_MAX only *sizes* available memory; it must not keep it, or the
        // program's follow-up MC_ALLOC (a common idiom, e.g. ZORK) finds nothing
        // left.  Report the segment/length but free the block again.
        if(seg && func == 53)
            mem_free_segment(seg);
        if(seg)
        {
            put16(mcb + 0, seg);
            put16(mcb + 2, got);
            if(func == 53)
                memory[mcb + 4] = 1; // MC_MAX: additional memory is available
            cpuSetAX(0);
            cpuSetCX(0);
        }
        else
        {
            if(func == 53)
                memory[mcb + 4] = 0; // MC_MAX: no additional memory
            cpuSetAX(0xFFFF);
            cpuSetCX(3); // out of memory
        }
        break;
    }

    case 57: // MC_FREE: free the region named by the MCB (ext 0xFF = free all)
    {
        uint32_t mcb = cpuGetAddrDS(dx);
        if(memory[mcb + 4] != 0xFF)
            mem_free_segment(get16(mcb + 0));
        cpuSetAX(0);
        cpuSetCX(0);
        break;
    }

    case 58: // MC_ALLFREE: obsolete; nothing to do
        cpuSetAX(0);
        cpuSetCX(0);
        break;

    case 54: // MC_ABSMAX / MC_ABSALLOC: absolute allocation not supported yet
    case 56:
        cpuSetAX(0xFFFF);
        cpuSetCX(3);
        break;

    case 105: // T_GET: Get Date and Time -> fills DAT at DS:DX, AL = seconds (BCD)
    {
        // DAT structure: word = days since 1978-01-01 (=day 1), byte hour (BCD),
        // byte minute (BCD); AL returns seconds (BCD).
        time_t now = time(0);
        struct tm *lt = localtime(&now);
        struct tm epoch = {.tm_year = 78, .tm_mon = 0, .tm_mday = 1, .tm_hour = 12};
        long days = (long)(difftime(now, mktime(&epoch)) / 86400) + 1;
        uint32_t dat = cpuGetAddrDS(dx);
        put16(dat, (uint16_t)days);
        memory[dat + 2] = ((lt->tm_hour / 10) << 4) | (lt->tm_hour % 10);
        memory[dat + 3] = ((lt->tm_min / 10) << 4) | (lt->tm_min % 10);
        debug(debug_dos, "CP/M get date/time: day %ld %02d:%02d:%02d\n", days,
              lt->tm_hour, lt->tm_min, lt->tm_sec);
        bdos_ret(((lt->tm_sec / 10) << 4) | (lt->tm_sec % 10));
        break;
    }

    default:
        debug(debug_dos, "CP/M BDOS %u: UNIMPLEMENTED (DX=%04x)\n", func, dx);
        bdos_ret(0xFF); // 0xFF = error / not found for most file funcs
        break;
    }
}

// INT 28h keyboard interface for CP/M-86 programs.  ZORK (and other Infocom
// interpreters) busy-poll INT 28h with DI=4 and read CL: CL!=0 means a key is
// available, with the character deposited in the caller's DS:0x200 (the poll
// loop then reads it from there).  Other DI values behave as a no-op idle.
void intr_cpm_int28(void)
{
    if(cpuGetDI() != 4)
        return;
    // Non-consuming keyboard status: CL != 0 when input is waiting.  The program
    // reads the actual line afterwards with BDOS 10 (DOS buffered input, which
    // reads stdin), so poll stdin here -- using the keyboard layer instead would
    // consume from a different stream and desync the line read.
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    int ready = poll(&pfd, 1, 0) > 0;
    cpuSetCX((cpuGetCX() & 0xFF00) | (ready ? 0xFF : 0x00));
}
