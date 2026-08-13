# GUI Win32 example.  See ../README.md.
CROSS ?= x86_64-w64-mingw32-
CC    := $(CROSS)gcc

CFLAGS  := -O2 -Wall -Wextra -m64
LDFLAGS := -nostdlib -Wl,--entry=winstart -Wl,--subsystem,windows
LIBS    := -lkernel32 -luser32 -lgdi32

all: window.exe

window.exe: window.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LIBS)

clean:
	rm -f window.exe

.PHONY: all clean
