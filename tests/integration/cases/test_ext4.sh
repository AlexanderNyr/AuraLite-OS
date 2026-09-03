#!/usr/bin/env bash
# test_ext4.sh — F7 integration wrapper (FSFULL_PLAN.md F7).
#
# Runs the F3 ext4 harness (tests/ext4/test_ext4.sh) so it is covered by
# the CI "fsfull" shard.  The harness is fully self-contained: it builds
# its own disks (each case owns its state — no shared disk between cases),
# drives its own QEMU boots, and exits nonzero if any of its assertions
# fail.  `exec` preserves that exit code.
#
# It was already exercised as a standalone target (patches/FS_F3_ext4.patch);
# this file only registers it with the shard runner.
set -u
exec bash "$(dirname "$0")/../../ext4/test_ext4.sh"
