# Rebuild FARMULTI.CMD and FARPTR.CMD from C source using Open Watcom.
# Pre-built binaries are kept in git so Watcom is not required to run the tests.
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

CFLAGS86 = -mm -zm -s -os -w4

.PHONY: all clean

all: FARMULTI.CMD FARPTR.CMD

%.obj: %.c
	$(CC86) $< $(CFLAGS86)

FARMULTI.CMD: farmulti.obj
	$(WLINK) format cpm86 name $@ option nodefaultlibs, map file $<

FARPTR.CMD: farptr.obj
	$(WLINK) format cpm86 name $@ option nodefaultlibs, map file $<

clean:
	rm -f *.obj *.map
