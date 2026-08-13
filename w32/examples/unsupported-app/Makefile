# A deliberately unsupported Win32 program.  See ../README.md.
#
# It builds fine -- mingw-w64 has no idea AuraLite exists -- and is REFUSED
# at load time with the offending import named.
CROSS ?= x86_64-w64-mingw32-
CC    := $(CROSS)gcc

CFLAGS  := -O2 -Wall -Wextra -m64
LDFLAGS := -nostdlib -Wl,--entry=winstart
LIBS    := -lkernel32 -ladvapi32

all: registry.exe

registry.exe: registry.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LIBS)

clean:
	rm -f registry.exe

.PHONY: all clean
