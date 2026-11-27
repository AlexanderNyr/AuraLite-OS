#!/usr/bin/env bash
# M6k: real userspace FD/OFD lifetime on fresh persistent AuraFS /data.
set -uo pipefail
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64
IL_SELFTEST=fast
IL_SMP=4
IL_CPU=qemu64
LOG="$IL_LOGDIR/aurafs_fd.log"
LOG2="$IL_LOGDIR/aurafs_fd_remount.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT
il_section "AuraFS userspace open/dup/fork/close lifecycle"
DISKS=(-device ahci,id=fd-ahci)
for i in 0 1 2; do
    img="$IL_LOGDIR/aurafs_fd_$i.img"
    rm -f "$img"
    truncate -s 64M "$img"
    DISKS+=(-drive "id=fd-d$i,file=$img,format=raw,if=none"
            -device "ide-hd,drive=fd-d$i,bus=fd-ahci.$i")
done
il_send_prompt "run /tests/aurafsfd"
il_send_prompt 'echo AURAFS_FD_RC=$?'
il_send_prompt 'echo AURAFS_FD_ALIVE'
il_run_qemu_prompt "$LOG" 240 "${DISKS[@]}" -fw_cfg name=opt/auralite.fsformat,string=1
il_assert_grep "$LOG" '\[data\] PASS: .*mounted at /data' '/data really mounted'
il_assert_count "$LOG" '\[aurafs-fd\] PASS: actual open/dup/fork/close' 1 'userspace lifecycle completed once'
il_assert_grep "$LOG" '^AURAFS_FD_RC=0' 'program exit status zero'
il_assert_grep "$LOG" '\[aurafs-fd\] ok: fork shares OFD offset' 'real fork shared offset and child close'
il_assert_grep "$LOG" '\[aurafs-fd\] ok: 96 real open-close cycles' 'real cache pressure retained old descriptors'
il_assert_grep "$LOG" '\[aurafs-fd\] ok: last open hardlink survives' 'open hardlink after last unlink'
il_assert_grep "$LOG" '^AURAFS_FD_ALIVE' 'shell responsive afterwards'
il_assert_no_grep "$LOG" '\[aurafs-fd\] FAIL:|\[bsod\]|KERNEL EXCEPTION|\[DOUBLE FAULT\]|STACK CORRUPTION' 'no test or kernel failure'
# Same images, new QEMU, no format permission; completion marker was fsynced.
IL_LAST_LOG="$LOG2"
il_send_prompt 'cat /data/m6k-result'
il_send_prompt 'echo AURAFS_FD_REMOUNT_ALIVE'
il_run_qemu_prompt "$LOG2" 120 "${DISKS[@]}" -fw_cfg name=opt/auralite.fsformat,string=0
il_assert_grep "$LOG2" '\[data\] PASS: .*mounted at /data' '/data remounted without format'
il_assert_count "$LOG2" 'M6k-FD-PASS' 1 'fsynced completion marker survived power-off'
il_assert_grep "$LOG2" '^AURAFS_FD_REMOUNT_ALIVE' 'remounted shell responsive'
il_assert_no_grep "$LOG2" '\[bsod\]|KERNEL EXCEPTION|\[DOUBLE FAULT\]|STACK CORRUPTION' 'no remount exception'
il_summary
