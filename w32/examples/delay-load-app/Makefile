# Delay/runtime-load Win32 example (W32A-18).  See ../README.md.
#
# USER32 is resolved at run time, so it is NOT on the link line: only
# KERNEL32 (LoadLibrary/GetProcAddress/WriteFile) is linked.
CROSS ?= x86_64-w64-mingw32-
CC    := $(CROSS)gcc

CFLAGS  := -O2 -Wall -Wextra -m64
LDFLAGS := -nostdlib -Wl,--entry=winstart
LIBS    := -lkernel32

all: delayload.exe

delayload.exe: delayload.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LIBS)

clean:
	rm -f delayload.exe

.PHONY: all clean
