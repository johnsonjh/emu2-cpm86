# Rebuild CP/M-86 relocation test oracles from C source using Open Watcom.
# Pre-built binaries are kept in git so Watcom is not required to run the tests.
#
# Oracles are named after the CP/M-86 memory model they exercise:
#   TINY    (8080 single group):        TINY -- prebuilt only; single-group
#                                       flags unclear, no rebuild target
#   SMALL   (-ms -zm, near code+data):  SMALL
#   COMPACT (-mm -zm + __far data ptr): COMPACT  (far-data-pointer relocation)
#   MEDIUM  (-mm -zm, far code):        MEDIUM, MEDIUM2
#   LARGE   (-ml -zm, far code+data):   LARGE
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

all: MEDIUM.CMD MEDIUM2.CMD COMPACT.CMD SMALL.CMD LARGE.CMD

MEDIUM.obj:  medium.c;  $(CC86) $< $(CF_MED)
MEDIUM2.obj: medium2.c; $(CC86) $< $(CF_MED)
COMPACT.obj: compact.c; $(CC86) $< $(CF_MED)
SMALL.obj:   small.c;   $(CC86) $< $(CF_SMALL)
LARGE.obj:   large.c;   $(CC86) $< $(CF_LARGE)

%.CMD: %.obj
	$(WLINK) format cpm86 name $@ option nodefaultlibs, map file $<

clean:
	rm -f *.obj *.map
