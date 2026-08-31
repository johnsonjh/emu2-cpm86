#define _GNU_SOURCE

#include "cpm86.h"
#include "dbg.h"
#include "dos.h"
#include "env.h"
#include "dosnames.h"
#include "emu.h"
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

// TPA size override from "-m <kb>"; 0 = not set (see cpm86_get_tpa_kb).
unsigned cpm86_tpa_kb_cli = 0;

// TPA size in KB: "-m" > CPM86_TPA_KB env var > ~640K default.
// Used by both the CMD loader and dos.c MCB init so they agree on one ceiling.
unsigned cpm86_get_tpa_kb(void)
{
    static const unsigned default_tpa_kb = 640;
    if(cpm86_tpa_kb_cli)
        return cpm86_tpa_kb_cli;
    const char *e = getenv("CPM86_TPA_KB");
    if(e && *e)
    {
        unsigned v = (unsigned)strtoul(e, 0, 0);
        if(v)
            return v;
    }
    return default_tpa_kb;
}

// Parsed program image (segments are emu2 paragraph addresses).
static uint16_t cpm_code_seg, cpm_data_seg, cpm_base_seg;
// Extra/stack segments of the currently-loaded program (0 = none), so BDOS 47
// (Chain To Program) can free them before loading the next program.
static uint16_t cpm_extra_seg, cpm_stack_seg;
// CP/M-86 DMA (disk transfer) address is segment:offset: BDOS 26 sets the
// offset, BDOS 51 the segment.  Defaults to base page 0x80.
static uint16_t cpm_dma_seg, cpm_dma_off;
static void cpm_set_dta(void); // defined below; used by the loader

// Reported CP/M version (BDOS function 12).  The low byte holds the version as
// packed nibbles (0x22 = 2.2, 0x31 = 3.1); the high byte is the system type
// (0 = plain CP/M).  CP/M 3.0 and later maintain the last-record byte count
// (LRBC) at FCB+32 (FCB+0x20), letting programs recover exact file lengths
// instead of rounding up to the 128-byte record.  On open (BDOS 15) the caller
// signals interest by pre-setting FCB+32 to 0xFF; the BDOS replaces it with the
// actual count.  Search results (BDOS 17/18) also carry the LRBC at ENTRY+13
// (S1) of each fabricated directory entry.  Older programs that check for 2.2
// leave FCB+32 alone, so the metadata is only exposed when we report 3.0+.
// Default is CP/M 3.1; override with EMU2_CPMVER (e.g. "2.2", "3.1").
static uint16_t cpm_version = 0x0031; // packed BDOS version, 0x31 = CP/M 3.1
static int cpm_lrbc = 1;              // expose LRBC byte-count metadata (CP/M 3+)
static int cpm_lrbc_trunc = 1;        // also trim host files to the LRBC length
static int cpm_lrbc_isx = 0;          // LRBC: UNUSED (ISX) vs USED (DOS Plus) bytes

// The LRBC byte that carries an exact file's last-record byte count has no
// universally agreed meaning; two conventions exist (see EMU2_CPM_ISXLRBC):
//   DOS Plus (default): LRBC = bytes USED in the last record (size mod 128).
//   ISX:                LRBC = bytes UNUSED in the last record (128 - that).
// In both, LRBC == 0 means the file fills its last record exactly (no partial
// tail), so a multiple-of-128 size always encodes as 0 either way.

// Encode an exact file size's last-record byte count (for FCB+32 or ENTRY+13).
static uint8_t cpm_lrbc_encode(unsigned long size)
{
    unsigned used = (unsigned)(size & 0x7F); // 0 = the file fills its last record
    if(cpm_lrbc_isx)
        return (uint8_t)((128 - used) & 0x7F); // unused bytes (0 stays 0)
    return (uint8_t)used;                      // used bytes
}

