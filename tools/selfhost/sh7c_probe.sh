# sh7c_probe.sh -- SELFHOST SH7c happy-path gate script.
#
# Staged at /tests/sh7c_probe.sh and run in-guest as
#     sh /tests/sh7c_probe.sh
#
# SH7c stages the boot-offset generator in-guest and proves the guest computes
# the same boot_info_t layout the host does (offsetof over the shared
# boot/shared/boot_info.h).  /bin/bootoffsets emits the header/inc and verifies
# the layout in --check.  This script (no grep/cut in the scripting shell, so
# every branch is on a process exit status):
#   S1  --check runs and reports the layout (exit 0 => tool computes offsets)
#   S2  --c regenerates boot_offsets.h (exit 0 => C header form produces)
#   S3  --asm regenerates boot_offsets.inc (exit 0 => NASM form produces)
#   S4  a bad argument is rejected (non-zero), so the S1 success is meaningful
#
# Byte-identity of the guest offsets vs the host generator is pinned at dev
# speed by tests/unit/test_bootoffsets_twin.c (compiled with the real host
# generated header); this probe proves the stripped ELF runs on AuraLite.

# S1: the in-guest verifier computes and reports the layout.
if bootoffsets --check
then
  echo [selfhost] sh7c: s1-layout-verified
else
  echo [selfhost] sh7c: UNREACHABLE-CHECK-FAILED
fi

# S2: the C header form regenerates in-guest (exit status only).
if bootoffsets --c > /tmp/sh7c_boot_offsets.h
then
  echo [selfhost] sh7c: s2-c-header-generated
else
  echo [selfhost] sh7c: UNREACHABLE-NO-C-HEADER
fi

# S3: the NASM %define form regenerates in-guest.
if bootoffsets --asm > /tmp/sh7c_boot_offsets.inc
then
  echo [selfhost] sh7c: s3-asm-inc-generated
else
  echo [selfhost] sh7c: UNREACHABLE-NO-ASM
fi

# S4 (negative control): an unknown mode must fail, not exit 0.
if bootoffsets --no-such-mode
then
  echo [selfhost] sh7c: UNREACHABLE-FALSE-USAGE
else
  echo [selfhost] sh7c: s4-bad-usage-rejected
fi

echo [selfhost] boot-offset header PASS: generated in-guest
