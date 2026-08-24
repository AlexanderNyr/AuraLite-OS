#!/usr/bin/env bash
# tests/integration/run_all.sh — orchestrate every QEMU integration test.
#
# Usage:
#   tests/integration/run_all.sh                 # run everything
#   tests/integration/run_all.sh --fast          # skip slow/flaky tests
#   tests/integration/run_all.sh ahci usb        # run only matching cases
#   FILTER=ahci tests/integration/run_all.sh     # same via env
#   tests/integration/run_all.sh --group usb     # one CI shard (see below)
#   tests/integration/run_all.sh --check-groups  # verify the shard partition
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
GROUP=""
CHECK_GROUPS=0
ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --fast|-f) FAST=1 ;;
        --group)   GROUP="$2"; shift ;;
        --check-groups) CHECK_GROUPS=1 ;;
        -h|--help)
            sed -n '2,12p' "$0" ; exit 0 ;;
        *) ARGS+=("$1") ;;
    esac
    shift
done
if [ "${#ARGS[@]}" -gt 0 ]; then
    FILTER="$(IFS='|'; echo "${ARGS[*]}")"
fi

# ---- test cases, in a deterministic order ----
#
# This list is hand-maintained, not discovered: the order matters (cheap
# smoke tests first) and some cases are deliberately grouped.  Because it is
# hand-maintained it can fall behind cases/ -- it once did, by 27 cases, a
# fifth of the suite that CI never ran.  tools/check_test_registry.py now
# fails the build when this list and cases/ disagree in either direction.
ALL_CASES=(
    test_boot_to_shell
    test_perf_smoke
    test_metal_null
    test_selftest_modes
    test_gui_dirty_uefi
    test_shell_commands
    test_syscalls
    test_selftest
    test_posix_p10
    test_execve_args
    test_errno
    test_tls_errno
    test_init_array
    test_stopped
    test_socket_errno
    test_keymaps
    test_posix2024_conf
    test_open_flags
    test_lseek
    test_signals
    test_termios
    test_jobcontrol
    test_permissions
    test_timestamps
    test_fifo_symlinks
    test_initrd_dirs
    test_install_dirs
    test_search_path
    test_sdk_examples
    test_spawn_argv
    test_spawn_argv_hostile
    test_apm_packages
    test_external_install
    test_runtime_layout
    test_gui_bad_pointers
    test_opengl
    test_process_cleanup
    test_memory_reaping
    test_fork_cow
    test_elf_permissions
    test_w32_pe_loader
    test_w32_kernel32
    test_w32_user32
    test_w32_crt
    test_w32_loadlibrary
    test_w32_integration
    test_ahci_large_read
    test_doom
    test_stack_guard
    test_panic_diag
    test_ist_double_fault
    test_rng
    test_crypto
    test_tls
    test_x2_https
    test_x509
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
    test_dns_cache
    test_dns_tcp
    test_ip_frag
    test_e1000_irq
    test_virtio_net
    test_udp_sockets
    test_http_get
    test_http_x6
    test_tcp_server
    test_tcp_x5
    test_tcp_options
    test_ipv6_ping6
    test_tcp6
    test_trust_store
    test_graphics
    test_smp
    test_smp_tss
    test_smp_init_order
    test_fpu_smp
    test_siginfo
    test_auxv
    test_fdshare
    test_gui
    test_gui_usb
    test_gbrowser
    test_gbrowser_net

    # ---- registered by AUDIT_A0: previously on disk but never run ----
    # Filesystem and core cases
    test_devfs
    test_procfs
    test_tmpfs
    test_diskfs
    test_fat32_mkdir
    test_mmap_shared
    test_mmap_file
    test_uaccess
    test_process_spawn_many
    # USB: the U3-U9 phase gates from USB_PLAN.md, plus older class cases
    test_xhci_address
    test_xhci_control
    test_xhci_bulk
    test_xhci_interrupt
    test_usb_hid_input
    test_usb_hub_depth
    test_usb_hub_full
    test_usb_full_stack
    test_usb_driver_registry
    test_usb_string
    test_usb_isoc
    test_usb_cdc_acm
    test_usb_printer
    test_usb_audio_full
    # Graphics and userspace
    test_3d_render
    test_virgl_gpu
    test_shell_all
    test_sysmon_data
    test_userspace_apps
)

# Slow ones we skip in --fast mode.
# test_doom boots UEFI and reads a 28 MB IWAD; test_ahci_large_read reads
# 16 MiB.  Both are correctness gates rather than smoke tests, so --fast skips
# them.
SLOW_CASES_RE='test_fat32_persistence|test_http_get|test_ext2|test_fs_stress|test_doom|test_ahci_large_read'

