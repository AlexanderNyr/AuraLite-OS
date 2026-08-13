# Console Win32 example.  See ../README.md for the toolchain.
#
# mingw-w64 is a HOST build tool here: it produces a .exe that AuraLite runs.
# Nothing from mingw-w64's runtime ships in AuraLite -- only the compiler
# output -- which is the licensing distinction recorded in w32/LICENSING.md.

CROSS ?= x86_64-w64-mingw32-
CC    := $(CROSS)gcc

# -nostdlib keeps mingw-w64's CRT out of the image: the personality provides
# the startup sequence itself (W32-6), and linking mingw's would mean
# shipping code under its licence rather than just its headers.
CFLAGS  := -O2 -Wall -Wextra -m64
LDFLAGS := -nostdlib -Wl,--entry=winstart
LIBS    := -lkernel32 -luser32

all: hello.exe

hello.exe: hello.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LIBS)

clean:
	rm -f hello.exe

.PHONY: all clean
