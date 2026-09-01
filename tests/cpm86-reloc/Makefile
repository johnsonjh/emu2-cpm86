# Rebuild CP/M-86 relocation test oracles from C source using Open Watcom.
# Pre-built binaries are kept in git so Watcom is not required to run the tests.
#
# Oracles are named after the CP/M-86 memory model they exercise:
#   TINY    (8080 single group):        TINY -- prebuilt only; single-group
#                                       flags unclear, no rebuild target
#   SMALL   (-mcmodel=s, near/near):    SMALL -- linked with the real CP/M-86 C
#                                       runtime (small-model codegen references
#                                       the startup symbol _small_code_), so it
#                                       uses a `main` entry, not `cpmmain`
#   COMPACT (-mm -zm + __far data ptr): COMPACT  (far-data-pointer relocation)
#   MEDIUM  (-mm -zm, far code):        MEDIUM, MEDIUM2
#   LARGE   (-ml, far code+data):       LARGE -- NOT BUILDABLE here: Open Watcom
#                                       ships no large-model CP/M-86 runtime
#                                       (clibl.lib / _cstart_ / _big_code_
#                                       missing). Tracked untested; see README.
#
# Two link styles:
#   clib-free (cpmmain + wlink option nodefaultlibs): MEDIUM/MEDIUM2/COMPACT
#   hosted    (main + owcc gcc-like driver):          SMALL
#
# Usage:
#   make              # rebuild if the Open Watcom tools are in PATH
#   make WATCOM_DOCKER=1  # use ghcr.io/open-watcom/open-watcom-v2 via Docker

CC86  = wcc
WLINK = wlink
OWCC  = owcc

ifdef WATCOM_DOCKER
  _D = docker run --rm -v "$(CURDIR):/src" -w /src ghcr.io/open-watcom/open-watcom-v2
  CC86  = $(_D) wcc
  WLINK = $(_D) wlink
  OWCC  = $(_D) owcc
endif

CF_MED = -mm -zm -s -os -w4

.PHONY: all clean

# LARGE.CMD is intentionally absent (no large-model CP/M-86 runtime available).
all: MEDIUM.CMD MEDIUM2.CMD COMPACT.CMD SMALL.CMD

MEDIUM.o:  medium.c;  $(CC86) $< $(CF_MED)
MEDIUM2.o: medium2.c; $(CC86) $< $(CF_MED)
COMPACT.o: compact.c; $(CC86) $< $(CF_MED)

MEDIUM.CMD MEDIUM2.CMD COMPACT.CMD: %.CMD: %.o
	$(WLINK) format cpm86 name $@ option nodefaultlibs, map file $<

SMALL.CMD: small.c
	$(OWCC) -bcpm86 -mcmodel=s -fno-stack-check -o $@ $<

clean:
	rm -f *.o *.obj *.map
