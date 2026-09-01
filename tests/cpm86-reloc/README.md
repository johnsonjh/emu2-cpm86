# CP/M-86 relocation regression tests

This directory contains oracles that verify `cpm86_load_cmd` in `src/cpm86.c`:
group allocation, P_LOAD fixup application, and entry-stack setup.  Pre-built
`.CMD` binaries are kept in git; C sources document what was built and allow
reproduction with Open Watcom.

Oracles are named for the **CP/M-86 memory model** they exercise, so the file
name tells you which load-time layout is under test.

## Oracle inventory

| File | Memory model | Prints | emu2-runnable | Source |
|------|-------------|--------|---------------|--------|
| `TINY.CMD`    | 8080 single-group (data==0, entry 0x100)         | —     | yes (load-smoke) | `tiny.c` |
| `SMALL.CMD`   | small (near CODE+DATA, no P_LOAD fixups)          | `OK!` | yes | `small.c` |
| `COMPACT.CMD` | compact (`__far` DATA pointer → CODE stubs)       | `OK!` | yes | `compact.c` |
| `MEDIUM.CMD`  | medium (far calls, 4 funcs × separate `_TEXT`)    | `OK!` | yes | `medium.c` |
| `MEDIUM2.CMD` | medium (minimal single far-call smoke)            | `A`   | yes | `medium2.c` |
| `large.c`     | large (far code+data, far-pointer relocation)     | `OK!` | **untested** | `large.c` |

`COMPACT.CMD` is built with `-mm -zm` but its data table uses explicit `__far`
pointers, so it exercises the far-data-pointer (compact-model) relocation path
regardless of the model flag.

`SMALL.CMD` uses a `main` entry and is linked against the real CP/M-86 C runtime
(`cstartcpm.obj` + `clibs.lib`): small-model codegen references the startup
symbol `_small_code_`, so it cannot use the bare `cpmmain` + `option
nodefaultlibs` style of the medium/compact oracles.  It has no P_LOAD fixups
(reloc bit 0), so it verifies the loader's non-relocating two-group path.

**`large.c` has no prebuilt `LARGE.CMD` and is not run** — Open Watcom ships no
large-model CP/M-86 runtime (`clibl.lib`, `_cstart_`, `_big_code_` are all
missing from the `cpm86` target), so a hosted large-model program cannot be
linked, and small-model-style clib-free linking fails on `_big_code_` too.  The
source is kept as documentation of the intended large-model far-pointer oracle.
Tracked in ravn/emu2-cpm86#16.

MAME-only and DR C oracle sources are not included here; they require
toolchains or infrastructure outside the scope of emu2.

## Building oracles

Pre-built binaries cover the models tested by `run.sh`.  Rebuild from source if
you change the C files or want to verify bit-identity:

```sh
# native Open Watcom in PATH:
make

# via Docker (ghcr.io/open-watcom/open-watcom-v2):
make WATCOM_DOCKER=1
```

`TINY.CMD` has no Makefile target: the 8080-model wlink flags that produce a
genuine single-group layout (data descriptor == 0) are unclear; it is kept as a
prebuilt load-smoke only.

## Running

```sh
make -C ../..          # build emu2 first
bash run.sh            # expected: all PASS + "ALL PASS"
```

## P_LOAD relocation format

Each 4-byte fixup record:
- `byte[0]`: `(loc_group << 4) | tgt_group`  (1=CODE 2=DATA 3=EXTRA 4=STACK)
- `bytes[1-2]`: paragraph offset within loc group (little-endian)
- `byte[3]`: byte offset within that paragraph
- Terminator: first all-zero record

The loader adds `tgt_group`'s load segment to the word at `loc_group:para:byte`.
`hdr[0x7F]` bit 7 signals table presence; `hdr[0x7D..0x7E]` is the file record
number (× 128 = byte offset) of the table.

Reimplementation derived from the CCP/M-86 3.1 source; validated with Digital
Research C v1.1 / LINK86 v1.2.
