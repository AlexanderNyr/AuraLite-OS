# AuraLite OS Syscall ABI

## Convention (Linux-compatible)

System calls use the fast `SYSCALL`/`SYSRET` mechanism (no IDT involvement).
Arguments are passed in registers:

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
| `R11`    | destroyed (holds user RFLAGS for SYSRET)  |

Return value: a non-negative value on success; `-1` (i.e. `0xFFFFFFFFFFFFFFFF`)
on error. Per-syscall `errno` decoding is not implemented yet.

## MSR configuration (set in `syscall_init`)

| MSR      | Value / Role                                        |
|----------|-----------------------------------------------------|
| `STAR`   | `[47:32]=0x08` (SYSCALL CS), `[63:48]=0x08` (SYSRET base) |
| `LSTAR`  | address of `syscall_entry` (full 64-bit)            |
| `SFMASK` | `0x200` — clears IF on entry                        |
| `EFER`   | bit 0 (SCE) set to enable SYSCALL/SYSRET            |

SYSRET uses the **64-bit operand size** form (`o64 sysret`, opcode `48 0F 07`)
so `CS = (STAR[63:48] + 0x10) | RPL3 = 0x1B` and `SS = STAR[63:48] + 8 | RPL3`.

## Stack handling

SYSCALL does **not** switch stacks. `syscall_entry` manually saves the user RSP
and loads a kernel stack (set per-thread via `set_syscall_stack()`) before
calling the C dispatch, then restores the user RSP before SYSRET. This prevents
the kernel handler from corrupting the user's stack.

## Implemented syscalls

| Number | Name     | Signature                                   | Notes                       |
|--------|----------|---------------------------------------------|-----------------------------|
| 0      | `read`   | `read(fd, buf, count)`                      | Returns 0 (EOF); no input device yet |
| 1      | `write`  | `write(fd, buf, count)`                     | Only fd=1 (stdout) supported |
| 39     | `getpid` | `getpid()`                                  | Returns the current thread ID |
| 60     | `exit`   | `exit(code)`                                | Terminates the calling thread |

## User-space interface

User programs link against the minimal libc (`libc/`) which provides:

- `libc/src/syscall.asm` — generic `syscall(num, a1..a6)` wrapper that remaps
  the C ABI (rdi, rsi, rdx, rcx, r8, r9) to the SYSCALL ABI (rax, rdi, rsi,
  rdx, r10, r8, r9).
- `libc/src/libc.c` — POSIX-style wrappers: `write`, `read`, `_exit`, `getpid`.
- `libc/crt/crt0.asm` — `_start` entry: clears RBP, calls `main`, exits.

## Planned syscalls (Phases 10–11)

| Number | Name     | Phase | Purpose                          |
|--------|----------|-------|----------------------------------|
| 2      | `open`   | 10    | Open a file from the VFS         |
| 3      | `close`  | 10    | Close a file descriptor          |
| 9      | `mmap`   | —     | Map memory                       |
| 11     | `munmap` | —     | Unmap memory                     |
| 12     | `brk`    | —     | Adjust program break             |
| 57     | `fork`   | 11    | Fork the process                 |
| 59     | `execve` | 11    | Execute a program                |
| 61     | `wait4`  | 11    | Wait for a child                 |
| 22     | `pipe`   | 11    | Create a pipe                    |
