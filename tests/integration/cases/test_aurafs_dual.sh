#!/usr/bin/env bash
# M6n: explicit new-format creation, media-image torn-super fault, fresh boot.
set -uo pipefail
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3
IL_SELFTEST=fast
IL_SMP=4
IL_CPU=qemu64
LOG="$IL_LOGDIR/aurafs_dual.log"
LOG2="$IL_LOGDIR/aurafs_dual_recover.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT
DISKS=(-device ahci,id=dual-ahci)
for i in 0 1 2; do
    img="$IL_LOGDIR/aurafs_dual_$i.img"
    rm -f "$img"; truncate -s 64M "$img"
    DISKS+=(-drive "id=dual-d$i,file=$img,format=raw,if=none"
            -device "ide-hd,drive=dual-d$i,bus=dual-ahci.$i")
done
il_section "explicit dual-v2 format on all-zero /data, then two checkpoints"
il_send_prompt 'write /data/dual-note DUAL_OLD'
il_send_prompt 'fsync /data'
il_send_prompt 'write /data/dual-note DUAL_NEW'
il_send_prompt 'fsync /data'
il_run_qemu_prompt "$LOG" 300 "${DISKS[@]}" \
    -fw_cfg name=opt/auralite.fsformat,string=1 \
    -fw_cfg name=opt/auralite.aurafs_dual,string=1
il_assert_grep "$LOG" '\[data\] PASS: .*mounted at /data' '/data mounted'
il_assert_grep "$LOG" 'checkpoint format=dual-v2 .*migration=none' 'explicit new format created'
il_assert_count "$LOG" '^fsync: /data' 2 'both checkpoints completed'
il_assert_grep "$LOG" 'barriers=device-FLUSH' 'device barriers used'
# QEMU has exited. Corrupt only the newest slot's first sector on disposable
# media, keeping the older complete checkpoint intact. This is NOT a live
# hardware power-cut simulation; it is a reproducible torn-image fixture.
META="$IL_LOGDIR/aurafs_dual_fault.txt"
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if python3 - "$IL_LOGDIR/aurafs_dual_2.img" "$META" <<'PY'
import hashlib, struct, sys
path, meta = sys.argv[1:]
slots = []
with open(path, 'r+b') as f:
    for offset in (65536, 61440):
        f.seek(offset); b = f.read(4096)
        assert len(b) == 4096 and b[128:136] == b'_AURA2FS'
        assert hashlib.sha256(b[:4064]).digest() == b[4064:]
        assert struct.unpack_from('<Q', b, 24)[0] == offset
        slots.append((struct.unpack_from('<Q', b, 56)[0], offset))
    slots.sort()
    assert slots[1][0] > slots[0][0]
    f.seek(slots[1][1]); f.write(bytes(512)); f.flush()
with open(meta, 'w') as f:
    f.write(str(slots[0][1]) + '\n')
print('torn newest slot', slots[1], 'expected fallback', slots[0])
PY
then il_pass 'valid distinct generations verified, newest header torn'
else il_fail 'could not create validated media fault'; il_summary; exit 1
fi
OLDER=$(cat "$META")
IL_LAST_LOG="$LOG2"
il_send_prompt 'cat /data/dual-note'
il_send_prompt 'run /tests/aurafsfd'
il_send_prompt 'echo DUAL_FD_RC=$?'
il_send_prompt 'write /data/guard-note ROOT_OLD'
il_send_prompt 'fsync /data'
il_send_prompt 'write /data/guard-note ROOT_NEW'
il_send_prompt 'fsync /data'
il_send_prompt 'echo DUAL_RECOVER_ALIVE'
il_run_qemu_prompt "$LOG2" 240 "${DISKS[@]}" \
    -fw_cfg name=opt/auralite.fsformat,string=0 \
    -fw_cfg name=opt/auralite.aurafs_dual,string=0
