# Dialog Win32 example (W32A-18).  See ../README.md for the toolchain.
#
# mingw-w64 is a HOST build tool: it produces a .exe AuraLite runs.  None of
# mingw-w64's runtime ships (see -nostdlib); only its headers are used.
CROSS ?= x86_64-w64-mingw32-
CC    := $(CROSS)gcc

CFLAGS  := -O2 -Wall -Wextra -m64
LDFLAGS := -nostdlib -Wl,--entry=winstart
LIBS    := -lkernel32 -luser32 -lcomdlg32

all: dialog.exe

dialog.exe: dialog.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LIBS)

clean:
	rm -f dialog.exe

.PHONY: all clean
