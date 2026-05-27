#define _GNU_SOURCE

#include "cpm86.h"
#include "dbg.h"
#include "dos.h"
#include "env.h"
#include "dosnames.h"
#include "emu.h"
#include "keyb.h"
#include "loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

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
static void cpm_set_dta(void); // defined below; used by the loader

// Reported CP/M version (BDOS function 12).  The low byte holds the version as
// packed nibbles (0x22 = 2.2, 0x31 = 3.1); the high byte is the system type
// (0 = plain CP/M).  CP/M 3.0 and later maintain the last-record byte count
// (LRBC) in the directory/FCB S1 field, letting programs recover exact file
// lengths instead of rounding up to the 128-byte record; older programs that
// check for 2.2 ignore S1, so the metadata is only exposed when we report 3.0+.
// Default is CP/M 3.1; override with EMU2_CPMVER (e.g. "2.2", "3.1").
static uint16_t cpm_version = 0x0031; // packed BDOS version, 0x31 = CP/M 3.1
static int cpm_lrbc = 1;              // expose LRBC byte-count metadata (CP/M 3+)

// Parse EMU2_CPMVER ("3.1", "2.2", "3", ...) into the reported BDOS version and
// decide whether to expose the LRBC byte-count metadata (CP/M 3.0 and later).
static void cpm_init_version(void)
{
    const char *ver = getenv(ENV_CPMVER);
    if(ver)
    {
        char *end = 0;
        long major = strtol(ver, &end, 10);
        long minor = 0;
        if(*end == '.' && end[1])
            minor = strtol(end + 1, &end, 10);
        if(*end || major < 1 || major > 9 || minor < 0 || minor > 9)
            print_error("invalid CP/M version '%s'\n", ver);
        cpm_version = (uint16_t)(((major & 0x0F) << 4) | (minor & 0x0F));
    }
    cpm_lrbc = (cpm_version & 0xFF) >= 0x30;
    debug(debug_dos, "CP/M version reported as %x.%x, LRBC %s\n",
          (cpm_version >> 4) & 0x0F, cpm_version & 0x0F, cpm_lrbc ? "on" : "off");
}

// ---------------------------------------------------------------------------
// Fabricated CP/M "disk" geometry.  Each drive (A..) maps to a host directory
// (EMU2_DRIVE_<letter>, default "."); we present its files as a CP/M disk whose
// geometry is computed PER DRIVE from that directory's contents.  The block size
// sets the allocation granularity (smallest reportable file size) and, with the
// 8-/16-bit block-number rule, the disk size; files bigger than one directory
// entry are reported as multiple extents, up to the per-file extent limit.
//   EMU2_CPM_DISK[_<letter>] = auto (default) | 1k | 2k | 4k | 8k | 16k
//   EMU2_CPM_FREE            = target free-space percent for auto sizing (def 25)
//   EMU2_CPM_PLUS            = CP/M 3 limits: 2048 extents/32MB files, 512MB disks
// The disk is also capped at the guest-tool ceiling: a standard CP/M 2.2 program
// (16-bit record counts) tops out at ~8MB; CP/M-3 (PLUS) reaches 512MB.
// ---------------------------------------------------------------------------
static unsigned cpm_blk_size = 2048; // CURRENT drive's geometry (set by cpm_select_disk)
static unsigned cpm_bsh = 4, cpm_blm = 15, cpm_exm = 0;
static unsigned cpm_dsm = 4095, cpm_dir_blocks = 0, cpm_drm = 1023;
static unsigned cpm_max_ext = 512;   // max extents per file (512 std, 2048 CP/M3)
static uint32_t cpm_av_addr = 0;     // scratch for the BDOS 27 allocation vector
static int cpm_cur_drive = -1;       // currently selected drive (0=A)
// Cached per-drive geometry (computed once per drive from its directory):
static struct cpm_geo
{
    unsigned blk_size, bsh, blm, exm, dsm, dir_blocks, drm;
    char ready;
} cpm_geo[26];
// Multi-extent emission state (a large file spans several directory entries,
// returned one per Search-Next call without advancing the host scan):
static unsigned cpm_blk_next;         // next free block to hand out (this scan)
static unsigned long cpm_ext_left;    // blocks of the current file not yet emitted
static unsigned cpm_ext_entry;        // physical directory-entry index for that file
static unsigned long cpm_ext_records; // total 128-byte records of the current file
static unsigned long cpm_ext_bytes;   // exact byte size of the current file (LRBC)
static uint8_t cpm_ext_name[12];      // user# + 8.3 name (+ attr bits) of that file

