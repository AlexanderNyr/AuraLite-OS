#!/usr/bin/env bash
# tests/integration/lib/gui_lane.sh — WR-0 (W32RUN_PLAN.md) the live-GUI lane.
#
# Sourced AFTER lib/lib.sh + il_init.  It boots AuraLite under OVMF — the only
# firmware with a linear framebuffer (BIOS Stage 2 sets no VBE mode; the GUI
# composes off-screen and the frame is black — fb.c's own comment, and the
# skip precedent in test_gui_dirty_uefi.sh) — with two UNIX sockets: a serial
# socket (the shell) and a monitor socket (HMP: screendump, sendkey, mouse).
# Every live-frame gate (WR-2..WR-4) drives the app through these.
#
# The three lessons WR-0 bring-up paid for, encoded here so no gate repeats
# them:
#   * OVMF, not SeaBIOS — BIOS has no framebuffer;
#   * the ISO is a hybrid MBR disk, attach if=ide + `-boot order=c`, NOT
#     media=cdrom (that drops OVMF to the UEFI Shell with no FS mapping);
#   * the monitor screendumps the GOP framebuffer even under -display none.
#
# API (all after gl_boot):
#   gl_require_ovmf                     — locate OVMF; loud-SKIP + exit 0 if absent
#   gl_make_fat_disk <img> <file...>   — build a /fat delivery disk (app binaries)
#   gl_boot [extra qemu args...]       — background QEMU under OVMF; wait for GOP
#                                        + `auralite#`.  Honours GL_APP_DISK,
#                                        GL_SELFTEST (default full), GL_TIMEOUT.
#   gl_send <cmd>                      — send a shell command, drain briefly
#   gl_cmd  <cmd> <wait-regex> [to]    — send + wait for regex (one connection)
#   gl_run_w32 <exe> [args...]         — `run w32run <exe> [args]` (args survive
#                                        the shell's w32run auto-route, which
#                                        drops extra argv — init.c)
#   gl_wait <regex> [timeout]          — wait for regex on serial
#   gl_shot <name>                     — screendump -> $GL_SHOTDIR/<name>.ppm
#                                        + .png; sets GL_LAST_PPM / GL_LAST_PNG
#   gl_key/gl_type via HMP; gl_move/gl_click/gl_dblclick via QMP (abs pointer)
#     -- gui_input.py auto-falls back to the HMP PS/2 relative path on
#        QEMU >= 9, where QMP input-send-event aborts the emulator
#   gl_stop                            — kill the VM (idempotent; on trap)
#
# GL_LOG accumulates the serial transcript for il_assert_grep.

GL_TOOLS="${GL_TOOLS:-$IL_ROOT/tools}"
GL_CONSOLE="$GL_TOOLS/gui_input.py"
GL_ORACLE="$GL_TOOLS/fb_oracle.py"

gl_require_ovmf() {
    GL_OVMF_CODE="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
    GL_OVMF_VARS="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
    if [ ! -f "$GL_OVMF_CODE" ] || [ ! -f "$GL_OVMF_VARS" ]; then
        echo "  [gui-lane] SKIP: OVMF firmware not found at $GL_OVMF_CODE"
        echo "             set OVMF_CODE=/path/to/OVMF_CODE.fd to override."
        exit 0
    fi
    il_have qemu-system-x86_64 python3
}

# gl_make_fat_disk <img> <file...> — a FAT disk the guest mounts at /fat, with
# 64 sectors of MBR slack up front so both the AHCI probe and mtools agree.
gl_make_fat_disk() {
    il_have mformat mcopy
    local img="$1"; shift
    local part="$img.part"
    rm -f "$img" "$part"
    dd if=/dev/zero of="$part" bs=1M count=64 status=none
    mformat -i "$part" -F -h 32 -s 32 -t 128 ::
    local f
    for f in "$@"; do
        mcopy -i "$part" "$f" "::/$(basename "$f")"
    done
    dd if=/dev/zero of="$img" bs=512 count=64 status=none
    cat "$part" >> "$img"
    rm -f "$part"
    echo "  [gui-lane] built /fat delivery disk $img ($# file(s))"
}

