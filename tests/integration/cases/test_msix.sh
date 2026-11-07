#!/usr/bin/env bash
# test_msix.sh — RESIDUE3 C4 (RES-36) message-signalled interrupts on virtio.
#
# Four lanes, because "MSI works" is only half a claim until the fallback is
# proven to still work:
#
#   1. virtio-blk  + MSI-X on   — the receipt names the vector, the device
#                                 accepts the table entry, and a real request
#                                 delivers a real message
#   2. virtio-blk  + MSI off    — the fw_cfg knob keeps the device on its
#                                 INTx/polling path, and NO msi receipt may
#                                 appear
#   3. virtio-gpu  + MSI-X on   — same three steps through the control queue
#   4. virtio-gpu  + MSI off    — the GPU still comes up and still reaches
#                                 the shell
#
# Why a disk and a GPU rather than a unit test: the vector programming is a
# register write that a unit test CAN fake (tests/unit/test_msi_caps.c does,
# 12 cases).  What a unit test cannot show is that a device honours the table
# entry and signals the vector — that needs QEMU on the other side, which is
# what these lanes are.
#
# QEMU notes measured while writing this:
#   - NEITHER virtio-blk-pci nor virtio-gpu-pci in this QEMU build has an
#     `msix` property: passing it is a hard error ("Property ... not found"),
#     which is what the first run of this gate did -- every lane died before
#     the firmware ran.  MSI-X is offered by both devices unconditionally; the
#     plan's `virtio-blk-pci,msix=on` wording was a plan-time guess.
#   - the ISO is a raw hybrid image the runner attaches with if=ide, so the
#     virtio-blk lanes need their own scratch disk on if=none.
#   - extra QEMU arguments go straight after the timeout (il_run_qemu appends
#     "$@"); a "--" separator is NOT stripped, despite what lib.sh's own
#     header comment says -- QEMU answers it with "invalid option".

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "RESIDUE3 C4 MSI/MSI-X on virtio (RES-36)"

# A scratch disk for the virtio-blk lanes: the driver's self-test only READS
# LBA 0, so eight zeroed megabytes are enough and nothing ever writes to it.
DISK="$IL_LOGDIR/msix_scratch.img"
if [ ! -f "$DISK" ] || [ "$(wc -c < "$DISK")" -lt 8388608 ]; then
    dd if=/dev/zero of="$DISK" bs=1M count=8 status=none
fi

# IL_SELFTEST=fast: this gate is about interrupt transport, not about the
# self-test corpus, and the full corpus is a minute of serial output the lanes
# would only have to scroll through.  (lib.sh defaults to `full` for CI.)

# ---------------------------------------------------------------- lane 1 ---
il_section "lane 1/4 — virtio-blk with MSI-X enabled"

LOG="$IL_LOGDIR/msix_blk_on.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "exit"

IL_SELFTEST=fast il_run_qemu "$LOG" 90 \
    -drive "file=$DISK,format=raw,if=none,id=hd,snapshot=on" \
    -device "virtio-blk-pci,drive=hd"

il_assert_grep_fixed "$LOG" "[msi] message-signalled interrupts: ENABLED (build default on)" \
                     "the MSI switch is on by build default"
il_assert_grep        "$LOG" "\[msi\] virtio-blk: MSI-X vector [0-9]+ \(1 msg, [0-9]+ entr" \
                     "the MSI-X capability is parsed and a vector programmed"
il_assert_grep_fixed "$LOG" "[msi] virtio-blk: queue 0 -> table entry 0 (readback 0)" \
                     "the device accepted the MSI-X table entry"
il_assert_grep        "$LOG" "\[msi\] virtio-blk: [0-9]+ interrupt\(s\) delivered on vector" \
                     "an interrupt was actually DELIVERED on that vector"
il_assert_grep_fixed "$LOG" "[virtio-blk] self-test: LBA 0 read OK (interrupt delivered)" \
                     "a real request completed and the message arrived with it"
il_assert_grep        "$LOG" "\[virtio-blk\] ready: capacity [1-9][0-9]* sectors" \
                     "the device config capability yields a real capacity"
il_assert_no_grep     "$LOG" "spurious interrupt" "no spurious-vector report"
il_assert_no_grep     "$LOG" "PANIC|UNHANDLED EXCEPTION|from KERNEL mode" \
                     "no kernel exception/panic"