// A file is representable (shown, sized for, counted) only if it fits the per-file
// extent limit -- 512 extents (8MB) normally, 2048 (32MB) with EMU2_CPM_PLUS.
// cpm_search skips bigger files, so the sizing/free-space scans skip them too.
static int cpm_representable(unsigned long size)
{
    return (size + 16383) / 16384 <= cpm_max_ext;
}

// Sum either the bytes (blk==0) or the rounded block count (blk>0) of the
// representable regular files in host directory `path`.
static unsigned long cpm_scan_dir(const char *path, unsigned blk)
{
    unsigned long total = 0;
    DIR *d = opendir(path);
    if(d)
    {
        struct dirent *e;
        char full[4096];
        while((e = readdir(d)))
        {
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", path, e->d_name);
            if(0 == stat(full, &st) && S_ISREG(st.st_mode) &&
               cpm_representable((unsigned long)st.st_size))
                total += blk ? ((unsigned long)st.st_size + blk - 1) / blk
                             : (unsigned long)st.st_size;
        }
        closedir(d);
    }
    return total;
}

// Compute and cache the geometry for `drive` from its host directory + env vars.
static void cpm_compute_geo(int drive)
{
    int plus = getenv("EMU2_CPM_PLUS") != 0;
    cpm_max_ext = plus ? 2048 : 512;
    const char *path = get_base_path(drive);
    // EMU2_CPM_DISK_<letter> overrides the global EMU2_CPM_DISK.
    char key[20];
    snprintf(key, sizeof key, "EMU2_CPM_DISK_%c", 'A' + drive);
    const char *mode = getenv(key);
    if(!mode)
        mode = getenv("EMU2_CPM_DISK");
    const char *fenv = getenv("EMU2_CPM_FREE");
    unsigned free_pct = fenv ? (unsigned)strtoul(fenv, 0, 0) : 25;
    if(free_pct > 90)
        free_pct = 90;

    // Guest-tool disk ceiling: ~8MB for standard CP/M 2.2, 512MB for CP/M-3.
    unsigned long disk_max = plus ? 512UL * 1024 * 1024 : 65535UL * 128;
    unsigned long total = cpm_scan_dir(path, 0);
    unsigned long need = total * 100 / (100 - free_pct); // leave free_pct% free
    if(need < 64UL * 1024)
        need = 64UL * 1024;
    if(need > disk_max)
        need = disk_max;

    unsigned blk;
    if(mode && strcasecmp(mode, "auto"))
    {
        blk = (unsigned)strtoul(mode, 0, 0) * 1024; // "1k".."16k" -> bytes
        if(blk != 1024 && blk != 2048 && blk != 4096 && blk != 8192 && blk != 16384)
            blk = 2048;
    }
    else if(need <= 256UL * 1024)
        blk = 1024; // 1K is 8-bit -> 256K max
    else if(!plus)
        // Standard: keep the disk within 2048 blocks (the CP/M 2.2 tool limit), so
        // 2K up to 4MB, 4K up to 8MB (the standard 8MB ceiling).
        blk = (need <= 2048UL * 2048) ? 2048 : 4096;
    else if(need <= 65535UL * 2048)
        blk = 2048; // CP/M-3: 16-bit block numbers scale to 65535 blocks
    else if(need <= 65535UL * 4096)
        blk = 4096;
    else if(need <= 65535UL * 8192)
        blk = 8192;
    else
        blk = 16384;

    // Size in *blocks*: files round up to whole blocks and the directory takes
    // space, so size from rounded block counts (+16-block dir reserve) for free%.
    unsigned long fblocks = cpm_scan_dir(path, blk);
    unsigned long dblocks = (fblocks + 16) * 100 / (100 - free_pct);
    unsigned long blocks = (need + blk - 1) / blk;
    if(dblocks > blocks)
        blocks = dblocks;
    // Caps.  1K is 8-bit -> 256 blocks.  A standard CP/M 2.2 tool (e.g. STAT) has
    // a fixed ~2048-block allocation bitmap and a 16-bit record count, so cap the
    // disk at 2048 blocks AND 65535 records; CP/M-3 (PLUS) only needs DSM<=65534.
    unsigned long max_blocks = (blk == 1024) ? 256 : (plus ? 65535 : 2048);
    if(!plus && max_blocks > 65535UL * 128 / blk)
        max_blocks = 65535UL * 128 / blk;
    if(blocks > max_blocks)
        blocks = max_blocks;
    if(blocks < 8)
        blocks = 8;

    struct cpm_geo *g = &cpm_geo[drive];
    g->blk_size = blk;
    g->bsh = 0;
    for(unsigned t = blk / 128; t > 1; t >>= 1)
        g->bsh++;
    g->blm = blk / 128 - 1;
    g->dsm = (unsigned)(blocks - 1);
    unsigned bpe = (g->dsm >= 256) ? 8 : 16;
    unsigned lpe = (bpe * blk) / 16384;
    g->exm = (lpe < 1 ? 1 : lpe) - 1;
    // Directory entries scale with the disk, but AL0/AL1 marks at most 16 blocks.
    unsigned entries = (unsigned)(blocks / 2);
    unsigned max_entries = blk / 2;
    if(entries > max_entries)
        entries = max_entries;
    if(entries > 2048)
        entries = 2048;
    if(entries < 64)
        entries = 64;
    g->drm = entries - 1;
    g->dir_blocks = (unsigned)(((unsigned long)entries * 32 + blk - 1) / blk);
    if(g->dir_blocks < 1)
        g->dir_blocks = 1;
    g->ready = 1;
    debug(debug_dos,
          "CP/M disk %c: '%s' blk=%u DSM=%u DRM=%u dir_blks=%u max_ext=%u\n",
          'A' + drive, path, g->blk_size, g->dsm, g->drm, g->dir_blocks, cpm_max_ext);
}