# ---- thematic CI shards (2026-08-21) ----
#
# One 129-case job ran ~2 h on a shared runner; six thematic shards run
# in parallel instead.  The partition lives HERE, next to the list it
# partitions, and is SELF-CHECKED on every invocation: each case must
# match EXACTLY ONE group regex.  A new case that matches none (or two)
# refuses to run rather than silently dropping out of CI — the
# AUDIT_A0 disease (27 cases on disk that CI never ran) does not get a
# second chapter.
GROUP_NAMES="core posix fs usb net gui"
group_re() {
    case "$1" in
        core)  echo '^test_(boot_to_shell|perf_smoke|metal_null|selftest|selftest_modes|shell_commands|syscalls|execve_args|errno|tls_errno|socket_errno|init_array|stopped|spawn_argv|spawn_argv_hostile|process_cleanup|process_spawn_many|memory_reaping|fork_cow|elf_permissions|stack_guard|panic_diag|ist_double_fault|smp|smp_tss|smp_init_order|fpu_smp|siginfo|auxv|fdshare|fd_isolation|user_processes|uaccess|mmap_shared|mmap_file)$' ;;
        posix) echo '^test_(posix_p10|posix2024_conf|open_flags|lseek|signals|termios|jobcontrol|permissions|timestamps|fifo_symlinks|initrd_dirs|install_dirs|search_path|sdk_examples|apm_packages|external_install|runtime_layout|keymaps|shell_all|sysmon_data|userspace_apps)$' ;;
        fs)    echo '^test_(ahci_large_read|ahci_rw|fat32_persistence|fat32_full|fat32_mkdir|ext2|fs_stress|devfs|procfs|tmpfs|diskfs)$' ;;
        usb)   echo '^test_(usb_[a-z0-9_]+|usbfs|usbfs_fat32|xhci_[a-z]+)$' ;;
        net)   echo '^test_(networking|dns_cache|dns_tcp|ip_frag|e1000_irq|virtio_net|udp_sockets|http_get|http_x6|tcp_server|tcp_x5|tcp_options|ipv6_ping6|tcp6|trust_store|rng|crypto|tls|x2_https|x509|gbrowser_net)$' ;;
        gui)   echo '^test_(gui|gui_dirty_uefi|gui_usb|gui_bad_pointers|opengl|graphics|3d_render|virgl_gpu|gbrowser|doom|w32_[a-z0-9_]+)$' ;;
        *)     echo '' ;;
    esac
}

check_groups() {
    local bad=0 c n g re
    for c in "${ALL_CASES[@]}"; do
        n=0
        for g in $GROUP_NAMES; do
            re=$(group_re "$g")
            [[ "$c" =~ $re ]] && n=$((n+1))
        done
        if [ "$n" -ne 1 ]; then
            echo "${C_R}shard partition BROKEN: $c matches $n group(s); every case must match exactly one — fix group_re() in $0${C_END}"
            bad=1
        fi
    done
    return "$bad"
}

# The partition is checked on EVERY run (129 x 6 regex matches is
# free); --check-groups checks and exits, for the CI step and the
# curious.
if ! check_groups; then
    exit 2
fi
if [ "$CHECK_GROUPS" -eq 1 ]; then
    echo "shard partition OK: every case matches exactly one of: $GROUP_NAMES"
    exit 0
fi
if [ -n "$GROUP" ]; then
    FILTER=$(group_re "$GROUP")
    if [ -z "$FILTER" ]; then
        echo "${C_R}unknown group '$GROUP' (have: $GROUP_NAMES)${C_END}"
        exit 2
    fi
fi

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

# Per-case verdicts land in the artifact CI uploads: step stdout is
# admin-only on GitHub, serial logs alone cannot say which case
# failed (measured while chasing the first matrix runs), but this
# file travels with the logs and names names.
RESULTS="$ROOT/build/integration-logs/results-${GROUP:-all}.txt"
mkdir -p "$ROOT/build/integration-logs"
: > "$RESULTS"

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
    # The case's own stdout (its per-assert ✔/✘ lines) goes to the
    # artifact too: GitHub step stdout is admin-only on public repos,
    # and the serial logs alone cannot name a failing ASSERT -- the
    # first sharded runs proved results-*.txt names the failing CASE,
    # this file names the failing LINE.
    CASE_OUT="$ROOT/build/integration-logs/${case_name}.out"
    bash "$script" 2>&1 | tee "$CASE_OUT"
    case_rc=${PIPESTATUS[0]}          # tee's 0 must not mask the case
    if [ "$case_rc" -eq 0 ]; then
        dt=$(( $(date +%s) - t0 ))
        echo "${C_G}${C_BOLD}✔ PASS${C_END} $case_name  ${C_DIM}(${dt}s)${C_END}"
        echo "PASS $case_name ${dt}s" >> "$RESULTS"
        pass=$((pass+1))
    else
        dt=$(( $(date +%s) - t0 ))
        echo "${C_R}${C_BOLD}✘ FAIL${C_END} $case_name  ${C_DIM}(${dt}s)${C_END}"
        echo "FAIL $case_name ${dt}s" >> "$RESULTS"
        fail=$((fail+1))
        FAILED_LIST+=("$case_name")
    fi
done

total_dt=$(( $(date +%s) - total_t0 ))
echo "SUMMARY pass=$pass fail=$fail skipped=$skipped time=${total_dt}s group=${GROUP:-all}" >> "$RESULTS"

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