il_assert_grep_fixed "$LOG" "auralite#" "the boot still reaches the shell"

# ---------------------------------------------------------------- lane 2 ---
il_section "lane 2/4 — virtio-blk with MSI disabled (INTx fallback)"

LOG="$IL_LOGDIR/msix_blk_off.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "exit"

IL_SELFTEST=fast il_run_qemu "$LOG" 90 \
    -drive "file=$DISK,format=raw,if=none,id=hd,snapshot=on" \
    -device "virtio-blk-pci,drive=hd" \
    -fw_cfg "name=opt/auralite.msi,string=off"

il_assert_grep_fixed "$LOG" "[msi] message-signalled interrupts: DISABLED (fw_cfg opt/auralite.msi=off)" \
                     "the knob turns MSI off and says so"
il_assert_grep_fixed "$LOG" "[virtio-blk] self-test: LBA 0 read OK (INTx/polling)" \
                     "the device still serves requests with MSI off"
il_assert_no_grep     "$LOG" "delivered on vector" \
                     "no MSI receipt appears on the fallback lane"
il_assert_no_grep     "$LOG" "MSI-X vector" \
                     "no vector is programmed on the fallback lane"
il_assert_no_grep     "$LOG" "PANIC|UNHANDLED EXCEPTION|from KERNEL mode" \
                     "no kernel exception/panic"
il_assert_grep_fixed "$LOG" "auralite#" "the boot still reaches the shell"

# ---------------------------------------------------------------- lane 3 ---
il_section "lane 3/4 — virtio-gpu with MSI-X enabled"

LOG="$IL_LOGDIR/msix_gpu_on.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "exit"

# NOTE: no `msix=on` here -- seen above, this QEMU has no such property.
IL_SELFTEST=fast il_run_qemu "$LOG" 90 -device "virtio-gpu-pci"

il_assert_grep_fixed "$LOG" "[msi] virtio-gpu: MSI-X vector" \
                     "the GPU's MSI-X capability is parsed and a vector programmed"
il_assert_grep_fixed "$LOG" "[msi] virtio-gpu: queue 0 -> table entry 0 (readback 0)" \
                     "the GPU accepted the MSI-X table entry"
il_assert_grep        "$LOG" "\[msi\] virtio-gpu: [0-9]+ interrupt\(s\) delivered on vector" \
                     "the GPU delivered a message on that vector"
il_assert_grep        "$LOG" "\[virtio-gpu\] ready:" "the GPU driver came up"
il_assert_no_grep     "$LOG" "spurious interrupt" "no spurious-vector report"
il_assert_no_grep     "$LOG" "PANIC|UNHANDLED EXCEPTION|from KERNEL mode" \
                     "no kernel exception/panic"
il_assert_grep_fixed "$LOG" "auralite#" "the boot still reaches the shell with a GPU attached"

# ---------------------------------------------------------------- lane 4 ---
il_section "lane 4/4 — virtio-gpu with MSI disabled (INTx fallback)"

LOG="$IL_LOGDIR/msix_gpu_off.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "exit"

IL_SELFTEST=fast il_run_qemu "$LOG" 90 \
    -device "virtio-gpu-pci" \
    -fw_cfg "name=opt/auralite.msi,string=off"

il_assert_grep_fixed "$LOG" "[msi] message-signalled interrupts: DISABLED (fw_cfg opt/auralite.msi=off)" \
                     "the knob turns MSI off on the GPU lane too"
il_assert_grep        "$LOG" "\[virtio-gpu\] ready:" "the GPU still comes up with MSI off"
il_assert_no_grep     "$LOG" "delivered on vector" \
                     "no MSI receipt appears on the fallback lane"
# A disabled build must NOT claim "no capability": the device has one, the
# driver simply was not allowed to use it.  That distinction is the whole
# reason the attach site has three branches rather than two.
il_assert_no_grep     "$LOG" "MSI/MSI-X capability" \
                     "a disabled build does not misreport the device"
il_assert_no_grep     "$LOG" "PANIC|UNHANDLED EXCEPTION|from KERNEL mode" \
                     "no kernel exception/panic"
il_assert_grep_fixed "$LOG" "auralite#" "the boot still reaches the shell"

il_summary