// Make `drive` the current disk, computing its geometry on first use.
static void cpm_select_disk(int drive)
{
    if(drive < 0 || drive >= 26)
        drive = 0;
    if(!cpm_av_addr)
        cpm_av_addr = get_static_memory(8192 + 16, 16);
    if(!cpm_geo[drive].ready)
        cpm_compute_geo(drive);
    struct cpm_geo *g = &cpm_geo[drive];
    cpm_blk_size = g->blk_size;
    cpm_bsh = g->bsh;
    cpm_blm = g->blm;
    cpm_exm = g->exm;
    cpm_dsm = g->dsm;
    cpm_dir_blocks = g->dir_blocks;
    cpm_drm = g->drm;
    cpm_cur_drive = drive;
}

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
    // CP/M terminates the command tail with a NUL.  (A CR here is read as a stray
    // operand byte by command parsers that scan for a <=1 terminator, e.g. STAT.)
    memory[bp + 0x81 + tl] = 0x00;
    cpm_dma_seg = cpm_base_seg; // default DMA = base page 0x80
    cpm_dma_off = 0x80;
    cpm_set_dta(); // sync emu2's DTA to the default, so programs that read the
                   // directory without setting the DMA (e.g. STAT) work too

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
    cpm_init_version();
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

// Write one CP/M directory entry (one extent of the current file) to the DTA:
// user#+name+attrs, the EX/S2/RC for this extent, and its slice of the block map
// (8-bit numbers if the disk has <=256 blocks, else 16-bit).  Advances the block
// allocator and the extent counter.
static void cpm_emit_extent(uint32_t dta)
{
    unsigned bpe = (cpm_dsm >= 256) ? 8 : 16; // block pointers per directory entry
    unsigned lext_per_entry = cpm_exm + 1;    // 16K logical extents per entry
    unsigned long total_lext = (cpm_ext_records + 127) / 128;

    memcpy(memory + dta, cpm_ext_name, 12); // user# + name (+ attribute high-bits)
    unsigned ex = 0, s2 = 0, rc = 0, s1 = 0;
    if(total_lext)
    {
        unsigned long last = (unsigned long)(cpm_ext_entry + 1) * lext_per_entry;
        if(last > total_lext)
            last = total_lext;
        unsigned long extnum = last - 1; // highest 16K extent in this entry
        int is_last = (extnum == total_lext - 1); // file's final extent?
        rc = is_last ? (unsigned)(cpm_ext_records - extnum * 128) : 128;
        ex = extnum & 0x1F;
        s2 = (extnum >> 5) & 0x3F;
        // CP/M 3 records the byte count of the file's last record in S1 (0 = a
        // full 128-byte record), so programs see the exact length, not a record-
        // rounded one.  Only the final extent carries it, and only when LRBC is on.
        if(is_last && cpm_lrbc)
            s1 = (unsigned)(cpm_ext_bytes & 0x7F);
    }
    memory[dta + 0x0C] = ex; // EX
    memory[dta + 0x0D] = s1; // S1 (CP/M 3 last-record byte count; 0 = full)
    memory[dta + 0x0E] = s2; // S2
    memory[dta + 0x0F] = rc; // RC (records in the last 16K extent; 128 = full)

    for(int i = 0x10; i < 0x20; i++)
        memory[dta + i] = 0;
    unsigned nb = (cpm_ext_left < bpe) ? (unsigned)cpm_ext_left : bpe;
    for(unsigned i = 0; i < nb; i++)
    {
        if(cpm_dsm >= 256)
            put16(dta + 0x10 + i * 2, cpm_blk_next);
        else
            memory[dta + 0x10 + i] = (uint8_t)cpm_blk_next;
        cpm_blk_next++;
    }
    cpm_ext_left -= nb;
    cpm_ext_entry++;

    memory[dta + 0x20] = 0xE5; // slots 1-3 of the 128-byte record are empty
    memory[dta + 0x40] = 0xE5;
    memory[dta + 0x60] = 0xE5;
}