gl_boot() {
    local selftest="${GL_SELFTEST:-full}"
    GL_TIMEOUT="${GL_TIMEOUT:-100}"
    GL_SHOTDIR="${GL_SHOTDIR:-$IL_LOGDIR/gui-lane-shots}"
    mkdir -p "$GL_SHOTDIR"
    GL_LOG="${GL_LOG:-$IL_LOGDIR/gui_lane.log}"
    : > "$GL_LOG"
    IL_LAST_LOG="$GL_LOG"

    GL_SER="$IL_LOGDIR/gui_lane_ser.$$.sock"
    GL_MON="$IL_LOGDIR/gui_lane_mon.$$.sock"
    # QMP alongside HMP.  HMP's mouse_move/mouse_button queue RELATIVE motion
    # (qemu_input_queue_rel); usb-tablet is an ABSOLUTE device, so those
    # commands are accepted silently and move nothing -- measured: the guest
    # logged 0 HID reports over 600 polls while `info mice` showed the tablet
    # as the current mouse.  input-send-event with abs axes is the supported
    # path for an absolute pointer and it is QMP-only.
    GL_QMP="$IL_LOGDIR/gui_lane_qmp.$$.sock"
    rm -f "$GL_SER" "$GL_MON" "$GL_QMP"
    GL_VARS="$IL_LOGDIR/gui_lane_vars.$$.fd"
    cp "$GL_OVMF_VARS" "$GL_VARS"

    local app_args=()
    if [ -n "${GL_APP_DISK:-}" ] && [ -f "${GL_APP_DISK:-}" ]; then
        app_args=(
            -drive "id=dapp,file=$GL_APP_DISK,format=raw,if=none,snapshot=on"
            -device ahci,id=ahci -device ide-hd,drive=dapp,bus=ahci.0
        )
    fi

    "$IL_QEMU" \
        -drive "if=pflash,format=raw,readonly=on,file=$GL_OVMF_CODE" \
        -drive "if=pflash,format=raw,file=$GL_VARS" \
        -drive "file=$IL_ISO,format=raw,if=ide,snapshot=on" \
        "${app_args[@]}" \
        -m 512M -smp "${IL_SMP:-2}" -cpu "${IL_CPU:-qemu64}" \
        -no-reboot -no-shutdown -boot order=c \
        -netdev user,id=net0 -device e1000,netdev=net0 \
        -device usb-ehci,id=ehci -device usb-tablet,bus=ehci.0,id=gltablet \
        -fw_cfg "name=opt/auralite.selftest,string=$selftest" \
        -vga std -display none \
        -serial "unix:$GL_SER,server,nowait" \
        -monitor "unix:$GL_MON,server,nowait" \
        -qmp "unix:$GL_QMP,server,nowait" \
        >/dev/null 2>&1 &
    GL_QEMU_PID=$!

    # Wait for the sockets QEMU creates, then for the boot to reach the shell.
    local i
    for i in $(seq 1 50); do
        [ -S "$GL_SER" ] && [ -S "$GL_MON" ] && [ -S "$GL_QMP" ] && break
        kill -0 "$GL_QEMU_PID" 2>/dev/null || { echo "  [gui-lane] QEMU died on launch"; return 1; }
        sleep 0.2
    done

    echo "  [gui-lane] OVMF booting (TCG; up to ${GL_TIMEOUT}s to the shell)..."
    python3 "$GL_CONSOLE" serial-wait --serial "$GL_SER" \
        --pattern 'auralite#' --timeout "$GL_TIMEOUT" --log "$GL_LOG" || {
            echo "  [gui-lane] never reached the shell (see $GL_LOG)"; return 1; }
    return 0
}

gl_send() {
    python3 "$GL_CONSOLE" serial-run --serial "$GL_SER" \
        --cmd "$1" --secs "${2:-2}" --log "$GL_LOG"
}

# gl_cmd <cmd> <wait-regex> [timeout]
gl_cmd() {
    python3 "$GL_CONSOLE" serial-run --serial "$GL_SER" \
        --cmd "$1" --wait "$2" --timeout "${3:-30}" --log "$GL_LOG"
}

gl_run_w32() {
    local exe="$1"; shift
    gl_send "run w32run $exe $*" "${GL_RUN_SECS:-8}"
}

gl_wait() {
    python3 "$GL_CONSOLE" serial-wait --serial "$GL_SER" \
        --pattern "$1" --timeout "${2:-30}" --log "$GL_LOG"
}

gl_shot() {
    local name="$1"
    GL_LAST_PPM="$GL_SHOTDIR/$name.ppm"
    GL_LAST_PNG="$GL_SHOTDIR/$name.png"
    python3 "$GL_CONSOLE" shot --monitor "$GL_MON" "$GL_LAST_PPM" >/dev/null
    python3 - "$GL_LAST_PPM" "$GL_LAST_PNG" <<'PY'
import sys
from PIL import Image
Image.open(sys.argv[1]).convert("RGB").save(sys.argv[2])
PY
    echo "  [gui-lane] shot $name -> $GL_LAST_PNG"
}

gl_key()  { python3 "$GL_CONSOLE" key  --monitor "$GL_MON" "$@" >/dev/null; }
gl_type() { python3 "$GL_CONSOLE" type --monitor "$GL_MON" "$1" >/dev/null; }
# The pointer goes over QMP (absolute axes); HMP cannot move a usb-tablet.
gl_click()   { python3 "$GL_CONSOLE" click    --qmp "$GL_QMP" --monitor "$GL_MON" "$1" "$2" >/dev/null; }
gl_dblclick(){ python3 "$GL_CONSOLE" dblclick --qmp "$GL_QMP" --monitor "$GL_MON" "$1" "$2" >/dev/null; }
gl_move()    { python3 "$GL_CONSOLE" move     --qmp "$GL_QMP" --monitor "$GL_MON" "$1" "$2" >/dev/null; }

# gl_oracle <fb_oracle args...> — thin passthrough so cases read declaratively.
gl_oracle() { python3 "$GL_ORACLE" "$@"; }

gl_stop() {
    [ -n "${GL_QEMU_PID:-}" ] || return 0
    kill "$GL_QEMU_PID" 2>/dev/null || true
    wait "$GL_QEMU_PID" 2>/dev/null || true
    rm -f "$GL_SER" "$GL_MON" "$GL_QMP" "$GL_VARS" 2>/dev/null || true
    GL_QEMU_PID=""
}
