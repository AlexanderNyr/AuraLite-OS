#!/usr/bin/env bash
# test_w32_pe_loader.sh — WIN32_PLAN.md phase W32-3 gate: the kernel PE loader.
#
# Proves, in a booted system, that:
#   (a) a genuine PE32+ .exe loads, runs Ring 3 code and exits with its own
#       status -- the whole point of the phase;
#   (b) a byte-identical image whose only difference is an EFI subsystem is
#       REFUSED, which is what stops AuraLite's own BOOTX64.EFI from ever
#       being launched as a process;
#   (c) neither path faults the kernel.
#
# Both fixtures are built in-tree (nasm -f win64 + lld-link), so this test
# depends on no downloaded binary.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32-3 kernel PE32+ loader"

LOG="$IL_LOGDIR/w32_pe_loader.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 10
il_send "run /tests/petest.exe"
il_send_delay 3
il_send "run /tests/petest_efi.exe"
il_send_delay 3
il_send "run /tests/petest_reloc.exe"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 70

# --- (a) the loader works -------------------------------------------------
il_assert_grep "$LOG" "\[pe\] +loaded 4 section\(s\)" \
    "PE image mapped: all four sections"
il_assert_grep "$LOG" "W32-PE-LOADER-OK" \
    "Ring 3 code from a PE image ran and wrote to stdout"
il_assert_grep "$LOG" "'/tests/petest\.exe' \(tid [0-9]+\) exited \(code=77\)" \
    "PE process exited with its own status (77)"

# The marker is written through a pointer that lives in .data and needs the
# image's own base to be correct, so printing it also proves the data section
# was mapped and populated, not merely the text.
il_assert_grep "$LOG" "entry 0x140001000" \
    "entry point resolved against the image base"

# --- (b) the EFI refusal --------------------------------------------------
il_assert_grep "$LOG" "\[pe\] +refused: unsupported \(subsystem=10\)" \
    "EFI-subsystem image refused by the loader"
il_assert_no_grep "$LOG" "petest_efi.*code=77" \
    "the refused image did not run"

# --- (b2) the relocation path ---------------------------------------------
# petest_reloc.exe is the same program linked at 0x800000000000, which is at
# USER_VADDR_TOP and therefore can never be honoured.  The loader must move it
# and fix up its .data pointer; printing the identical marker is the proof,
# because the marker is reached *through* that pointer.
il_assert_grep "$LOG" "\[pe\] +relocated [0-9]+ entries" \
    "base relocations applied"
il_assert_grep "$LOG" "loaded 4 section\(s\) at 0x180000000.*\(relocated\)" \
    "relocated image loaded at the fallback base"
il_assert_count "$LOG" "W32-PE-LOADER-OK" 2 \
    "relocated image produced the identical output"
il_assert_grep "$LOG" "'/tests/petest_reloc\.exe' \(tid [0-9]+\) exited \(code=77\)" \
    "relocated PE process exited with the same status"

# --- (c) nothing broke ----------------------------------------------------
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION|kernel panic|Page Fault" \
    "no kernel fault on either path"
il_assert_grep "$LOG" "Goodbye!" \
    "shell survived both and exited cleanly"

il_summary
