#!/usr/bin/env bash
# test_usb_msc_hotplug.sh — runtime USB mass-storage attach/detach via QEMU HMP.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "USB runtime MSC hotplug attach/detach (xHCI storage)"

USB="$IL_BUILD/usb-hotplug-msc.img"
il_make_disk "$USB" 8 "HOTPLUG!"

LOG="$IL_LOGDIR/usb_msc_hotplug.log"
MON="$IL_LOGDIR/usb_msc_hotplug.hmp.sock"
rm -f "$LOG" "$MON"
IL_LAST_LOG="$LOG"
trap 'rm -f "$MON"; il_dump_on_error' EXIT

set +e
timeout --foreground 55 "$IL_QEMU" \
    -cdrom "$IL_ISO" \
    -m 512M \
    -smp 2 \
    -display none \
    -serial "file:$LOG" \
    -monitor "unix:$MON,server,nowait" \
    -no-reboot \
    -cpu qemu64 \
    -boot order=d \
    -netdev user,id=net0 \
    -device e1000,netdev=net0 \
    -device qemu-xhci,id=xhci \
    -drive "file=$USB,format=raw,if=none,id=hotstick" &
QPID=$!
set -e

python3 - "$LOG" "$MON" <<'PY'
import pathlib, socket, sys, time
log = pathlib.Path(sys.argv[1])
mon = pathlib.Path(sys.argv[2])

def wait_for(pattern, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        txt = log.read_text(errors='ignore') if log.exists() else ''
        if pattern in txt:
            return True
        time.sleep(0.25)
    return False

for _ in range(150):
    if mon.exists(): break
    time.sleep(0.1)
if not mon.exists(): raise SystemExit('monitor socket did not appear')
if not wait_for('hotplug monitor started', 35):
    raise SystemExit('AuraLite hotplug monitor did not start')

def hmp(cmd):
    s = socket.socket(socket.AF_UNIX)
    s.connect(str(mon))
    time.sleep(0.1)
    s.sendall(cmd.encode('ascii') + b'\n')
    time.sleep(0.2)
    s.close()

hmp('device_add usb-storage,bus=xhci.0,port=1,drive=hotstick,id=hotmsc')
if not wait_for('USB mass storage ready (hotplug)', 30):
    raise SystemExit('hotplug MSC did not become ready')
hmp('device_del hotmsc')
if not wait_for('hotplug: detached mass storage', 15):
    raise SystemExit('hotplug MSC did not detach')
PY

set +e
wait "$QPID"
set -e

il_assert_grep "$LOG" "\[usb\] hotplug monitor started" "hotplug monitor started"
il_assert_grep "$LOG" "\[usb\] hotplug: new root device" "new storage device detected after boot"
il_assert_grep "$LOG" "\[msc\] hotplug: probing mass storage" "MSC hotplug probe started"
il_assert_grep "$LOG" "\[msc\] PASS: USB mass storage ready \(hotplug\)" "MSC hotplug ready"
il_assert_grep "$LOG" "\[msc\] PASS: USB mass storage READ\(10\) works" "MSC hotplug READ(10) works"
il_assert_grep "$LOG" "\[usbfs\] device available at /usb" "usbfs hotplug view updated"
il_assert_grep "$LOG" "\[msc\] hotplug: detached mass storage" "MSC hotplug detach detected"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|hotplug: root enumeration failed|SET_ADDRESS failed|READ\(10\) sector 0 failed" \
    "no MSC hotplug faults"

il_summary
