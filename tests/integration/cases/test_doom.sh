#!/usr/bin/env bash
# test_doom.sh — the DOOM port, end to end.  DOOM_PLAN.md D5.
#
# Boots AuraLite with the WAD disk attached and asserts that the engine
# initialises all the way through to graphics.  This is the gate that says
# "DOOM still runs", and it is the only test that exercises the whole stack at
# once: libc's stdio and printf, the FAT32 read path, the AHCI driver, the ELF
# loader and the compositor.
#
# It SKIPS LOUDLY, and exits 0, when its inputs are absent.  The engine is
# GPL-2.0 and is not vendored (DOOM_PLAN.md D1), and the Freedoom IWAD is a
# 22 MB download (D5) -- so on a clean checkout with no network neither exists,
# and that is a legitimate state, not a failure.  What is NOT legitimate is a
# test that quietly reports success without having run: every skip below says
# exactly what was missing and what to run to fix it.
#
# UEFI is required, not preferred.  AuraLite only gets a linear framebuffer
# from the UEFI GOP path (boot/uefi/efi_main.c); on a BIOS boot there is no
# framebuffer at all, DG_Init has nothing to draw into, and the failure would
# look like a DOOM bug rather than a missing display.  Hence the OVMF check.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init

il_section "DOOM (doomgeneric + Freedoom) on AuraLite"

DOOM_DIR="$IL_BUILD/doom"
DOOM_DISK="$DOOM_DIR/doomdisk.img"
LOG="$IL_LOGDIR/doom.log"
IL_LAST_LOG="$LOG"

# ---- the skips, each naming its own remedy --------------------------------

if [ ! -f "$DOOM_DISK" ]; then
    echo "${C_YELLOW}SKIP: no WAD disk at $DOOM_DISK${C_RESET}"
    echo "      DOOM is GPL-2.0 and is not vendored; the IWAD is a 22 MB download."
    echo "      Build both with:  make run-doom   (or: make $DOOM_DISK)"
    exit 0
fi

OVMF_CODE=""
for c in /usr/share/OVMF/OVMF_CODE_4M.fd \
         /usr/share/OVMF/OVMF_CODE.fd \
         /usr/share/ovmf/OVMF_CODE.fd \
         /usr/share/edk2/x64/OVMF_CODE.4m.fd; do
    [ -f "$c" ] && { OVMF_CODE="$c"; break; }
done

OVMF_VARS=""
for v in /usr/share/OVMF/OVMF_VARS_4M.fd \
         /usr/share/OVMF/OVMF_VARS.fd \
         /usr/share/ovmf/OVMF_VARS.fd \
         /usr/share/edk2/x64/OVMF_VARS.4m.fd; do
    [ -f "$v" ] && { OVMF_VARS="$v"; break; }
done

if [ -z "$OVMF_CODE" ] || [ -z "$OVMF_VARS" ]; then
    echo "${C_YELLOW}SKIP: OVMF not found${C_RESET}"
    echo "      DOOM needs a framebuffer, and AuraLite only gets one from the"
    echo "      UEFI GOP path -- a BIOS boot has no framebuffer to draw into."
    echo "      Install it with:  sudo apt-get install ovmf"
    exit 0
fi

il_have qemu-system-x86_64

trap il_dump_on_error EXIT

# OVMF needs a WRITABLE copy of the variable store.
VARS="$IL_BUILD/doom_ovmf_vars.fd"
cp "$OVMF_VARS" "$VARS"

# ---- run -------------------------------------------------------------------
#
# The timings are generous on purpose.  Loading a 28 MB IWAD through the FAT32
# read path takes ~40 s at the ~740 KB/s the driver sustains, and OVMF itself
# takes appreciably longer to reach the kernel than SeaBIOS does.  A tight
# budget here would produce a test that fails on slow CI runners for reasons
# that have nothing to do with DOOM.

il_send_delay 25
il_send "run /fat/doom/doom"
il_send_delay 240

il_run_qemu "$LOG" 320 \
    -drive "if=pflash,format=raw,unit=0,readonly=on,file=$OVMF_CODE" \
    -drive "if=pflash,format=raw,unit=1,file=$VARS" \
    -drive "id=wad,file=$DOOM_DISK,format=raw,if=none,snapshot=on" \
    -device ahci,id=ahci \
    -device ide-hd,drive=wad,bus=ahci.0 \
    -vga std

# ---- assertions ------------------------------------------------------------

# The UEFI framebuffer, without which none of the rest can work.
il_assert_grep_fixed "$LOG" "[BL6] GOP framebuffer located" "UEFI GOP framebuffer located"

# The compositor window, created by AuraLite's own platform layer (D3).
il_assert_grep_fixed "$LOG" "DOOM-WINDOW-CREATED 640x400" "DOOM window created"

# The WAD was found, opened and read.  This is the assertion that covers the
# AHCI leak and the FAT32 chain walk: the engine cannot get past W_Init
# without reading all 28 MB correctly.
il_assert_grep_fixed "$LOG" "adding /fat/doom/freedoom1.wad" "IWAD located on the WAD disk"
il_assert_grep_fixed "$LOG" "Freedoom: Phase 1"              "IWAD identified as Freedoom Phase 1"

# Engine initialisation, in order.  Each of these was, at some point in the
# port, the exact line the engine died on.
il_assert_grep_fixed "$LOG" "Z_Init: Init zone memory allocation daemon" "zone memory initialised"
il_assert_grep_fixed "$LOG" "M_LoadDefaults: Load system defaults"       "config loaded (needs %i)"
il_assert_grep_fixed "$LOG" "W_Init: Init WADfiles"                      "WAD subsystem initialised"
il_assert_grep_fixed "$LOG" "R_Init: Init DOOM refresh daemon"           "renderer initialised"
il_assert_grep_fixed "$LOG" "P_Init: Init Playloop state"                "playloop initialised"
il_assert_grep_fixed "$LOG" "HU_Init: Setting up heads up display"       "HUD initialised (needs %.3d)"
il_assert_grep_fixed "$LOG" "ST_Init: Init status bar"                   "status bar initialised"

# Graphics, which is the point of the whole exercise.
il_assert_grep_fixed "$LOG" "I_InitGraphics: DOOM screen size: w x h: 320 x 200" \
    "graphics initialised at 320x200"

# Failures seen during the port, asserted by name so a regression reports the
# cause rather than just an absent marker.
il_assert_no_grep_fixed "$LOG" "W_GetNumForName: STCFN%.3 not found!" \
    "integer precision works in printf"
il_assert_no_grep_fixed "$LOG" "Unknown configuration variable: 'joystick_physical_button%i'" \
    "%i works in printf"
il_assert_no_grep_fixed "$LOG" "[elf]  segment file range out of bounds" \
    "the ELF was read in full"
il_assert_no_grep_fixed "$LOG" "[proc] ELF load failed" "the binary loaded"

il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no exception"
il_assert_no_grep "$LOG" "PANIC"               "no panic"

rm -f "$VARS"

il_summary
