#!/usr/bin/env bash
# lab_wr2.sh — интерактивный стенд WR-2: OVMF + 7zFM.exe с тремя сокетами.
# Использование:
#   lab_wr2.sh boot     — собрать /fat-диск (с SUBDIR) и загрузить QEMU, дождаться shell
#   lab_wr2.sh launch   — запустить 7zFM.exe C:/fat
#   lab_wr2.sh verify   — гейт WR-2(б): полный цикл extract + md5-сверка с хостом
# Остальное — напрямую через tools/gui_input.py.
set -u
ROOT=/home/user/AuraLite-OS
LAB=/tmp/wr2lab
SER=$LAB/ser.sock
MON=$LAB/mon.sock
QMP=$LAB/qmp.sock
LOG=$LAB/serial.log
GI="python3 $ROOT/tools/gui_input.py"

mkdir -p $LAB

case "${1:-}" in
boot)
    rm -f $SER $MON $QMP $LOG
    # /fat-диск: бинарники + fixture + SUBDIR с файлом внутри
    IMG=$LAB/fat.img
    PART=$LAB/fat.part
    rm -f $IMG $PART
    dd if=/dev/zero of=$PART bs=1M count=64 status=none
    mformat -i $PART -F -h 32 -s 32 -t 128 ::
    mcopy -i $PART ~/appbins/7zFM.exe  ::/7zFM.exe
    mcopy -i $PART ~/appbins/7z.dll    ::/7z.dll
    if [ ! -s $ROOT/build/wr2/wr2_fixture.zip ]; then
        python3 $ROOT/tools/wr2_make_fixture.py --check || { echo "[lab] НЕ СОБРАН fixture"; exit 1; }
    fi
    mcopy -i $PART $ROOT/build/wr2/wr2_fixture.zip ::/wr2_fixture.zip
    mmd    -i $PART ::/SUBDIR
    printf 'stub content for the WR-2 navigate delta\n' > /tmp/subdir_file.txt
    mcopy -i $PART /tmp/subdir_file.txt ::/SUBDIR/NESTED.TXT
    mdir -i $PART ::/SUBDIR
    dd if=/dev/zero of=$IMG bs=512 count=64 status=none
    cat $PART >> $IMG
    echo "[lab] /fat диск собран: $(ls -la $IMG | awk '{print $5}') байт"

    cp /usr/share/OVMF/OVMF_VARS_4M.fd $LAB/vars.fd
    rm -f $LAB/full_serial.log
    qemu-system-x86_64 \
        -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
        -drive if=pflash,format=raw,file=$LAB/vars.fd \
        -drive file=$ROOT/build/auralite.iso,format=raw,if=ide,snapshot=on \
        -drive id=dapp,file=$IMG,format=raw,if=none,snapshot=on \
        -device ahci,id=ahci -device ide-hd,drive=dapp,bus=ahci.0 \
        -m 512M -smp 2 -cpu qemu64 \
        -no-reboot -no-shutdown -boot order=c \
        -netdev user,id=net0 -device e1000,netdev=net0 \
        -device usb-ehci,id=ehci -device usb-tablet,bus=ehci.0,id=gltablet \
        -fw_cfg name=opt/auralite.selftest,string=off \
        -vga std -display none \
        -chardev socket,id=ser0,path=$SER,server=on,wait=off,logfile=$LAB/full_serial.log \
        -serial chardev:ser0 \
        -monitor unix:$MON,server,nowait \
        -qmp unix:$QMP,server,nowait \
        >$LAB/qemu_stdout.log 2>$LAB/qemu_stderr.log &
    echo $! > $LAB/qemu.pid
    echo "[lab] QEMU pid $(cat $LAB/qemu.pid); ждём shell..."
    for i in $(seq 1 60); do [ -S $SER ] && break; sleep 2; done
    $GI serial-wait --serial $SER --pattern 'auralite#' --timeout 240 --log $LOG \
        && echo "[lab] shell достигнут" || { echo "[lab] ТАЙМАУТ boot"; exit 1; }
    ;;
launch)
    $GI serial-send --serial $SER "w32run /fat/7zFM.exe C:/fat &"
    echo "[lab] 7zFM запущен, ждём окно..."
    sleep 20
    $GI shot --monitor $MON $LAB/shot.png
    echo "[lab] кадр: $LAB/shot.png"
    ;;
stop)
    [ -f $LAB/qemu.pid ] && kill $(cat $LAB/qemu.pid) 2>/dev/null
    echo "[lab] остановлено"
    ;;
verify)
    # WR-2(b) gate: byte-exact extract from inside the archive through
    # IDD_COPY.  Full clean cycle: boot -> launch 7zFM -> open the archive
    # (double-click) -> select HELLO.TXT -> Extract (toolbar btn[1]) ->
    # OK in the Copy dialog -> read /fat/HELLO.TXT over serial and compare
    # every byte against the host's unpack of build/wr2/wr2_fixture.zip.
    # No green without the md5 matching the host reference.
    set -e
    EXPECT_MD5=$(python3 - <<'PY'
import zipfile, hashlib
z = zipfile.ZipFile("/home/user/AuraLite-OS/build/wr2/wr2_fixture.zip")
print(hashlib.md5(z.read("HELLO.TXT")).hexdigest())
PY
)
    bash "$0" stop >/dev/null 2>&1 || true
    bash "$0" boot >/dev/null
    sleep 3
    bash "$0" launch >/dev/null
    sleep 27
    $GI dblclick --monitor $MON --qmp $QMP 500 175 >/dev/null
    sleep 7
    $GI click  --monitor $MON --qmp $QMP 500 146 >/dev/null
    sleep 2
    $GI click  --monitor $MON --qmp $QMP 132 95  >/dev/null
    sleep 5
    $GI click  --monitor $MON --qmp $QMP 448 288 >/dev/null
    sleep 12
    LS=$($GI serial-run --serial $SER --cmd "ls /fat" --wait "auralite#" --timeout 15 2>/dev/null)
    echo "$LS" | grep -a "HELLO.TXT" | grep -aq "217 bytes" \
        || { echo "GATE FAIL: /fat/HELLO.TXT (217 bytes) отсутствует"; exit 1; }
    CAT=$($GI serial-run --serial $SER --cmd "cat /fat/HELLO.TXT" \
              --wait "auralite#" --timeout 15 2>/dev/null)
    GOT_MD5=$(python3 - "$CAT" <<'PY'
import sys, hashlib
t = sys.argv[1].split("cat /fat/HELLO.TXT", 1)[1]
t = t.rsplit("auralite#", 1)[0].lstrip(chr(10)).replace(chr(13), "")
print(hashlib.md5(t.encode()).hexdigest())
PY
)
    if [ "$GOT_MD5" = "$EXPECT_MD5" ]; then
        echo "GATE PASS: WR-2(b) byte-exact extract (md5 $GOT_MD5)"
    else
        echo "GATE FAIL: md5 guest=$GOT_MD5 host=$EXPECT_MD5"
        exit 1
    fi
    ;;
esac
