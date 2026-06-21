# AuraLite OS Syscall ABI

## Convention (Linux-compatible)

System calls use the fast `SYSCALL`/`SYSRET` mechanism. Arguments are passed
in registers:

| Register | Role                                       |
|----------|--------------------------------------------|
| `RAX`    | syscall number (in) / return value (out)   |
| `RDI`    | arg 1                                      |
| `RSI`    | arg 2                                      |
| `RDX`    | arg 3                                      |
| `R10`    | arg 4                                      |
| `R8`     | arg 5                                      |
| `R9`     | arg 6                                      |
| `RCX`    | destroyed (holds user RIP for SYSRET)      |
| `R11`    | destroyed (holds user RFLAGS for SYSRET)   |

Return value: a non-negative value on success; `-1`
(`0xFFFFFFFFFFFFFFFF`) on error.

## MSR configuration (set in `syscall_init`)

| MSR      | Value                                              |
|----------|----------------------------------------------------|
| `STAR`   | `[47:32]=0x08` (SYSCALL CS), `[63:48]=0x10` (SYSRET base) |
| `LSTAR`  | full 64-bit address of `syscall_entry`             |
| `SFMASK` | `0x200` — clears IF on entry                       |
| `EFER`   | bit 0 (SCE) set to enable SYSCALL/SYSRET           |

### Why STAR[63:48] = 0x10

SYSRET loads `CS = STAR[63:48] + 0x10` and `SS = STAR[63:48] + 0x08`.
With `STAR[63:48] = 0x10`:

- `CS = 0x10 + 0x10 = 0x20 | RPL3 = 0x23` — our user code segment (GDT index 4)
- `SS = 0x10 + 0x08 = 0x18 | RPL3 = 0x1B` — our user data segment (GDT index 3)

This is why the GDT has user **data** at index 3 (0x18) and user **code** at
index 4 (0x20) — the reverse of the conventional layout. Getting this wrong
causes a #GP because SYSRET would load SS from a DPL-0 (kernel) segment.

SYSRET uses the **64-bit operand size** form (`o64 sysret`, opcode `48 0F 07`).
NASM's plain `sysret` generates `0F 07` (32-bit), which sets `CS = STAR[63:48]`
*without* the +0x10 offset — producing a wrong selector.

## Stack handling

SYSCALL does **not** switch stacks — the handler runs on the user's RSP. This
is safe because:

- The user stack is mapped writable + user-accessible.
- Timer/IRQ interrupts switch to the TSS.RSP0 kernel stack (a different stack),
  so there is no conflict.
- `iretq` from the timer handler restores the user RSP.

The dispatch function (`syscall_dispatch`) runs on the user stack directly.
RCX/R11 (user RIP/RFLAGS) are saved to global variables before the C call and
restored before SYSRET.

## Implemented syscalls

| Number | Name       | Signature                                   | Notes                              |
|--------|------------|---------------------------------------------|------------------------------------|
| 0      | `read`     | `read(fd, buf, count)` → `ssize_t`          | fd=0: serial line input (polling UART with sched_yield, CR→LF, echo). fd≥3: VFS read. |
| 1      | `write`    | `write(fd, buf, count)` → `ssize_t`         | fd=1 (stdout) and fd=2 (stderr)    |
| 2      | `open`     | `open(path)` → `int`                        | Opens a file from the VFS          |
| 3      | `close`    | `close(fd)` → `int`                         | Closes a file descriptor           |
| 39     | `getpid`   | `getpid()` → `pid_t`                        | Returns the current thread ID      |
| 57     | `fork`     | `fork()` → `pid_t`                          | Clone address space + TCB          |
| 59     | `execve`   | `execve(path)` → `int`                      | Replace address space with new ELF |
| 60     | `exit`     | `exit(code)` → `noreturn`                   | Terminates the calling thread      |
| 61     | `wait4`    | `wait4(status)` → `pid_t`                   | Wait for a child to exit           |
| 80     | `listdir`  | `listdir(path)`                             | Non-standard: lists a directory    |
| 81     | `spawn`    | `spawn(path)` → `pid_t`                     | Non-standard: new process + address space |
| 82     | `dns`      | `dns_resolve(hostname)` → `uint32_t`        | Non-standard: DNS A-record lookup  |

## User-space libc interface

User programs link against the minimal libc (`libc/`) which provides:

- **`libc/src/syscall.asm`** — generic `syscall(num, a1..a6)` wrapper that
  remaps the C ABI (rdi, rsi, rdx, rcx, r8, r9) to the SYSCALL ABI
  (rax, rdi, rsi, rdx, r10, r8, r9).
- **`libc/src/libc.c`** — POSIX-style wrappers (`write`, `read`, `open`,
  `close`, `_exit`, `getpid`, `listdir`) plus string functions (`strlen`,
  `strcmp`, `strtok`, `strcpy`, `memset`, `memcpy`, `memcmp`) and `printf`.
- **`libc/crt/crt0.asm`** — `_start` entry: clears RBP, calls `main`, calls
  `_exit` with the return value.

## Syscall entry code path

```
user:  mov rax, num; mov rdi..r9, args; syscall
            │
            ▼  (CPU: CS←0x08, SS←0x10, RIP←LSTAR, RFLAGS←(R11 & ~SFMASK))
kernel: syscall_entry (syscall_entry.asm)
            │  save RCX, R11
            │  push rdi/rsi/rdx ; remap to C ABI (rdi=num, rsi=a1, rdx=a2, rcx=a3)
            │  call syscall_dispatch
            │  add rsp, 24
            │  restore RCX, R11
            │  o64 sysret
            ▼
user:  RAX = return value
```

## Planned syscalls (not yet implemented)

| Number | Name     | Purpose                          |
|--------|----------|----------------------------------|
| 9      | `mmap`   | Map memory into user space       |
| 11     | `munmap` | Unmap memory                     |
| 12     | `brk`    | Adjust program break             |
| 57     | `fork`   | Fork the process (needs COW)     |
| 59     | `execve` | Execute a new program            |
| 61     | `wait4`  | Wait for a child process         |
| 22     | `pipe`   | Create a pipe                    |
