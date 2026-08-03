# Makefile for a console AuraLite OS application.
#
# The only thing an application needs to know is where the SDK is:
#
#     make AURALITE_SDK=/path/to/auralite/build/sdk
#
# Everything else — compiler, flags, linker script, libraries — comes from
# auralite.mk, so this application is built exactly the way the programs
# shipped with the OS are.

AURALITE_SDK ?= ../../build/sdk
include $(AURALITE_SDK)/auralite.mk

APP := hello-app.elf

all: $(APP)

$(APP): hello-app.o
	$(AURALITE_LD) $(AURALITE_LDFLAGS) $< $(AURALITE_LIBS) -o $@

hello-app.o: hello-app.c
	$(AURALITE_CC) $(AURALITE_CFLAGS) -c $< -o $@

clean:
	rm -f *.o *.elf

.PHONY: all clean