// CP/M Search First/Next (BDOS 17/18).  emu2's DOS find writes a DOS FCB to the
// DTA (drive at +0, name at +1..11, attr/size from +0x0C/+0x1D); CP/M programs
// expect a directory entry (user# at +0, name at +1..11, EX/S1/S2/RC at +12..15,
// allocation map at +16..31).  We reformat that into one or more extents and
// return directory code 0 (entry at DTA+0), or 0xFF at end of directory.
static unsigned cpm_search(unsigned ah)
{
    uint8_t fcb_name_save[11], fcb_drive_save = 0;
    uint32_t fcb_addr = 0;
    int restore_name = 0;
    uint32_t dta = (uint32_t)cpm_dma_seg * 16 + cpm_dma_off;

    if(ah == 0x11)
    {
        fcb_addr = cpuGetAddrDS(cpuGetDX());
        // Select the FCB's drive (byte 0: 0/wildcard = current, 1..16 = A..P) so
        // the block geometry matches the directory the DOS find will read.
        unsigned fdrv = memory[fcb_addr] & 0x1F;
        cpm_select_disk((fdrv >= 1 && fdrv <= 16) ? (int)fdrv - 1
                                                  : dos_get_default_drive());
        cpm_blk_next = cpm_dir_blocks; // file data starts after the directory blocks
        cpm_ext_left = 0;              // no file in progress
        // A '?' (0x3F) drive byte is CP/M's "match every entry" wildcard: the BDOS
        // ignores the FCB name, so programs (STAT) stash data there (the user# at
        // FCB+4).  emu2's DOS find matches by name and treats 0x3F as an invalid
        // drive (-> cwd), so force an all-'?' name AND a 0 (default) drive byte so
        // the find reads the currently-selected drive; RESTORE both afterward.
        if(memory[fcb_addr] == 0x3F)
        {
            memcpy(fcb_name_save, memory + fcb_addr + 1, 11);
            memset(memory + fcb_addr + 1, '?', 11);
            fcb_drive_save = memory[fcb_addr];
            memory[fcb_addr] = 0; // 0 = default drive (the one selected via BDOS 14)
            restore_name = 1;
        }
    }

    // Mid-file: hand back the next extent without advancing the host-dir scan.
    if(ah == 0x12 && cpm_ext_left > 0)
    {
        cpm_emit_extent(dta);
        return 0;
    }

    // Fetch the next host file that fits the disk; skip ones too big to represent.
    unsigned find = ah;
    for(;;)
    {
        unsigned r = bdos_via_dos(find);
        if(restore_name)
        {
            memcpy(memory + fcb_addr + 1, fcb_name_save, 11);
            memory[fcb_addr] = fcb_drive_save;
            restore_name = 0;
        }
        if(r == 0xFF)
            return 0xFF;
        unsigned long sz = get32(dta + 0x1D);   // size DOS wrote (read before reuse)
        unsigned dos_attr = memory[dta + 0x0C]; // DOS attr (bit 0 = read-only)
        unsigned long records = (sz + 127) / 128;
        unsigned long lext = (sz + 16383) / 16384;             // 16K logical extents
        unsigned long blocks = (sz + cpm_blk_size - 1) / cpm_blk_size;
        if(lext > cpm_max_ext ||
           cpm_blk_next + blocks > (unsigned long)cpm_dsm + 1) // too big / disk full
        {
            debug(debug_dos, "CP/M search: skip '%.11s' (%lu blocks won't fit)\n",
                  (char *)(memory + dta + 1), blocks);
            find = 0x12;
            continue;
        }
        cpm_ext_name[0] = 0; // user 0 (was the DOS drive byte)
        memcpy(cpm_ext_name + 1, memory + dta + 1, 11);
        if(dos_attr & 0x01)
            cpm_ext_name[9] |= 0x80; // host not writable -> CP/M read-only (t1')
        cpm_ext_left = blocks;
        cpm_ext_entry = 0;
        cpm_ext_records = records;
        cpm_ext_bytes = sz;
        debug(debug_dos, "CP/M search(%02x): '%.11s' sz=%lu recs=%lu blocks=%lu\n", ah,
              (char *)(memory + dta + 1), sz, records, blocks);
        break;
    }
    cpm_emit_extent(dta);
    return 0;
}

