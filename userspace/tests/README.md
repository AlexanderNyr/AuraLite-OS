# userspace/tests — in-OS test programs

Programs that check the system from inside it and say so in a form a script
can grep. They ship into `/tests`, and `tests/integration/cases/*.sh` runs
them and asserts on their output.

They are deliberately shipped in the release image. Hiding them would break
the integration suite, which is the thing that tells us the OS works.

A test program here should:

- **print a machine-checkable line per check**, not a human-readable summary.
  `INSTTEST PASS: <what>` beats "everything looks fine".
- **distinguish "skipped" from "failed".** `/insttest` learned this the hard
  way: it reported a failure when `/disk` was simply not attached, which is a
  property of the QEMU invocation and not of the thing under test.
- **say why it failed.** A refusal with the wrong errno is a different bug
  from no refusal at all, and both look identical to a bare pass/fail.

Prefer a host unit test in `tests/unit/` where the logic is pure — it runs in
milliseconds and can compile the shipping source directly. A program belongs
here when it needs a real kernel underneath it.
