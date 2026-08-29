#!/usr/bin/env python3
"""prompt_qemu.py -- serial driver for long AuraLite guest jobs.

The init shell reads its UART through a polling/FIFO path.  Sending a whole
build queue on a wall-clock schedule loses lines whenever tcc is still busy.
This small host-side transport helper instead waits for a *fresh* ``auralite#``
prompt before it transmits each next command.  It does not build or inspect
any guest artifact: it only provides reliable serial keystrokes for the SH5d
in-guest compiler gate.

Beyond transport it enforces one cheap invariant, because the queue it drives
is ~170 compiler invocations: every ``run`` command must be observed to exit
zero before the next one is sent.  AuraLite's kernel prints
``[thread] '<name>' (tid N) exited (code=N)`` for every exiting thread
(kernel/proc/thread.c), so the receipt is already on the serial line.  Without
this the first failing tcc invocation would be followed by another hundred
compiles and a confusing link error minutes later.  ``--no-check-run-exit``
turns the check off for callers that queue ``run`` commands expected to fail.

Usage (normally through il_run_qemu_prompt in lib.sh):
    prompt_qemu.py --log LOG --timeout SECONDS --commands FILE \
        [--no-check-run-exit] -- qemu args...
"""

from __future__ import annotations

import argparse
import os
import re
import selectors
import signal
import subprocess
import sys
import time
from pathlib import Path

PROMPT = b"auralite#"
# kernel/proc/thread.c prints "[thread] '<name>' (tid N) exited (code=N)" for
# every thread that leaves.  A `run` command that produced no code=0 line did
# not complete successfully, whatever the shell printed around it.
EXIT_OK = b"exited (code=0)"
EXIT_ANY = re.compile(rb"exited \(code=(-?\d+)\)")


def terminate_group(proc: subprocess.Popen[bytes]) -> None:
    """Stop QEMU without matching unrelated processes by name."""
    if proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        proc.wait(timeout=3)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True)
    parser.add_argument("--timeout", required=True, type=float)
    parser.add_argument("--commands", required=True)
    parser.add_argument("--no-check-run-exit", action="store_true",
                        help="do not require `run` commands to exit zero")
    parser.add_argument("qemu", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    qemu = args.qemu
    if qemu[:1] == ["--"]:
        qemu = qemu[1:]
    if not qemu:
        parser.error("missing QEMU command after --")

    commands = [line.rstrip("\r\n") for line in
                Path(args.commands).read_text(encoding="utf-8").splitlines()
                if line.rstrip("\r\n")]
    if not commands:
        parser.error("commands file is empty")

    log_path = Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + args.timeout
    print("[prompt-qemu] waiting for AuraLite prompt; %d command(s) queued" %
          len(commands), flush=True)

    proc = subprocess.Popen(
        qemu,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
        start_new_session=True,
    )
    assert proc.stdin is not None and proc.stdout is not None

    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    rolling = bytearray()
    sent = 0
    waiting_for_prompt = True
    complete_at: float | None = None
    final_is_exit = False
    early_exit = False
    timed_out = False
    # Command most recently transmitted, and everything the guest printed
    # since.  Used to prove each `run` reached a zero exit before the queue
    # moves on; see the module docstring.
    pending: str | None = None
    since_send = bytearray()
    check_run_exit = not args.no_check_run_exit
    bad_run: str | None = None

    def run_exit_problem() -> str | None:
        """Why the pending `run` command did not exit zero (None if it did)."""
        if pending is None or not pending.lstrip().startswith("run "):
            return None
        if EXIT_OK in since_send:
            return None
        codes = EXIT_ANY.findall(bytes(since_send))
        if codes:
            return ("command %d/%d (%s) reported exit code %s, not 0"
                    % (sent, len(commands), pending[:80],
                       codes[-1].decode("ascii")))
        return ("command %d/%d (%s) produced no thread-exit receipt at all"
                % (sent, len(commands), pending[:80]))

    try:
        with log_path.open("wb") as log:
            while True:
                now = time.monotonic()
                if now >= deadline:
                    timed_out = True
                    print("[prompt-qemu] timeout after %.0fs (%d/%d commands sent)" %
                          (args.timeout, sent, len(commands)), flush=True)
                    break

                # `exit` intentionally removes the shell prompt.  Give its
                # final output a short opportunity to flush before stopping
                # QEMU; all other final commands must prove completion by a
                # newly printed prompt.
                if complete_at is not None and now >= complete_at:
                    break

                events = selector.select(timeout=min(0.25, deadline - now))
                if not events:
                    if proc.poll() is not None:
                        if sent < len(commands):
                            early_exit = True
                        break
                    continue

                for key, _ in events:
                    try:
                        chunk = os.read(key.fd, 65536)
                    except BlockingIOError:
                        continue
                    if not chunk:
                        selector.unregister(proc.stdout)
                        if proc.poll() is not None:
                            if sent < len(commands):
                                early_exit = True
                            break
                        continue
                    log.write(chunk)
                    log.flush()
                    rolling.extend(chunk)
                    since_send.extend(chunk)
                    # Keep enough overlap to find a prompt split across reads,
                    # but clear it after handling one so an old prompt can
                    # never dispatch two commands.
                    if waiting_for_prompt and PROMPT in rolling:
                        rolling.clear()
                        # The shell only prints a fresh prompt once the
                        # previous command is done, so this is the one point
                        # where the pending command's exit receipt is complete.
                        if check_run_exit:
                            bad_run = run_exit_problem()
                            if bad_run is not None:
                                break
                        if sent < len(commands):
                            command = commands[sent]
                            sent += 1
                            try:
                                proc.stdin.write(command.encode("utf-8") + b"\n")
                                proc.stdin.flush()
                            except BrokenPipeError:
                                early_exit = True
                                break
                            print("[prompt-qemu] %d/%d: %s" %
                                  (sent, len(commands), command[:96]), flush=True)
                            pending = command
                            since_send.clear()
                            final_is_exit = (sent == len(commands) and
                                             command.strip() == "exit")
                            # The next command is gated on the *next* prompt,
                            # which cannot be the one that triggered this send.
                            waiting_for_prompt = True
                            if final_is_exit:
                                complete_at = time.monotonic() + 3.0
                        else:
                            # Queue drained: the final command's own exit
                            # receipt has just been collected above.
                            pending = None
                            complete_at = time.monotonic() + 1.0
                    elif len(rolling) > 2 * len(PROMPT):
                        del rolling[:-len(PROMPT)]

                if bad_run is not None:
                    break
                if early_exit:
                    break
                if proc.poll() is not None:
                    if sent < len(commands):
                        early_exit = True
                    break
    finally:
        selector.close()
        terminate_group(proc)

    if bad_run is not None:
        print("[prompt-qemu] stopping: %s" % bad_run, file=sys.stderr)
        print("[prompt-qemu] the guest build is not worth continuing past a "
              "failed command; see the serial log for the compiler's own "
              "diagnostic", file=sys.stderr)
        return 1
    if timed_out:
        print("[prompt-qemu] command queue did not finish before timeout "
              "(%d/%d)" % (sent, len(commands)), file=sys.stderr)
        return 1
    if early_exit:
        print("[prompt-qemu] QEMU exited before command queue completed "
              "(%d/%d)" % (sent, len(commands)), file=sys.stderr)
        return 1
    print("[prompt-qemu] complete (%d/%d commands sent%s)" %
          (sent, len(commands), "; final exit" if final_is_exit else ""),
          flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