// Console status (0xFF if a char is ready) and blocking console input, using
// emu2's INT 21h keyboard handlers (the BIOS keyboard, with PC-key translation).
static unsigned cpm_con_status(void)
{
    return bdos_via_dos(0x0B);
}
static unsigned cpm_con_getc(void)
{
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

    case 12: // Return Version Number (0x0022 = 2.2, 0x0031 = 3.1); EMU2_CPMVER
        bdos_ret(cpm_version);
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

    case 24: // Get login vector (bit d set = drive d on-line): report the current
    {        // drive plus every drive explicitly mapped with EMU2_DRIVE_<letter>.
        unsigned vec = 1u << (dos_get_default_drive() & 0x0F);
        for(int d = 0; d < 16; d++)
        {
            char key[16];
            snprintf(key, sizeof key, "EMU2_DRIVE_%c", 'A' + d);
            if(getenv(key))
                vec |= 1u << d;
        }
        bdos_ret(vec);
        break;
    }

    case 29: // Get R/O vector: no drives are read-only (the host disk is writable)
        bdos_ret(0);
        break;

    case 30: // Set File Attributes: map the CP/M read-only bit (t1' = high bit of
    {        // FCB byte 9) to a host chmod.  sys/archive bits have no host analog.
        uint32_t fcb = cpuGetAddrDS(dx);
        int ro = memory[fcb + 9] & 0x80;
        bdos_ret(dos_chmod_fcb(fcb, ro) ? 0 : 0xFF);
        break;
    }

    case 31: // Get DPB (disk parameter block) address -> returned in ES:BX.
    {        // Fabricate a DPB for the current drive's geometry.
        cpm_select_disk(dos_get_default_drive());
        uint32_t d = (uint32_t)cpm_base_seg * 16 + 0x40; // scratch in the base page
        put16(d + 0, 8u << cpm_bsh); // SPT  128-byte sectors per track (cosmetic)
        memory[d + 2] = cpm_bsh;     // BSH  block shift
        memory[d + 3] = cpm_blm;     // BLM  block mask
        memory[d + 4] = cpm_exm;     // EXM  extent mask
        put16(d + 5, cpm_dsm);       // DSM  max block number
        put16(d + 7, cpm_drm);       // DRM  directory entries - 1
        // AL0/AL1: the high-order bits mark the blocks reserved for the directory.
        uint16_t almask = 0;
        for(unsigned i = 0; i < cpm_dir_blocks && i < 16; i++)
            almask |= 0x8000u >> i;
        memory[d + 9] = almask >> 8;   // AL0
        memory[d + 10] = almask & 0xFF; // AL1
        put16(d + 11, 0);            // CKS  checksum vector size (fixed disk: 0)
        put16(d + 13, 2);            // OFF  reserved tracks
        cpuSetAX(0);
        cpuSetES(cpm_base_seg);
        cpuSetBX(0x40); // ES:BX -> DPB  (do NOT use bdos_ret: it clobbers BX)
        break;
    }

    case 27: // Get Allocation Vector address -> ES:BX.  Build a bitmap (1 bit per
    {        // block, MSB first) marking the directory blocks plus the blocks used
             // by the files now in the directory, so free-space matches the listing.
        cpm_select_disk(dos_get_default_drive());
        unsigned nblocks = cpm_dsm + 1;
        // directory blocks + the blocks used by the current drive's files
        unsigned long used_blocks =
            cpm_dir_blocks + cpm_scan_dir(get_base_path(cpm_cur_drive), cpm_blk_size);
        if(used_blocks > nblocks)
            used_blocks = nblocks;
        unsigned nbytes = (nblocks + 7) / 8;
        for(unsigned i = 0; i < nbytes; i++)
        {
            uint8_t bits = 0;
            for(unsigned b = 0; b < 8; b++)
                if((unsigned long)(i * 8 + b) < used_blocks)
                    bits |= 0x80u >> b; // mark used (MSB = lowest block number)
            memory[cpm_av_addr + i] = bits;
        }
        cpuSetAX(0);
        cpuSetES((uint16_t)(cpm_av_addr >> 4));
        cpuSetBX((uint16_t)(cpm_av_addr & 0x0F)); // ES:BX -> allocation vector
        break;
    }

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

    case 15: // open file (+ CP/M-3 LRBC byte count in the S1 field)
    {
        uint32_t fcb = cpuGetAddrDS(dx);
        unsigned r = bdos_via_dos(0x0F);
        // S1 (fcb[13]) = bytes in the last record = file size mod 128 (0 = full
        // 128-byte record).  Only CP/M 3.0+ programs read this field; under 2.2
        // S1 is reserved and must stay 0, so only fill it when LRBC is enabled.
        if(r != 0xFF && cpm_lrbc)
        {
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
