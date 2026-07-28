#!/usr/bin/env bash
# test_sysmon_data.sh — verify the real /proc data backing the gsysmon GUI
# app and general CPU/mem/net/disk accounting (not the old placeholder
# random-number generator that used to live in gsysmon.c).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "sysmon backing data (/proc/loadavg, /proc/netdev, /proc/diskstats)"

LOG="$IL_LOGDIR/sysmon_data.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "cat /proc/loadavg"
il_send_delay 1
il_send "cat /proc/netdev"
il_send_delay 1
il_send "cat /proc/diskstats"
il_send_delay 1
il_send "cat /proc/stat"
il_send_delay 1
il_send "cat /proc/cpuinfo"
il_send_delay 1
il_send "run /gsysmon"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 30

il_assert_grep "$LOG" "[0-9]+\\.[0-9][0-9] [0-9]+\\.[0-9][0-9] [0-9]+\\.[0-9][0-9] [0-9]*/[0-9]* [0-9]*" \
    "loadavg has real load/runnable-thread fields"
il_assert_grep "$LOG" "Inter-|" "netdev header present"
il_assert_grep "$LOG" "e1000:" "netdev shows the active NIC's stats row"
il_assert_grep "$LOG" "ahci0" "diskstats shows the AHCI stats row"
il_assert_grep "$LOG" "cores" "cpuinfo shows a real (SMP-derived) core count"
il_assert_grep "$LOG" "loaded 3 segment" "gsysmon launched as a real ELF process"

il_summary
