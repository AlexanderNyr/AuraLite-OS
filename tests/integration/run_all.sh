#!/usr/bin/env bash
# tests/integration/run_all.sh — orchestrate every QEMU integration test.
#
# Usage:
#   tests/integration/run_all.sh             # run everything
#   tests/integration/run_all.sh --fast      # skip slow/flaky tests
#   tests/integration/run_all.sh ahci usb    # run only matching cases
#   FILTER=ahci tests/integration/run_all.sh # same via env
#
# Exit code is 0 only if every case PASSes.

set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# ---- colors (consistent with lib.sh) ----
if [ -t 1 ] && [ "${NO_COLOR:-}" = "" ]; then
    C_R=$'\033[31m'; C_G=$'\033[32m'; C_Y=$'\033[33m'
    C_B=$'\033[34m'; C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'; C_END=$'\033[0m'
else
    C_R=""; C_G=""; C_Y=""; C_B=""; C_BOLD=""; C_DIM=""; C_END=""
fi

# ---- argument parsing ----
FAST=0
FILTER="${FILTER:-}"
ARGS=()
for a in "$@"; do
    case "$a" in
        --fast|-f) FAST=1 ;;
        -h|--help)
            sed -n '2,12p' "$0" ; exit 0 ;;
        *) ARGS+=("$a") ;;
    esac
done
if [ "${#ARGS[@]}" -gt 0 ]; then
    FILTER="$(IFS='|'; echo "${ARGS[*]}")"
fi

# ---- discover test cases (in a deterministic order) ----
ALL_CASES=(
    test_boot_to_shell
    test_shell_commands
    test_syscalls
    test_selftest
    test_posix_p10
    test_execve_args
    test_errno
    test_open_flags
    test_lseek
    test_signals
    test_termios
    test_jobcontrol
    test_permissions
    test_timestamps
    test_fifo_symlinks
    test_gui_bad_pointers
    test_process_cleanup
    test_memory_reaping
    test_fork_cow
    test_elf_permissions
    test_stack_guard
    test_fd_isolation
    test_user_processes
    test_ahci_rw
    test_fat32_persistence
    test_fat32_full
    test_ext2
    test_fs_stress
    test_usb_msc
    test_usb_hid
    test_usb_ohci
    test_usb_ehci
    test_usb_ehci_hid
    test_usb_xhci
    test_usb_hub
    test_usb_generic_hid
    test_usb_generic_keyboard
    test_usb_xhci_hub
    test_usb_hotplug
    test_usb_msc_hotplug
    test_usbfs
    test_usbfs_fat32
    test_networking
    test_e1000_irq
    test_virtio_net
    test_udp_sockets
    test_http_get
    test_tcp_server
    test_graphics
    test_smp
    test_smp_tss
    test_smp_init_order
    test_gui
    test_gui_usb
)

# Slow ones we skip in --fast mode.
SLOW_CASES_RE='test_fat32_persistence|test_http_get|test_ext2|test_fs_stress'

# ---- prereqs ----
need=(qemu-system-x86_64 python3 xorriso clang ld.lld nasm)
missing=0
for b in "${need[@]}"; do
    command -v "$b" >/dev/null 2>&1 || { echo "${C_R}missing: $b${C_END}"; missing=1; }
done
[ "$missing" -eq 0 ] || exit 2

# ---- build the ISO once ----
if [ ! -f "$ROOT/build/auralite.iso" ]; then
    echo "${C_Y}[runner] building ISO…${C_END}"
    make iso >/dev/null
fi

# ---- run ----
pass=0; fail=0; skipped=0
FAILED_LIST=()
total_t0=$(date +%s)

print_banner() {
    echo
    echo "${C_BOLD}${C_B}╭─────────────────────────────────────────────────────────────╮${C_END}"
    printf  "${C_BOLD}${C_B}│ %-59s │${C_END}\n" "$1"
    echo "${C_BOLD}${C_B}╰─────────────────────────────────────────────────────────────╯${C_END}"
}

for case_name in "${ALL_CASES[@]}"; do
    script="$HERE/cases/${case_name}.sh"
    [ -x "$script" ] || chmod +x "$script" 2>/dev/null || true
    [ -f "$script" ] || { echo "${C_Y}skip $case_name (no script)${C_END}"; continue; }

    if [ -n "$FILTER" ] && ! [[ "$case_name" =~ $FILTER ]]; then
        skipped=$((skipped+1))
        continue
    fi
    if [ "$FAST" -eq 1 ] && [[ "$case_name" =~ $SLOW_CASES_RE ]]; then
        echo "${C_DIM}skip (--fast): $case_name${C_END}"
        skipped=$((skipped+1))
        continue
    fi

    print_banner "▶ $case_name"
    t0=$(date +%s)
    if bash "$script"; then
        dt=$(( $(date +%s) - t0 ))
        echo "${C_G}${C_BOLD}✔ PASS${C_END} $case_name  ${C_DIM}(${dt}s)${C_END}"
        pass=$((pass+1))
    else
        dt=$(( $(date +%s) - t0 ))
        echo "${C_R}${C_BOLD}✘ FAIL${C_END} $case_name  ${C_DIM}(${dt}s)${C_END}"
        fail=$((fail+1))
        FAILED_LIST+=("$case_name")
    fi
done

total_dt=$(( $(date +%s) - total_t0 ))

echo
echo "${C_BOLD}════════════════════════ SUMMARY ════════════════════════${C_END}"
printf "  passed : ${C_G}%d${C_END}\n" "$pass"
printf "  failed : ${C_R}%d${C_END}\n" "$fail"
printf "  skipped: %d\n" "$skipped"
printf "  time   : %ds\n" "$total_dt"
echo "  logs   : build/integration-logs/"
echo

if [ "$fail" -eq 0 ]; then
    echo "${C_BOLD}${C_G}ALL INTEGRATION TESTS PASSED${C_END}"
    exit 0
else
    echo "${C_BOLD}${C_R}FAILED:${C_END}"
    printf "  - %s\n" "${FAILED_LIST[@]}"
    exit 1
fi