il_assert_grep "$LOG2" "checkpoint format=dual-v2 active=$OLDER " 'v2 autodetected and older intact slot selected'
il_assert_count "$LOG2" 'DUAL_OLD' 1 'old checkpoint content recovered'
il_assert_no_grep "$LOG2" 'DUAL_NEW' 'unrecoverable new checkpoint not served'
il_assert_count "$LOG2" '\[aurafs-fd\] PASS: actual open/dup/fork/close' 1 'actual userspace FD lifecycle works after recovery on v2'
il_assert_grep "$LOG2" '^DUAL_FD_RC=0' 'userspace program exit zero'
il_assert_grep "$LOG2" 'checkpoint candidate metadata verified before publication' 'candidate metadata verified before real sync publication'
il_assert_grep "$LOG2" '^DUAL_RECOVER_ALIVE' 'shell alive after recovery and writes'
il_assert_no_grep "$LOG2" 'formatting CoW|\[aurafs-fd\] FAIL:|\[bsod\]|KERNEL EXCEPTION|\[DOUBLE FAULT\]' 'no reformat or test/kernel failure'
# M6s: both supers and root blocks remain valid; a newer descendant is corrupt. Do not
# reinterpret that as permission to roll back to an older namespace.
LOG3="$IL_LOGDIR/aurafs_dual_root_refusal.log"
HASH="$IL_LOGDIR/aurafs_dual_root_fault.sha256"
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if python3 - "$IL_LOGDIR/aurafs_dual_2.img" "$HASH" <<'PYCODE'
import hashlib, struct, sys
path, receipt = sys.argv[1:]
slots = []
with open(path, 'r+b') as f:
    for offset in (65536, 61440):
        f.seek(offset); b = f.read(4096)
        assert b[128:136] == b'_AURA2FS'
        assert hashlib.sha256(b[:4064]).digest() == b[4064:]
        assert struct.unpack_from('<Q', b, 24)[0] == offset
        slots.append((struct.unpack_from('<Q', b, 56)[0],
                      struct.unpack_from('<Q', b, 64)[0]))
    slots.sort(); assert slots[1][0] > slots[0][0] and slots[1][1] != slots[0][1]
    def tree(root):
        seen, leaves, pending = set(), set(), [root]
        while pending:
            at = pending.pop()
            assert at not in seen and len(seen) < 16384
            seen.add(at); f.seek(at); b = f.read(4096)
            assert hashlib.sha256(b[:4064]).digest() == b[4064:]
            assert struct.unpack_from('<Q', b, 24)[0] == at
            typ, count = struct.unpack_from('<I', b, 12)[0], struct.unpack_from('<I', b, 32)[0]
            if typ == 2:
                assert 0 < count <= 125
                pending.extend(struct.unpack_from('<Q', b, 36 + i * 32 + 24)[0] for i in range(count))
            else:
                assert typ == 3
                leaves.add(at)
        return seen, leaves
    older, _ = tree(slots[0][1]); newer, leaves = tree(slots[1][1])
    target = sorted(leaves - older - {slots[1][1]})
    assert target, 'need a newer-exclusive descendant, not either root'
    f.seek(target[0]); f.write(bytes(512)); f.flush()
with open(path, 'rb') as f:
    digest = hashlib.file_digest(f, 'sha256').hexdigest()
with open(receipt, 'w') as f: f.write(digest)
print('both supers/roots valid, older tree intact, newer exclusive descendant torn', slots, target[0])
PYCODE
then il_pass 'validated descendant fault leaves both supers/roots and older tree intact'
else il_fail 'could not create root fault'; il_summary; exit 1
fi
IL_LAST_LOG="$LOG3"
il_send_prompt 'echo ROOT_GUARD_ALIVE'
il_run_qemu_prompt "$LOG3" 240 "${DISKS[@]}" \
    -fw_cfg name=opt/auralite.fsformat,string=0 \
    -fw_cfg name=opt/auralite.aurafs_dual,string=0
il_assert_grep "$LOG3" 'selected checkpoint metadata guard failed; mount refused' 'bad selected descendant refuses mount'
il_assert_no_grep "$LOG3" '\[data\] PASS:|checkpoint format=dual-v2' 'no older-root mount fallback'
il_assert_grep "$LOG3" '^ROOT_GUARD_ALIVE' 'shell alive after mount refusal'
il_assert_no_grep "$LOG3" 'formatting CoW|\[bsod\]|KERNEL EXCEPTION|\[DOUBLE FAULT\]' 'no format or kernel failure'
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if python3 - "$IL_LOGDIR/aurafs_dual_2.img" "$HASH" <<'PYCODE'
import hashlib, sys
with open(sys.argv[1], 'rb') as f: digest = hashlib.file_digest(f, 'sha256').hexdigest()
with open(sys.argv[2]) as f: assert digest == f.read().strip()
PYCODE
then il_pass 'entire faulted data image unchanged by refusal boot'
else il_fail 'data image changed on refused mount'
fi
il_summary
