#include "dbg.h"
#include "env.h"
#include "version.h"
#include "os.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

char *prog_name;

void print_version(void)
{
    printf("EMU2 - Simple x86 + DOS and CP/M-86 Emulator, version " EMU2_VERSION
#ifdef __DATE__
           "  (Compiled " __DATE__ ")"
#endif
#ifdef GIT_SHA
           "  [" GIT_SHA "]"
#endif
           "\n");
}

NORETURN void print_usage(void)
{
    print_version();
    printf("\n"
           "Usage: %s [options] <prog.exe> [args...] [-- environment vars]\n"
           "\n"
           "Options (processed before program name):\n"
           "  -h            Show this help.\n"
           "  -b <addr>     Load header-less binary at address.\n"
           "  -r <seg>:<ip> Specify a run address to start execution.\n"
           "                (only for binary loaded data).\n"
           "  -m <kb>       CP/M-86 TPA size in KB; same as CPM86_TPA_KB env var.\n"
           "                Default: ~640K (standard DOS arena).\n"
           "  -P <byte>     Fill free CP/M-86 memory with <byte> before loading\n"
           "                (same as CPM86_POISON=<byte>); implies -D.\n"
           "  -D            Fill free CP/M-86 memory with 0xFF before loading\n"
           "                (same as CPM86_DIRTY_GROUPS).\n"
           "\n"
           "Environment variables:\n"
           "  %-18s  Base name of a file to write the debug log, defaults to\n"
           "\t\t      the exe name if not given.\n"
           "  %-18s  List of debug options to activate, from the following:\n"
           "\t\t      'cpu', 'int', 'port', 'dos', 'video'.\n"
           "  %-18s  DOS program name, if not given use the unix name.\n"
           "  %-18s  DOS default (current) drive letter, if not given use 'C:'\n"
           "  %-18s  DOS current working directory, use 'C:\\' if not given.\n"
           "  %-18s  Set unix path as root of drive 'n', by default all drives\n"
           "\t\t      point to the unix working directory.\n"
           "  %-18s  Set DOS code-page. Set to '?' to show list of code-pages.\n"
           "  %-18s  Limit DOS memory to 512KB, fixes some old buggy programs.\n"
           "  %-18s  Specifies a DOS append paths, separated by ';'.\n"
           "  %-18s  Set version of DOS to emulate, e.g. '2.11', '3.20', etc.\n"
           "  %-18s  Setup text mode with given number of rows, from 12 to 50.\n"
           "  %-18s  CP/M-86 disk block size: auto|1k|2k|4k|8k|16k (per drive\n"
           "\t\t      with EMU2_CPM_DISK_<letter>).\n"
           "  %-18s  CP/M-86 auto-disk target free-space percent (default 25).\n"
           "  %-18s  CP/M-86 CP/M-3 limits: 2048 extents / 32MB files / 512MB.\n"
           "  %-18s  CP/M-86 reported version, e.g. '3.1' (default) or '2.2'.\n"
           "\t\t      3.0+ enables the last-record byte count for exact sizes.\n"
           "  %-18s  Keep CP/M-86 output padded to 128-byte records: report the\n"
           "\t\t      LRBC byte count but do not trim host files to it.\n"
           "  %-18s  Use the ISX last-record byte count convention (unused bytes)\n"
           "\t\t      instead of DOS Plus (used bytes); default off (DOS Plus).\n"
           "  %-18s  CP/M-86 console emulation (VT52/colour); set 0 to disable.\n"
           "  %-18s  CP/M-86 TPA size in KB; same as -m.\n"
           "  %-18s  Fill free memory with <byte> before load (debug).\n"
           "  %-18s  Fill free memory with 0xFF before load (debug).\n",
           prog_name, ENV_DBG_NAME, ENV_DBG_OPT, ENV_PROGNAME, ENV_DEF_DRIVE, ENV_CWD,
           ENV_DRIVE "n", ENV_CODEPAGE, ENV_LOWMEM, ENV_APPEND, ENV_DOSVER, ENV_ROWS,
           "EMU2_CPM_DISK", "EMU2_CPM_FREE", "EMU2_CPM_PLUS", ENV_CPMVER,
           ENV_LRBC_NOTRUNC, ENV_CPM_ISXLRBC, "EMU2_VT52",
           "CPM86_TPA_KB", "CPM86_POISON", "CPM86_DIRTY_GROUPS");
    exit(EXIT_SUCCESS);
}

NORETURN void print_usage_error(const char *format, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", prog_name);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fprintf(stderr, "\nTry '%s -h' for more information.\n", prog_name);
    exit(EXIT_FAILURE);
}

NORETURN void print_error(const char *format, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", prog_name);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

static FILE *debug_files[debug_MAX];
static const char *debug_names[debug_MAX] = {"cpu", "int", "port", "dos", "video"};

static FILE *open_log_file(const char *base, const char *type)
{
    char log_name[64 + strlen(base) + strlen(type)];
    int fd = -1;
    for(int i = 0; fd == -1 && i < 1000; i++)
    {
        sprintf(log_name, "%s-%s.%d.log", base, type, i);
        fd = open(log_name, O_CREAT | O_EXCL | O_WRONLY, 0666);
    }
    if(fd == -1)
        print_error("can't open debug log '%s'\n", log_name);
    fprintf(stderr, "%s: %s debug log on file '%s'.\n", prog_name, type, log_name);
    return fdopen(fd, "w");
}

static void close_log_files(void)
{
    for(int i = 0; i < debug_MAX; i++)
        if(debug_files[i] != 0)
        {
            fclose(debug_files[i]);
            debug_files[i] = 0;
        }
}

void init_debug(const char *base)
{
    if(getenv(ENV_DBG_NAME))
        base = getenv(ENV_DBG_NAME);
    if(getenv(ENV_DBG_OPT))
    {
        // Parse debug types:
        const char *spec = getenv(ENV_DBG_OPT);
        for(int i = 0; i < debug_MAX; i++)
        {
            if(strstr(spec, debug_names[i]))
                debug_files[i] = open_log_file(base, debug_names[i]);
        }
        atexit(close_log_files);
    }
}

int debug_active(enum debug_type dt)
{
    if(dt < debug_MAX)
        return debug_files[dt] != 0;
    else
        return 0;
}

void debug(enum debug_type dt, const char *format, ...)
{
    va_list ap;
    if(debug_active(dt))
    {
        va_start(ap, format);
        vfprintf(debug_files[dt], format, ap);
        va_end(ap);
        fflush(debug_files[dt]);
    }
}
