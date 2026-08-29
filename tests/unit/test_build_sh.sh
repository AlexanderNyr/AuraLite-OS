#!/usr/bin/env bash
# test_build_sh.sh -- SELFHOST_PLAN.md SH6f host gate: D5 target set + resume.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
FAILED=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAILED=1; }

# ---- D5: the two descriptions name the same targets ----
MAKE_SET=$(awk '/^SELFHOST_TARGETS[[:space:]]*:?=/{sub(/^[^=]*=[[:space:]]*/,""); print; exit}' Makefile)
GUEST_SET=$(awk '/^# SELFHOST_TARGETS:/{sub(/^# SELFHOST_TARGETS:[[:space:]]*/,""); print; exit}' tools/selfhost/Selfhost.mk)
if [ -z "$MAKE_SET" ]; then
    fail "Makefile missing SELFHOST_TARGETS"
elif [ -z "$GUEST_SET" ]; then
    fail "Selfhost.mk missing SELFHOST_TARGETS"
else
    # Word-set compare, order-insensitive.
    sort_words() { echo "$1" | tr ' ' '\n' | awk 'NF' | LC_ALL=C sort | tr '\n' ' '; }
    a=$(sort_words "$MAKE_SET")
    b=$(sort_words "$GUEST_SET")
    if [ "$a" = "$b" ]; then
        pass "D5: Makefile and Selfhost.mk name the same targets ($MAKE_SET)"
    else
        fail "D5 drift: Makefile='$MAKE_SET' Selfhost.mk='$GUEST_SET'"
    fi
fi

# Each D5 name is a rule in Selfhost.mk.
for t in $GUEST_SET; do
    if grep -q "^${t}:" tools/selfhost/Selfhost.mk; then
        pass "Selfhost.mk has rule '$t'"
    else
        fail "Selfhost.mk has no rule '$t'"
    fi
done

# ---- shmake + FAT= override: stop at phase6, resume, no-op ----
SHMAKE="$ROOT/build/shmake"
mkdir -p "$ROOT/build"
cc -std=c99 -Wall -Wextra -Werror -O2 -o "$SHMAKE" "$ROOT/tools/shmake/shmake.c" \
    || { echo "FAIL: shmake does not compile"; exit 1; }

WORKDIR=$(mktemp -d "$ROOT/build/sh6f-test.XXXXXX")
trap 'rm -rf "$WORKDIR"' EXIT
cd "$WORKDIR"
cat > stamp.sh << 'EOF'
#!/bin/sh
tag=$1; target=$2
echo "stamp $target" > "$target"
echo "g$tag rebuilt $target"
EOF
chmod +x stamp.sh

"$SHMAKE" -C "$WORKDIR" -f "$ROOT/tools/selfhost/Selfhost.mk" \
    CC="$WORKDIR/stamp.sh" phase6 > stop.out 2> stop.err
if grep -q "gp6 rebuilt P6" stop.out \
   && ! grep -q "gkern rebuilt" stop.out; then
    pass "phase6 builds P6 and not KERNEL"
else
    fail "phase6 graph"; cat stop.out; cat stop.err
fi
if [ -f "$WORKDIR/P6" ] && [ ! -f "$WORKDIR/KERNEL" ]; then
    pass "after phase6, P6 exists and KERNEL does not"
else
    fail "phase6 products (P6=$(ls P6 2>/dev/null || echo missing) KERNEL=$(ls KERNEL 2>/dev/null || echo missing))"
fi

"$SHMAKE" -C "$WORKDIR" -f "$ROOT/tools/selfhost/Selfhost.mk" \
    CC="$WORKDIR/stamp.sh" kernel initrd > resume.out 2> resume.err
if grep -q "gkern rebuilt KERNEL" resume.out \
   && grep -q "ginit rebuilt INITRD" resume.out \
   && ! grep -q "gp6 rebuilt" resume.out; then
    pass "resume builds KERNEL+INITRD and does not rebuild P6"
else
    fail "resume graph"; cat resume.out; cat resume.err
fi

"$SHMAKE" -C "$WORKDIR" -f "$ROOT/tools/selfhost/Selfhost.mk" \
    CC="$WORKDIR/stamp.sh" kernel initrd > noop.out 2> noop.err
if grep -q "is up to date" noop.out && ! grep -q "gkern rebuilt" noop.out; then
    pass "second full run rebuilds nothing"
else
    fail "no-op run"; cat noop.out; cat noop.err
fi

echo
if [ "$FAILED" -eq 0 ]; then
    echo "=== ALL BUILD.SH / D5 TESTS PASSED ==="
    exit 0
fi
echo "=== BUILD.SH / D5 TESTS FAILED ==="
exit 1
