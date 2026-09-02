#!/usr/bin/env bash
# test_f2fs.sh — F7 integration wrapper (FSFULL_PLAN.md F7).
#
# Runs the F4 f2fs harness (tests/f2fs/test_f2fs.sh) so it is covered by
# the CI "fsfull" shard.  The harness is fully self-contained: it builds
# its own disks (each case owns its state — no shared disk between cases),
# drives its own QEMU boots, and exits nonzero if any of its assertions
# fail.  `exec` preserves that exit code.
#
# It was already exercised as a standalone target (patches/FS_F4_f2fs.patch);
# this file only registers it with the shard runner.
set -u
exec "$(dirname "$0")/../../f2fs/test_f2fs.sh"