// Recover the exact file length from a record-rounded size `sz` and an LRBC byte
// `lrbc` (caller guarantees lrbc != 0, i.e. a genuine partial last record).
static unsigned long cpm_lrbc_exact(unsigned long sz, unsigned lrbc)
{
    unsigned long recs = (sz + 127) / 128;
    if(cpm_lrbc_isx)
        return recs * 128 - lrbc;       // lrbc = unused bytes in the last record
    return (recs - 1) * 128 + lrbc;     // lrbc = used bytes in the last record
}

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
    // EMU2_LRBC_NOTRUNC (any value but 0/off/no/false) keeps the LRBC byte count
    // visible but stops emu2 from trimming host files to that exact length, in
    // case a tool relies on output staying padded to the 128-byte record.  Host
    // truncation also implies LRBC, so a pre-3.0 version (which has no LRBC)
    // never trims, matching a real CP/M 2.2 that ignores the S1 byte count.
    {
        const char *nt = getenv(ENV_LRBC_NOTRUNC);
        int notrunc = nt && strcasecmp(nt, "0") && strcasecmp(nt, "off")
                         && strcasecmp(nt, "no") && strcasecmp(nt, "false");
        cpm_lrbc_trunc = cpm_lrbc && !notrunc;
    }
    // EMU2_CPM_ISXLRBC (any value but 0/off/no/false) interprets the S1 byte
    // count as the ISX "unused bytes" convention instead of DOS Plus "used
    // bytes".  Default off (DOS Plus), matching official DRI behaviour.
    {
        const char *isx = getenv(ENV_CPM_ISXLRBC);
        cpm_lrbc_isx = isx && strcasecmp(isx, "0") && strcasecmp(isx, "off")
                           && strcasecmp(isx, "no") && strcasecmp(isx, "false");
    }
    debug(debug_dos, "CP/M version reported as %x.%x, LRBC %s%s%s\n",
          (cpm_version >> 4) & 0x0F, cpm_version & 0x0F, cpm_lrbc ? "on" : "off",
          (cpm_lrbc && !cpm_lrbc_trunc) ? " (no host truncate)" : "",
          (cpm_lrbc && cpm_lrbc_isx) ? " (ISX convention)" : "");
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
static int cpm_all_ext;               // emit every extent vs only the matching first

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

    // CPM86_POISON=<byte>: fill free memory before loading (debug aid).
    int dirty_groups = 0;
    {
        const char *pe = getenv("CPM86_POISON");
        if(pe && *pe)
        {
            dirty_groups = 1;
            mem_poison_free((uint8_t)strtoul(pe, 0, 0));
        }
    }

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

    // Allocate all groups in one combined grant (load.sup style): sum min/max
    // across all groups, allocate once, then spread the grant proportionally
    // (min to each group first; surplus distributed by max-min, extra before stack).
    uint16_t extra_seg = 0, extra_par = 0, stack_seg = 0, stack_par = 0;
    uint16_t grant_seg = 0;
    {
        if(model_8080 && code_par < 0x1000)
            code_par = 0x1000;
        unsigned tpa_kb = cpm86_get_tpa_kb();
        uint32_t tpa_paras = (uint32_t)tpa_kb * 64;   // 1 KB = 64 paragraphs
        uint32_t fixed = (uint32_t)code_par + (model_8080 ? 0 : data_par);
        uint16_t ex_min = extra ? extra->min : 0;
        uint16_t ex_max = (extra && extra->max > extra->min) ? extra->max : ex_min;
        uint16_t st_min = stack ? stack->min : 0;
        uint16_t st_max = (stack && stack->max > stack->min) ? stack->max : st_min;
        uint32_t total_min = fixed + ex_min + st_min;
        uint32_t total_max = fixed + ex_max + st_max;
        if(total_min > tpa_paras || total_min > 0xFFFF)
        {
            debug(debug_dos, "CP/M-86 load: program min %u paras exceeds TPA %u "
                             "-> insufficient memory\n", (unsigned)total_min,
                  (unsigned)tpa_paras);
            return 0;
        }
        uint32_t want = total_max < tpa_paras ? total_max : tpa_paras;
        uint16_t gotmax = 0;
        grant_seg = mem_alloc_segment((uint16_t)want, &gotmax);
        uint32_t grant = 0;
        if(grant_seg)
            grant = want;
        else if(gotmax >= total_min)
        {
            // Capture gotmax before the retry call: mem_alloc_segment resets
            // *max on an exact-fit success, so reading it back would give 0.
            uint16_t retry_par = gotmax;
            uint16_t discard = 0;
            grant_seg = mem_alloc_segment(retry_par, &discard);
            grant = retry_par;
        }
        if(!grant_seg)
        {
            debug(debug_dos, "CP/M-86 load: could not allocate %u paras (min %u) "
                             "-> insufficient memory\n", (unsigned)want,
                  (unsigned)total_min);
            return 0;
        }
        uint32_t surplus = grant - total_min;
        // Spread surplus: even share per group wanting more, capped at max-min.
        uint16_t ex_par = ex_min, st_par = st_min;
        int nrels = (ex_max > ex_min) + (st_max > st_min);
        if(nrels && surplus)
        {
            // extra first, then stack (matches descriptor order)
            if(ex_max > ex_min)
            {
                uint32_t share = (surplus + nrels - 1) / nrels; // round up
                uint32_t room = (uint32_t)(ex_max - ex_min);
                if(share > room)
                    share = room;
                ex_par = ex_min + (uint16_t)share;
                surplus -= share;
                nrels--;
            }
            if(st_max > st_min && nrels && surplus)
            {
                uint32_t share = surplus; // last taker gets the remainder
                uint32_t room = (uint32_t)(st_max - st_min);
                if(share > room)
                    share = room;
                st_par = st_min + (uint16_t)share;
            }
        }
        extra_par = (extra && ex_par) ? ex_par : 0;
        stack_par = (stack && st_par) ? st_par : 0;

    }

    // Place groups contiguously within the grant.
    {
        uint16_t cur = grant_seg;
        cpm_code_seg = cur;
        cur = (uint16_t)(cur + code_par);
        if(model_8080)
        {
            cpm_data_seg = cpm_base_seg = cpm_code_seg;
        }
        else
        {
            cpm_data_seg = cpm_base_seg = cur;
            cur = (uint16_t)(cur + data_par);
        }
        if(extra_par)
        {
            extra_seg = cur;
            cur = (uint16_t)(cur + extra_par);
        }
        if(stack_par)
        {
            stack_seg = cur;
            cur = (uint16_t)(cur + stack_par);
        }
    }

    // Real CCP/M-86 (Regnecentralen RC759, Piccoline XIOS 3.1, measured on
    // physical-accurate MAME) does NOT hand a freestanding transient SS==DS.
    // It gives it a small (96-byte / 6-paragraph) scratch stack in a segment
    // BELOW the base/data segment (spec-mandated small default stack, DR
    // CP/M-86 System Guide §4.1.2 -- programs are required to switch to their
    // own SS=DS stack early in startup). emu2 used to just set SS=DS
    // unconditionally, which is a *more lenient* environment than real
    // hardware: a crt0 that forgets the SS=DS switch runs "by accident" under
    // the old emu2 (SS==DS trivially) but corrupts memory on real CCP/M-86
    // (a near pointer to an SS-relative stack local resolves against the
    // wrong segment). Reproduce the same non-equal-segment hazard here so a
    // program that passes under emu2 is real evidence, not a coincidence of
    // a too-forgiving loader. Only applies when the CMD did not request its
    // own explicit STACK group (type 4) -- that case already gets a real,
    // separately-allocated segment above.
    if(!stack_seg)
    {
        uint16_t avail = 0;
        uint16_t scratch = mem_alloc_segment(6, &avail);
        if(scratch)
        {
            stack_seg = scratch;
            stack_par = 6;
            memset(memory + (uint32_t)stack_seg * 16, 0, (uint32_t)stack_par * 16);
        }
        // If allocation fails (memory exhausted), fall through to the old
        // SS=DS behavior below rather than failing the load.
    }

    // Auxiliary groups (CMD types 5..8) map to base-page descriptor slots 4..7
    // (segment words at offsets 0x1B/0x21/0x27/0x2D).  Relocatable CMDs keep a
    // self-relocation buffer in an aux group and need its descriptor set, so load
    // each aux group's image and record its segment for the descriptor loop.
    uint16_t aux_seg[4] = {0, 0, 0, 0};
    uint16_t aux_par[4] = {0, 0, 0, 0};
    for(int i = 0; i < ng; i++)
    {
        if(g[i].type < 5 || g[i].type > 8)
            continue;
        int slot = g[i].type - 5;
        uint16_t want = g[i].max > g[i].min ? g[i].max : g[i].min;
        if(want < g[i].length)
            want = g[i].length;
        if(!want)
            want = 1;
        uint16_t avail = 0;
        uint16_t s = mem_alloc_segment(want, &avail);
        if(!s)
            return 0;
        if(g[i].length)
        {
            fseek(f, g[i].file_off, SEEK_SET);
            fread(memory + (uint32_t)s * 16, 1, (uint32_t)g[i].length * 16, f);
        }
        aux_seg[slot] = s;
        aux_par[slot] = want;
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
        if(!dirty_groups)
            memset(memory + (uint32_t)extra_seg * 16, 0, (uint32_t)extra_par * 16);
        if(extra->length)
        {
            fseek(f, extra->file_off, SEEK_SET);
            fread(memory + (uint32_t)extra_seg * 16, 1, (uint32_t)extra->length * 16, f);
        }
    }
    if(stack_seg && stack)
    {
        // (Our spec-default scratch stack, when stack==0, already memset its
        // own segment above at allocation time -- this block only applies to
        // a CMD-declared STACK group, which has real file image data to load.)
        if(!dirty_groups)
            memset(memory + (uint32_t)stack_seg * 16, 0, (uint32_t)stack_par * 16);
        if(stack->length)
        {
            fseek(f, stack->file_off, SEEK_SET);
            fread(memory + (uint32_t)stack_seg * 16, 1, (uint32_t)stack->length * 16, f);
        }
    }

    // P_LOAD fixup table: hdr[0x7F] bit 7 set, hdr[0x7D..0x7E] = file record
    // number (x128 = byte offset).  Each 4-byte entry: byte0=(loc<<4|tgt) group
    // numbers (1=CODE 2=DATA 3=EXTRA 4=STACK 5..8=AUX), bytes1-2=paragraph in
    // loc group (LE), byte3=byte offset.  Add tgt's load segment to the word.
    // Ends at first all-zero entry.
    if(hdr[0x7F] & 0x80)
    {
        uint16_t grp_seg[9] = {0};
        grp_seg[GT_CODE]  = cpm_code_seg;
        grp_seg[GT_DATA]  = data ? cpm_data_seg : 0;
        grp_seg[GT_EXTRA] = extra_seg;
        grp_seg[GT_STACK] = stack_seg;
        grp_seg[5] = aux_seg[0];
        grp_seg[6] = aux_seg[1];
        grp_seg[7] = aux_seg[2];
        grp_seg[8] = aux_seg[3];

        uint32_t fixrec = (uint32_t)hdr[0x7D] | ((uint32_t)hdr[0x7E] << 8);
        uint32_t pos = fixrec * 128;
        unsigned applied = 0;
        for(;;)
        {
            uint8_t rec[4];
            if(fseek(f, pos, SEEK_SET) != 0 || fread(rec, 1, 4, f) != 4)
                break;
            if(rec[0] == 0 && rec[1] == 0 && rec[2] == 0 && rec[3] == 0)
                break; // table ends at the first all-zero record
            unsigned loc_grp = (rec[0] >> 4) & 0x0F;
            unsigned tgt_grp = rec[0] & 0x0F;
            uint16_t para = (uint16_t)(rec[1] | (rec[2] << 8));
            uint8_t offs = rec[3] & 0x0F;
            if(loc_grp > 8 || tgt_grp > 8 || grp_seg[loc_grp] == 0 ||
               (grp_seg[tgt_grp] == 0 && tgt_grp != 0))
            {
                debug(debug_dos,
                      "CP/M-86 load: fixup #%u names undefined group "
                      "(byte 0x%02x) -- skipped\n",
                      applied, rec[0]);
                pos += 4;
                continue;
            }
            uint32_t addr = ((uint32_t)(grp_seg[loc_grp] + para) << 4) + offs;
            put16(addr, (get16(addr) + grp_seg[tgt_grp]) & 0xFFFF);
            applied++;
            pos += 4;
        }
        debug(debug_dos, "CP/M-86 load: applied %u load-time fixup(s)\n", applied);
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
    CPM_GDESC(0x18, (uint32_t)aux_par[0] << 4, aux_seg[0]); // aux group 1
    CPM_GDESC(0x1E, (uint32_t)aux_par[1] << 4, aux_seg[1]); // aux group 2
    CPM_GDESC(0x24, (uint32_t)aux_par[2] << 4, aux_seg[2]); // aux group 3
    CPM_GDESC(0x2A, (uint32_t)aux_par[3] << 4, aux_seg[3]); // aux group 4
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
    // SS: NOT cpm_base_seg. stack_seg is now always set (either the CMD's own
    // declared STACK group, or the spec-default scratch stack allocated
    // above), so the program starts on a segment genuinely separate from DS,
    // exactly as measured on real CCP/M-86. A conforming program's own crt0
    // is responsible for switching to SS=DS with SP from base-page word 6
    // (the "sp_top" field below, still base_seg-relative -- unchanged) as its
    // first act; this entry stack only has to survive up to that switch.
    uint16_t ss_seg  = stack_seg;
    uint32_t ssbp    = (uint32_t)ss_seg * 16;
    uint16_t ss_size = (uint16_t)((uint32_t)stack_par * 16);
    uint16_t entry_sp = (uint16_t)(ss_size - 4); // room for the far exit addr
    cpuSetSS(ss_seg);
    // Lay the far "exit" address (PSP:0000 = INT 20h) at the top of this
    // entry stack and enter with SP there: a plain "RETF to exit" pops it.
    // (Historical note, still true for programs that reload SS=DS themselves:
    // sp_top/bp+sp_top below is the DS-relative "top of stack" a program
    // reads from base-page word 6 once it has done that switch -- it is a
    // different address space from the entry-time SS used here.)
    put16(ssbp + entry_sp + 0, 0);   // exit IP  -> PSP:0000 = INT 20h
    put16(ssbp + entry_sp + 2, psp); // exit CS  = PSP segment
    cpuSetSP(entry_sp);
    // 8080-model / CP/M-80-heritage programs terminate with a near RET to the
    // warm-boot vector (offset 0), which (unlike a far RETF to PSP:0000) stays in
    // CS and lands at code:0000.  In the 8080 model the code entry is 0x100, so
    // code:0000 is never reached by normal execution -- arm a warm-boot trap there
    // so such a transfer terminates the program instead of executing the base page
    // as code.  (cpm_wboot_seg = 0 disables the trap for the small/compact models,
    // whose entry *is* code:0000.)
    cpm_wboot_seg = model_8080 ? cpm_code_seg : 0;
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
    cpm_extra_seg = extra_seg;
    cpm_stack_seg = stack_seg;
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
// Some CP/M programs (e.g. VEDIT PLUS) inadvertently leave a stale value in S1
// which would make the block number huge and seek past EOF so the first read
// returns no data and the program treats the file as empty.  Clear S1 before
// delegating so only EX (byte 0x0C) drives the file position.  (The LRBC now
// lives at FCB+32 / 0x20, not in S1, so this clear does not discard LRBC data.)
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
            s1 = cpm_lrbc_encode(cpm_ext_bytes);
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
        // A real CP/M directory holds one entry per 16K extent, so a big file
        // owns several.  Whether a search returns them all or just one depends on
        // the FCB: a '?' (0x3F) drive byte matches every entry (the raw-directory
        // read SDIR/STAT use to total sizes), and a '?' in the EX field matches
        // all extents; any other EX matches only that extent.  So emit every
        // extent only in those "match all" cases -- otherwise just the first, so
        // a plain search (DIR uses drive=current, EX=0) lists each file once
        // instead of once per extent.
        cpm_all_ext = (memory[fcb_addr] == 0x3F) || (memory[fcb_addr + 0x0C] == 0x3F);
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
    if(!cpm_all_ext)
        cpm_ext_left = 0; // matched the first extent only: next call advances files
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

// Characters (besides the structural '.' and 'd:') that end a filename token in
// a CP/M command line, as recognized by F_PARSE.  A null or carriage return ends
// the whole input; the rest are separators between successive specifications.
static int cpm_fname_delim(unsigned c)
{
    return c <= ' ' /* null, tab, CR, space, other control */ ||
           c == ',' || c == ';' || c == '=' || c == '<' || c == '>' ||
           c == '|' || c == '[' || c == ']';
}

// BDOS 152 (F_PARSE): parse one ASCII file specification into an FCB.  The PFCB
// at DS:DX holds two words -- +0 the offset of the input string, +2 the offset
// of the target FCB (both relative to the program's DS).  The FCB is filled in:
// drive (0 = default, 1..16 = A..P), an 8-char name and 3-char type uppercased
// and blank-padded with '*' expanded to '?', and ex/s1/s2/rc cleared.  Returns,
// per the DRI spec (matching what DIR.CMD expects):
//     0xFFFF  the file specification was invalid
//     0       it was the last one (terminated by a null or carriage return)
//     else    the offset of the delimiter that ended it, so the caller can step
//             past that delimiter and parse the following specification.
// Without this, DIR (which loops until F_PARSE returns 0) spun forever, as the
// unimplemented default returned 0x00FF -- nonzero, so "more to parse" -- every
// time.  Parsing is deliberately lenient (over-long fields and stray dots are
// truncated, never reported as 0xFFFF) so a malformed tail can't wedge a caller.
static unsigned cpm_parse_fcb(unsigned dx)
{
    uint32_t pfcb = cpuGetAddrDS(dx);
    uint16_t sptr = get16(pfcb + 0); // offset of the input ASCII string
    uint16_t fptr = get16(pfcb + 2); // offset of the FCB to fill
    uint32_t fcb = cpuGetAddrDS(fptr);

    // Read the i'th input byte, wrapping the 16-bit offset inside the segment.
#define CPM_IN(i) memory[cpuGetAddrDS((uint16_t)(sptr + (i)))]

    // Initialize the FCB: default drive, blank 8.3 name, cleared ex/s1/s2/rc.
    memory[fcb] = 0;
    memset(memory + fcb + 1, ' ', 11);
    memset(memory + fcb + 12, 0, 4);

    unsigned i = 0;
    while(CPM_IN(i) == ' ' || CPM_IN(i) == '\t') // skip leading blanks
        i++;
    if(CPM_IN(i) == 0 || CPM_IN(i) == '\r') // nothing but blanks then end-of-line
        return 0;

    // Optional "d:" drive prefix.
    unsigned d = CPM_IN(i) & ~0x20u; // fold to upper case
    if(d >= 'A' && d <= 'Z' && CPM_IN(i + 1) == ':')
    {
        memory[fcb] = d - 'A' + 1; // 1 = A: .. 16 = P:
        i += 2;
    }

    // Name (8 bytes at FCB+1), then type (3 bytes at FCB+9) after a '.'.
    unsigned fpos = 1, maxlen = 8, n = 0;
    for(;;)
    {
        unsigned c = CPM_IN(i);
        if(c == 0 || c == '\r' || cpm_fname_delim(c))
            break;
        if(c == '.')
        {
            if(fpos != 1) // only the first '.' separates name from type
                break;
            fpos = 9, maxlen = 3, n = 0;
            i++;
            continue;
        }
        if(c == '*') // expand to '?' through the end of the current field
        {
            while(n < maxlen)
                memory[fcb + fpos + n++] = '?';
            i++;
            continue;
        }
        if(n < maxlen) // store the char (uppercased); over-long fields truncate
            memory[fcb + fpos + n++] = (c >= 'a' && c <= 'z') ? c - 0x20 : c;
        i++;
    }

    // Null/CR ends the whole line (return 0); any other delimiter separates this
    // spec from the next, so hand back its offset for the caller to step over.
    unsigned t = CPM_IN(i);
    if(t == 0 || t == '\r')
        return 0;
    return (uint16_t)(sptr + i);
#undef CPM_IN
}

// BDOS 47 (P_CHAIN, "Chain To Program"): load and run the program named in the
// default DMA buffer, reusing the current PSP.  Multi-pass tools chain this way
// (e.g. a compiler driver -> "R PASS2" -> ... -> LINK86), so without it only the
// first program runs.  The command is a raw null/CR-terminated console-style line
// (NOT the CCP length-prefixed form).  Returns 1 if a program was loaded, 0 if
// the command was empty or the program was not found (caller then terminates).
static int cpm_chain(void)
{
    uint32_t dma = (uint32_t)cpm_dma_seg * 16 + cpm_dma_off;
    char cmd[130];
    unsigned n = 0;

    for(unsigned i = 0; i < 128 && n < sizeof(cmd) - 1; i++)
    {
        uint8_t c = memory[(dma + i) & 0xFFFFF];
        if(c == 0 || c == '\r')
            break;
        cmd[n++] = (char)c;
    }
    cmd[n] = 0;
    debug(debug_dos, "CP/M BDOS 47 (chain): command=\"%s\"\n", cmd);

    // Parse "[drive:] prog [tail]".  Strip a leading standalone "R" token (DR's
    // "run" loader, which itself uses BDOS 59): func 47 already does load+run.
    char *p = cmd;
    char prog[64];
    unsigned pn;
    for(;;)
    {
        while(*p == ' ')
            p++;
        if(p[0] && p[1] == ':')
            p += 2;
        pn = 0;
        while(*p && *p != ' ' && pn < sizeof(prog) - 5)
            prog[pn++] = *p++;
        prog[pn] = 0;
        while(*p == ' ')
            p++;
        if(pn == 1 && (prog[0] == 'R' || prog[0] == 'r') && *p)
            continue;
        break;
    }
    const char *tail = p;
    if(pn == 0)
        return 0;

    // Open <prog> then <prog>.CMD from the current host drive.
    FILE *f = fopen(prog, "rb");
    if(!f)
    {
        char withext[70];
        snprintf(withext, sizeof(withext), "%s.CMD", prog);
        f = fopen(withext, "rb");
    }
    if(!f)
    {
        debug(debug_dos, "CP/M chain: program \"%s\" not found\n", prog);
        return 0;
    }

    // Free the outgoing program's memory before loading the next pass, else a
    // deep chain leaks ~64K/pass.  (In the 8080 model base aliases data==code.)
    if(cpm_extra_seg)
        mem_free_segment(cpm_extra_seg);
    if(cpm_stack_seg)
        mem_free_segment(cpm_stack_seg);
    if(cpm_data_seg && cpm_data_seg != cpm_code_seg)
        mem_free_segment(cpm_data_seg);
    if(cpm_code_seg)
        mem_free_segment(cpm_code_seg);
    cpm_code_seg = cpm_data_seg = cpm_base_seg = cpm_extra_seg = cpm_stack_seg = 0;

    int ok = cpm86_load_cmd(f, tail);
    fclose(f);
    if(!ok)
    {
        debug(debug_dos, "CP/M chain: failed to load \"%s\"\n", prog);
        return 0;
    }

    // The BDOS entry returns via a trailing IRET (pops IP/CS/FLAGS from SS:SP),
    // but the loader set CS:IP/SP directly with no frame -- synthesize one so the
    // IRET lands at the chained program's entry.
    uint16_t ncs = cpuGetCS(), nip = cpuGetIP(), nsp = cpuGetSP();
    uint32_t sbase = (uint32_t)cpuGetSS() * 16;
    nsp -= 2;
    put16(sbase + nsp, 0x0200); // FLAGS: IF set
    nsp -= 2;
    put16(sbase + nsp, ncs);
    nsp -= 2;
    put16(sbase + nsp, nip);
    cpuSetSP(nsp);
    return 1;
}

void intr_cpm_bdos(void)
{
    unsigned func = cpuGetCX() & 0xFF;
    unsigned dx = cpuGetDX();

    // BDOS preserves SI/DI/BP/DS/SS (CP/M-86 System Guide S4.1); not ES.
    unsigned saved_si = cpuGetSI(), saved_di = cpuGetDI(), saved_bp = cpuGetBP();
    unsigned saved_ds = cpuGetDS(), saved_ss = cpuGetSS();
    // Console-output functions (2, 9, 6-out) have no return value; save all
    // regs so bdos_ret() doesn't clobber the caller's AX/BX/CX/DX.
    unsigned saved_ax = cpuGetAX(), saved_bx = cpuGetBX();
    unsigned saved_cx = cpuGetCX(), saved_dx = cpuGetDX();
    int no_return = (func == 2) || (func == 9) ||
                    (func == 6 && (dx & 0xFF) < 0xFD);

    switch(func)
    {
    case 0:   // System Reset / program termination
    case 143: // P_TERM: Terminate Process (MP/M, Concurrent CP/M, CP/M-86 v4).
              // On a single-user system -- which emu2 emulates -- this behaves
              // exactly like function 0: end the program.  DL holds a termination
              // code we have no use for.  Transients such as DIR exit through
              // this; without it they ran off the end of their code into a
              // zero-filled NOP-sled that wrapped around and re-ran them forever.
        debug(debug_dos, "CP/M BDOS %u: program termination\n", func);
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
    case 16: // close file.  CP/M 3 LRBC: if the program stored a partial last-
    {        // record byte count at FCB+32 (FCB+0x20), trim the host file to
             // that exact length before closing so its on-disk size is no longer
             // record-rounded.  Per CP/M 3 / DOS Plus, the LRBC lives at FCB+32,
             // NOT in S1 (FCB+13); programs must reset FCB+32 to 0 before
             // sequential I/O (the open handler only fills it, never clears it).
        if(cpm_lrbc_trunc)
        {
            uint32_t fcb = cpuGetAddrDS(dx);
            unsigned lrbc = memory[fcb + 0x20] & 0x7F; // FCB+32; 0 = full last record
            unsigned long sz = get32(fcb + 0x10);
            if(lrbc && sz)
            {
                unsigned long exact = cpm_lrbc_exact(sz, lrbc);
                if(exact < sz)
                    dos_truncate_fcb(fcb, exact);
            }
        }
        bdos_ret(bdos_via_dos(0x10));
        break;
    }
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
        // CP/M 3 / DOS Plus exact-size: the documented way to record a file's
        // last-record byte count is to re-issue F_ATTRIB with F6' (bit 7 of FCB
        // byte 6) set and the byte count in FCB+0x20.  When LRBC truncation is
        // enabled (off under EMU2_LRBC_NOTRUNC or CP/M 2.2), trim the closed
        // host file to the exact length that count implies.  dos_truncate_fcb_name
        // refuses any trim larger than one 128-byte record, so a stray F6' or a
        // bogus count cannot discard real data.
        if(cpm_lrbc_trunc && (memory[fcb + 6] & 0x80))
        {
            unsigned s1 = memory[fcb + 0x20] & 0x7F; // 0 = full last record
            long sz = dos_fcb_size_by_name(fcb);
            if(s1 && sz > 0)
            {
                unsigned long exact = cpm_lrbc_exact((unsigned long)sz, s1);
                if(exact < (unsigned long)sz)
                    dos_truncate_fcb_name(fcb, exact);
            }
        }
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

    case 15: // open file (+ CP/M-3 LRBC byte count at FCB+32)
    {
        uint32_t fcb = cpuGetAddrDS(dx);
        // Snapshot FCB+32 BEFORE calling the DOS open handler: dos.c zeroes
        // FCB+32 (the "current record" field) during open, so we must capture
        // the caller's 0xFF signal before it is overwritten.
        unsigned want_lrbc = cpm_lrbc && (memory[fcb + 0x20] == 0xFF);
        unsigned r = bdos_via_dos(0x0F);
        // Per CP/M 3 / DOS Plus: FCB+32 (FCB+0x20) carries the last-record byte
        // count (LRBC).  On open, the caller signals it wants the LRBC by
        // pre-setting FCB+32 to 0xFF; we then replace it with the actual count.
        // If FCB+32 was not 0xFF the program does not want LRBC, so leave it at 0.
        // The LRBC does NOT go in S1 (FCB+13); that field drives the sequential
        // block number and must remain 0 for normal I/O.
        if(r != 0xFF && want_lrbc)
        {
            unsigned long sz = get32(fcb + 0x10);
            memory[fcb + 0x20] = cpm_lrbc_encode(sz);
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

    case 128: // M_ALLOC: allocate memory via MPB {start,min,max,pd,flags}.
    {
        uint32_t mpb = cpuGetAddrDS(dx);
        uint16_t need = get16(mpb + 2); // mpb_min
        uint16_t want = get16(mpb + 4); // mpb_max
        uint16_t avail = 0;
        uint16_t got = want;
        uint16_t seg = mem_alloc_segment(want, &avail);
        if(!seg && avail >= need)
        {
            uint16_t a2 = 0;
            seg = mem_alloc_segment(avail, &a2);
            got = avail;
        }
        if(getenv("CPM86_TRACE_ALLOC"))
            fprintf(stderr, "BDOS128: need=%u want=%u avail=%u got=%u seg=%u\n",
                    need, want, avail, got, seg);
        if(seg && got >= need)
        {
            put16(mpb + 0, seg);
            put16(mpb + 4, got); // return actual grant in mpb_max
            bdos_ret(0);
            cpuSetCX(0);
        }
        else
        {
            if(seg)
                mem_free_segment(seg);
            bdos_ret(0xFFFF);
            cpuSetCX(3);
        }
        break;
    }

    case 130: // M_FREE: free segment from MFPB {start, pd}.
    {
        uint32_t mfpb = cpuGetAddrDS(dx);
        mem_free_segment(get16(mfpb + 0));
        bdos_ret(0);
        cpuSetCX(0);
        break;
    }

    case 104: // T_SET: accept call, return success (clock is read-only).
        bdos_ret(0);
        break;

    case 155: // T_SECONDS: like T_GET but also writes BCD seconds to DAT+4.
    case 105: // T_GET: fills DAT at DS:DX; days since 1978-01-01, BCD h/m; AL=BCD sec.
    {
        time_t now = time(0);
        struct tm *lt = localtime(&now);
        struct tm epoch = {.tm_year = 78, .tm_mon = 0, .tm_mday = 1, .tm_hour = 12};
        long days = (long)(difftime(now, mktime(&epoch)) / 86400) + 1;
        uint8_t bcd_sec = ((lt->tm_sec / 10) << 4) | (lt->tm_sec % 10);
        uint32_t dat = cpuGetAddrDS(dx);
        put16(dat, (uint16_t)days);
        memory[dat + 2] = ((lt->tm_hour / 10) << 4) | (lt->tm_hour % 10);
        memory[dat + 3] = ((lt->tm_min / 10) << 4) | (lt->tm_min % 10);
        if(func == 155)
            memory[dat + 4] = bcd_sec;
        debug(debug_dos, "CP/M get date/time: day %ld %02d:%02d:%02d\n", days,
              lt->tm_hour, lt->tm_min, lt->tm_sec);
        bdos_ret(bcd_sec);
        break;
    }

    case 152: // F_PARSE: parse a filename from DS:[PFCB] into an FCB (CP/M 3).
        bdos_ret(cpm_parse_fcb(dx));
        break;

    case 47: // P_CHAIN: Chain To Program
        if(!cpm_chain())
            exit(0);
        break;

    default:
        debug(debug_dos, "CP/M BDOS %u: UNIMPLEMENTED (DX=%04x)\n", func, dx);
        bdos_ret(0xFF); // 0xFF = error / not found for most file funcs
        break;
    }

    // Restore caller's preserved registers (P_CHAIN loads a new program; skip).
    if(func != 47)
    {
        cpuSetSI(saved_si);
        cpuSetDI(saved_di);
        cpuSetBP(saved_bp);
        cpuSetDS(saved_ds);
        cpuSetSS(saved_ss);
        if(no_return)
        {
            cpuSetAX(saved_ax);
            cpuSetBX(saved_bx);
            cpuSetCX(saved_cx);
            cpuSetDX(saved_dx);
        }
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
