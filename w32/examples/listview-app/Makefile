# List-view Win32 example (W32A-18).  See ../README.md for the toolchain.
CROSS ?= x86_64-w64-mingw32-
CC    := $(CROSS)gcc

CFLAGS  := -O2 -Wall -Wextra -m64
LDFLAGS := -nostdlib -Wl,--entry=winstart
LIBS    := -lkernel32 -luser32 -lgdi32 -lcomctl32

all: listview.exe

listview.exe: listview.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LIBS)

clean:
	rm -f listview.exe

.PHONY: all clean
