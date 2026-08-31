# Rebuild CP/M-86 relocation test oracles from C source using Open Watcom.
# Pre-built binaries are kept in git so Watcom is not required to run the tests.
#
# Memory models covered:
#   medium (-mm -zm): FARMULTI, FARPTR, FARRUN
#   small  (-ms -zm): SMALL
#   large  (-ml -zm): LARGE
#   8080 (single group): SPLIT -- prebuilt only; 8080-model flags unclear, no rebuild target
#
# Usage:
#   make              # rebuild if wcc86 + wlink are in PATH
#   make WATCOM_DOCKER=1  # use ghcr.io/open-watcom/open-watcom-v2 via Docker

CC86  = wcc86
WLINK = wlink

ifdef WATCOM_DOCKER
  _D = docker run --rm -v "$(CURDIR):/src" -w /src ghcr.io/open-watcom/open-watcom-v2
  CC86  = $(_D) wcc86
  WLINK = $(_D) wlink
endif

CF_MED   = -mm -zm -s -os -w4
CF_SMALL = -ms -zm -s -os -w4
CF_LARGE = -ml -zm -s -os -w4

.PHONY: all clean

all: FARMULTI.CMD FARPTR.CMD FARRUN.CMD SMALL.CMD LARGE.CMD

FARMULTI.obj: farmulti.c; $(CC86) $< $(CF_MED)
FARPTR.obj:   farptr.c;   $(CC86) $< $(CF_MED)
FARRUN.obj:   farrun.c;   $(CC86) $< $(CF_MED)
SMALL.obj:    small.c;    $(CC86) $< $(CF_SMALL)
LARGE.obj:    large.c;    $(CC86) $< $(CF_LARGE)

%.CMD: %.obj
	$(WLINK) format cpm86 name $@ option nodefaultlibs, map file $<

clean:
	rm -f *.obj *.map
