#!/usr/bin/env bash
# test_gui_theme.sh — RESIDUE2 T7: persisted user settings/theme.
#
# The dotfile convention (plan T7: "settings persistence rides the existing
# FS layout, no new daemon"): gtheme writes /disk/.aura-theme (fsync'd),
# and the desktop (glaunch) applies it at startup.  This case proves the
# whole chain ACROSS BOOT TWO of the same disk image:
#
#   boot 1: engine round-trip on /tmp (--selftest), then --save the accent
#           and read it back with --show (same-boot /disk round-trip);
#   boot 2: --show MUST return the saved accent (real persistence), and
#           glaunch MUST report loading the dotfile.
#
# The persistent drive is attached cache=none WITHOUT snapshot so guest
# writes reach the image file (the ISO stays snapshot-backed as always).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "GUI theme persistence (dotfile across reboot)"

DISK="$IL_BUILD/theme-disk.img"
il_make_disk "$DISK" 16 "AURALHCI"

# ---- boot 1: write the dotfile ---------------------------------------
LOG1="$IL_LOGDIR/gui_theme_b1.log"
IL_LAST_LOG="$LOG1"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run gtheme --selftest"
il_send_delay 2
il_send "run gtheme --save 0x00AA3311"
il_send_delay 3
il_send "run gtheme --show"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG1" 50 \
    -drive "file=$DISK,format=raw,if=none,id=themedisk,cache=none" \
    -device ahci,id=ahci1 \
    -device ide-hd,drive=themedisk,bus=ahci1.0

il_assert_grep "$LOG1" "GTHEME RTT PASS"    "engine round-trips every theme field"
il_assert_grep "$LOG1" "GTHEME SAVED 0xAA3311" "accent saved to the dotfile"
il_assert_grep "$LOG1" "GTHEME ACCENT 0xAA3311" "same-boot dotfile read-back"

# ---- boot 2: the dotfile must survive --------------------------------
LOG2="$IL_LOGDIR/gui_theme_b2.log"
IL_LAST_LOG="$LOG2"

IL_INPUT_QUEUE=""
il_send_delay 7
il_send "run gtheme --show"
il_send_delay 2
il_send "run glaunch"       # blocks in its event loop after the receipt line
il_send_delay 4

il_run_qemu "$LOG2" 50 \
    -drive "file=$DISK,format=raw,if=none,id=themedisk,cache=none" \
    -device ahci,id=ahci1 \
    -device ide-hd,drive=themedisk,bus=ahci1.0

il_assert_grep "$LOG2" "GTHEME ACCENT 0xAA3311" "accent persisted ACROSS the reboot"
il_assert_grep "$LOG2" "\[glaunch\] theme loaded from /disk/.aura-theme" \
    "desktop applies the dotfile at startup"

il_assert_no_grep "$LOG1" "GTHEME ERROR"  "no gtheme errors (boot 1)"
il_assert_no_grep "$LOG2" "GTHEME ERROR"  "no gtheme errors (boot 2)"
il_assert_no_grep "$LOG2" "PANIC"          "no kernel panic"

il_summary
