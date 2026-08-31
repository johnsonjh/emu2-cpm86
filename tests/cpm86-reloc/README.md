# CP/M-86 relocation regression tests

This directory contains oracles that verify `cpm86_load_cmd` in `src/cpm86.c`:
group allocation, P_LOAD fixup application, and entry-stack setup.  Pre-built
`.CMD` binaries are kept in git; C sources document what was built and allow
reproduction with Open Watcom.

## Oracle inventory

| File | Memory model | Prints | emu2-runnable | Source |
|------|-------------|--------|---------------|--------|
| `FARMULTI.CMD` | medium (far calls, 4 funcs × separate `_TEXT`) | `OK!` | yes | `farmulti.c` |
| `FARPTR.CMD`   | compact-like (far DATA pointer → CODE stubs)   | `OK!` | yes | `farptr.c` |
| `FARRUN.CMD`   | medium (far call, single smoke function)        | `A`   | yes | `farrun.c` |
| `SPLIT.CMD`    | 8080 single-group (data==0, entry 0x100)        | —     | yes (load-smoke) | `split.c` |
| `SMALL.CMD`    | small (near CODE+DATA, no P_LOAD fixups)        | `OK!` | yes | `small.c` |
| `LARGE.CMD`    | large (far code+data, far-pointer relocation)   | `OK!` | yes | `large.c` |

### Not run under emu2

| File | Reason | Source |
|------|--------|--------|
| `DRCFPTR.CMD` | Built by DR C 1.11 (LARGE, ~38 KB); requires DR C toolchain + MAME for verification | `drc_farptr.c` |
| `FPTRMAME.CMD` | MAME-instrumented (signals pass/fail via port 0x2FE); requires rc759 MAME | `farptr_mame.c` |

## Building oracles

Pre-built binaries cover all models tested by `run.sh`.  Rebuild from source
if you change the C files or want to verify bit-identity:

```sh
# native Open Watcom in PATH:
make

# via Docker (ghcr.io/open-watcom/open-watcom-v2):
make WATCOM_DOCKER=1
```

`SPLIT.CMD` has no Makefile target: the 8080-model wlink flags that produce a
genuine single-group layout (data descriptor == 0) are unclear; it is kept as
a prebuilt load-smoke only.

`DRCFPTR.CMD` and `FPTRMAME.CMD` are also prebuilts (not in the Makefile):
`DRCFPTR` requires the DR C 1.11 + LINK86 1.2 toolchain; `FPTRMAME` embeds
MAME-specific port signalling and is not useful outside an rc759 session.

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
