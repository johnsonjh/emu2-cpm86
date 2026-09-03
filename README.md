# emu2-cpm86

A simple text-mode x86 + DOS and CP/M-86 emulator for UNIX-like systems.

## Overview

This is an 8088/8086/80186/80286 CPU emulator and a reimplementation of
the Microsoft/IBM MS-DOS/PC-DOS and Digital Research CP/M-86 operating
systems for the UNIX console.

Most DOS and CP/M-86 system calls and text-mode video I/O is supported.

It is a fork of the excellent `emu2` emulator available from
[https://github.com/dmsc/emu2](https://github.com/dmsc/emu2)
and made available under the terms of the [GPL-v2.0 license](LICENSE).

## Availability

* [`johnsonjh/emu2-cpm86`&nbsp;@**GitLab**](https://gitlab.com/johnsonjh/emu2-cpm86)
* [`johnsonjh/emu2-cpm86`&nbsp;@**GitHub**](https://github.com/johnsonjh/emu2-cpm86)

## Using the emulator

To run a CP/M-86 `.cmd` file or a DOS `.exe` or `.com` file, simply
load it with:

```
emu2 myprog.exe
```

## Command-line options

The emulator accepts some options in the command line and more options as
environment variables, this allows child process (programs run by your DOS
program) to inherit the configuration.

The full usage is:
```
emu2 [options] <prog.exe> [args...] [-- environment vars]
```

Options (should be placed *before* the DOS or CP/M-86 program name):
- `-h`        Shows a brief help.
- `-b addr`   Load header-less binary at given address (to load ROMs or test data).
- `-r <seg>:<ip>`  Specify a run address to start execution (only for binary loaded data).

## Debugging

To trace what emu2 is doing, set `EMU2_DEBUG` to a comma- or space-separated
list of one or more keywords.  Each active keyword writes a separate log file
named `<base>-<keyword>.<n>.log` (where `<base>` is the program name or the
value of `EMU2_DEBUG_NAME`, and `<n>` is a sequence number to avoid overwriting
old logs).  The log files are flushed after every line and closed on exit.

The available keywords and what they trace:

| Keyword | Description |
|--------:|:------------|
| `cpu`   | Every instruction executed: all registers + flags + disassembly.  **Very** verbose; expect gigabytes for a non-trivial program.  Useful for tracing crashes or unexpected branches. |
| `int`   | Hardware interrupt dispatch (IRQ handling, emulator update cycles, INT 19, INT 2Fh multiplexer, DOS INT 25h raw-disk stubs).  Much lighter than `cpu`; good first step. |
| `port`  | Every I/O port read and write (`IN`/`OUT`).  Useful when a program talks to hardware (keyboard, video, timer). |
| `dos`   | All DOS INT 21h and CP/M-86 BDOS calls with their arguments and return values.  For CP/M-86 programs this also shows BDOS function names, FCB contents, file search results, memory allocation, and chain operations.  The most useful keyword for debugging application-level misbehaviour. |
| `video` | Terminal/video initialisation and mode changes (screen size, text mode, clear events).  Not individual character output.  Useful when the display layout looks wrong. |

### Debugging examples

```sh
# Trace all DOS/BDOS calls made by a DOS or CP/M-86 program
env EMU2_DEBUG="dos" emu2 myprog.cmd

# Trace both DOS/BDOS calls and interrupt dispatch
env EMU2_DEBUG="dos int" emu2 myprog.cmd

# Save logs under a fixed base name (avoids per-run sequence numbers)
env EMU2_DEBUG="dos" EMU2_DEBUG_NAME="trace" emu2 myprog.cmd
# -> writes trace-dos.0.log
```

## Environment variables

| Variable | Description |
|---------:|:------------|
| `EMU2_DEBUG_NAME` | Base name of a file to write the debug log, defaults to the exe name if not given. |
| `EMU2_DEBUG` | Comma- or space-separated list of keywords to trace. See the *Debugging* section above for details. Available keywords: `cpu`, `int`, `port`, `dos`, `video`. |
| `EMU2_PROGNAME` | DOS program name, if not given try to convert the UNIX name to an equivalent DOS path. |
| `EMU2_DEFAULT_DRIVE` | DOS default (current) drive letter, if not given use `C:` |
| `EMU2_CWD` | DOS current working directory, if not given tries to convert the current directory to the equivalent DOS path inside the DOS default drive, or the root of `C:` if not possible. |
| `EMU2_DRIVE_n` | Set UNIX path as root of drive `n`, by default all drives point to the UNIX working directory. |
| `EMU2_APPEND` | Sets a list of paths to search for *data* files on open, emulating the DOS `APPEND` command. Only *data files with a relative path* are included in the search, and the search is relative to the current working directory if no drive letter is specified in the APPEND path. For example, if set to "`TXT;C:\IN`", when opening the file "`CAT.TXT`" the file is searched as "`CAT.TXT`", "`TXT\CAT.TXT`" and "`C:\IN\CAT.TXT`" in turn. |
| `EMU2_CPM_APPEND` | Sets a list of CP/M-86 drive letters (e.g. `D:;E:`) to search for *data* files on open for CP/M-86 programs.  Works very much like the DOS `EMU2_APPEND` option, but for CP/M-86 programs. This mapping mapping multiple drives to appear as part of the current drive for programs that search and opening *data* files. |
| `EMU2_CODEPAGE` | Set DOS code-page to the specified string. Set to '?' to show list of included code-pages, multiple aliases separated with commas.  Set to a file name to read the mapping table from a file with the Unicode value for each byte.  You can [download](ftp://ftp.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/PC/) additional mapping tables via FTP. The code-page setting affects keyboard input and screen output, but does not change the DOS NLS information. The default code-page is `CP437`. |
| `EMU2_LOWMEM` | Limits main memory to 512KB, this fixes some old DOS programs with a bug that checks available memory using "signed" comparison instructions (`JLE` instead of `JBE`). This is needed at least for MASM versions 1.0 and 1.10. |
| `EMU2_DOSVER` | Changes the reported DOS version, allowing programs that checks this version to run.  You can specify a major version or a major dot minor, for example "3.20", "2.11" or "5". |
| `EMU2_ROWS` | Sets up the VGA text mode to the given number of rows, from 12 to 50 at the program start. Some full-screen DOS programs will retrieve this info and adjust the screen properly, some other will ignore this and setup the text mode again. |
| `EMU2_CPU_SPEED` | Limits the emulated CPU speed to at most the given number of instructions per millisecond. For reference, a value of 1000 (1 MIPS) approximates a fast 8086 or slow 80286. By default (or when set to 0), there is no limit; a modern PC can typically reach 200,000 or more instructions per millisecond. Note that this does not accurately emulate a specific CPU speed, since real 8086/80286 processors take a varying number of cycles per instruction. |
| `EMU2_CPM_DISK` | Block size of the fabricated CP/M-86 "disk" that presents each drive's host directory: `auto` (default), `1k`, `2k`, `4k`, `8k` or `16k`. The block size is the allocation granularity.  The smallest reportable file size (`1k` keeps small files exact; bigger blocks are needed for bigger disks). `auto` scans the directory and picks the smallest block size that holds it. The disk itself is sized to the directory's contents (see `EMU2_CPM_FREE`) and then capped at the guest-tool ceiling: about 8 MB for a standard CP/M 2.2 program (its 16-bit record count and allocation bitmap top out there), or 512 MB with `EMU2_CPM_PLUS`. Files too big to fit aren't listed, as on a real CP/M disk. Per-drive override: `EMU2_CPM_DISK_C`, `EMU2_CPM_DISK_D`, ... take precedence over the global setting. Only affects native CP/M-86 programs. |
| `EMU2_CPM_FREE` | Target percentage of virtual free space the `auto` disk sizing aims to leave (default `25`, clamped to 0..90). |
| `EMU2_CPM_PLUS` | When set, use CP/M 3 (Personal CP/M-86 / CP/M-86 Plus) limits: up to 2048 extents per file (32 MB files) and 512 MB disks, instead of the standard 512 extents (8 MB files, ~8 MB disks). Needs CP/M-3-aware tools to use the larger sizes. |
| `EMU2_CPM_VER` | CP/M version reported to native CP/M-86 programs by BDOS function 12, as a major dot minor (e.g. `3.1`, `2.2`) or a bare major (`3`). Defaults to `3.1`. CP/M 3.0 and later maintain the *Last-Record Byte Count* (LRBC) in the directory/FCB `S1` field, so programs see exact file lengths instead of sizes rounded up to the 128-byte record boundary. Older programs that expect 2.2 treat `S1` as reserved, so the byte-count metadata is only filled in when a 3.0+ version is reported; set this to `2.2` for programs that misbehave when told they are running under CP/M 3. Note: emu2 is a *single-user* CP/M-86 emulator. It does not emulate the multi-user / concurrent variants of CP/M (Concurrent CP/M-86, MP/M-86), so this only sets the reported single-user CP/M version.  There is no way (*yet*) to make a program believe it is running under *multi-user* Concurrent CP/M or M/PM. |
| `EMU2_CPM_NOTRUNC` | By default, when LRBC is active (CP/M 3.0+), emu2 trims a host file to its exact byte count on close, so output is no longer rounded up to a whole 128-byte record. Set this (to any value other than `0`/`off`/`no`/`false`) to keep files padded to the record boundary while still reporting the byte count via `S1`. Has no effect under CP/M 2.2, which has no LRBC and is never trimmed. |
| `EMU2_CPM_ISXLRBC` | Selects how the Last-Record Byte Count in `S1` is interpreted. There is no universally agreed meaning, so two conventions exist. By default emu2 follows Digital Research's DOS Plus / Personal CP/M-86: `S1` is the number of bytes *used* in the last record. Set this to use the ISX-style (ISIS-II emulator) convention instead, where `S1` is the number of *unused* bytes in the last record. In both conventions `S1` of `0` means the file fills its last record exactly. Has no effect under CP/M 2.2, which has no LRBC. |
| `EMU2_CPM_VT52` | Native CP/M-86 programs drive the console as a DOS-Plus (CP/M-86 4.1) terminal. emu2 interprets the console control sequences: VT52 cursor/erase codes, DRI colour codes (`ESC b`/`ESC c`), and ANSI/VT100 codes.  It applies them to the emulated PC screen, or, for programs that never touch the BIOS video and so talk straight to the host terminal, translates them to the ANSI your terminal understands. On by default; set to `0`/`off`/`no` to disable. No effect on DOS. |
| `EMU2_RAMDUMP` | Path of a file to write the full 1 MB guest RAM image on exit.  The file is written as a flat binary (offset `0` = physical address `0x00000`).  Useful for post-mortem inspection after a crash: load into a hex editor or disassembler and inspect the stack, BSS, and heap at the moment the program ended.  Has no effect on program behaviour; the dump is always written regardless of whether the program exits cleanly or crashes. |
| `EMU2_CPM_POISON` | Fill free CP/M-86 memory with byte specified before loading (for debugging). |
| `EMU2_CPM_DIRY` | Fill free CP/M-86 memory with `0xFF` before loading (for debugging). |
