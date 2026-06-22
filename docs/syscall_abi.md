# AuraLite OS Syscall ABI

AuraLite uses the x86_64 `SYSCALL`/`SYSRET` mechanism with a Linux-like register
calling convention. The ABI is intentionally small and currently tailored to the
init shell, bundled user programs and networking demos.

## Register convention

| Register | Role |
|---|---|
| `RAX` | Syscall number on entry; return value on exit. |
| `RDI` | Argument 1. |
| `RSI` | Argument 2. |
| `RDX` | Argument 3. |
| `R10` | Argument 4. |
| `R8` | Argument 5. |
| `R9` | Argument 6. |
| `RCX` | Clobbered by CPU; contains user RIP during syscall entry. |
| `R11` | Clobbered by CPU; contains user RFLAGS during syscall entry. |

Return convention:

- non-negative value: success;
- `-1` (`0xFFFFFFFFFFFFFFFF`): generic failure.

AuraLite does not yet implement `errno`.

## User-space wrapper

`libc/src/syscall.asm` exposes:

```c
int64_t syscall(int64_t num,
                uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t a4, uint64_t a5, uint64_t a6);
```

The wrapper remaps the System V AMD64 C ABI to the syscall ABI:

```text
C ABI:       rdi=num rsi=a1 rdx=a2 rcx=a3 r8=a4 r9=a5 [rsp+8]=a6
SYSCALL ABI: rax=num rdi=a1 rsi=a2 rdx=a3 r10=a4 r8=a5 r9=a6
```

## MSR configuration

Configured by `kernel/arch/x86_64/syscall_entry.asm` through `syscall_init`.

| MSR | Purpose | Value/meaning |
|---|---|---|
| `STAR` | Segment selectors | syscall CS base `0x08`, sysret base `0x10`. |
| `LSTAR` | Kernel syscall RIP | Address of `syscall_entry`. |
| `SFMASK` | RFLAGS mask | Clears IF (`0x200`) on syscall entry. |
| `EFER` | Extended features | Sets SCE to enable `SYSCALL`. |

### GDT selector layout for SYSRET

`SYSRET` derives user selectors from `STAR[63:48]`:

```text
CS = STAR[63:48] + 0x10 | RPL3
SS = STAR[63:48] + 0x08 | RPL3
```

AuraLite sets `STAR[63:48] = 0x10`, therefore:

```text
CS = 0x23  # user code selector
SS = 0x1B  # user data selector
```

This is why the GDT stores user data before user code.

`syscall_entry.asm` uses `o64 sysret`, not plain `sysret`, so the 64-bit SYSRET
selector formula is used.

## Stack handling

`SYSCALL` does **not** switch stacks. In the current implementation, the syscall
handler initially runs on the user's `RSP`; `syscall_dispatch()` is called from
that context.

Interrupts use the TSS/RSP0 path when transitioning from Ring 3 to Ring 0, so
timer IRQs and exceptions do not reuse the user stack in the same way.

Current caveats:

- saved `RCX`, `R11` and user `RSP` are stored in globals, so this path is not
  suitable for true SMP syscall concurrency yet;
- user pointers are not validated before the kernel dereferences them;
- blocking syscalls are mostly polling/spin-based.

## Syscall table

| Number | Name | Prototype | Status | Notes |
|---:|---|---|---|---|
| 0 | `read` | `read(fd, buf, count)` | ✅ | `fd=0` reads from serial stdin; `fd>=3` reads VFS files. |
| 1 | `write` | `write(fd, buf, count)` | ✅ | `fd=1/2` console; `fd>=3` VFS write, including `/tmp` tmpfs files. |
| 2 | `open` | `open(path)` | ✅ | Opens VFS path and returns global FD. |
| 3 | `close` | `close(fd)` | ✅ | Closes global FD. |
| 39 | `getpid` | `getpid()` | ✅ | Returns current TCB ID. |
| 57 | `fork` | `fork()` | 🧪 | Deep-copies user address space; simplified semantics. |
| 59 | `execve` | `execve(path)` | 🧪 | Replaces current address space with a new ELF. No argv/envp. |
| 60 | `exit` | `exit(code)` | ✅/🧪 | Terminates current thread; exit code handling is incomplete. |
| 61 | `wait4` | `wait4(status)` | 🧪 | Yield-polling wait; not POSIX-complete. |
| 80 | `listdir` | `listdir(path)` | 🧪 | Non-standard; currently prints through kernel/VFS path. |
| 81 | `spawn` | `spawn(path)` | 🧪 | Non-standard; creates process and loads ELF from VFS. |
| 82 | `dns` | `dns_resolve(hostname)` | 🧪 | Returns IPv4 A record in host-order integer form. |
| 83 | `net_connect` | `net_connect(ip, port)` | 🧪 | Opens the single global TCP client connection. |
| 84 | `net_send` | `net_send(buf, len)` | 🧪 | Sends on global TCP connection. |
| 85 | `net_recv` | `net_recv(buf, len)` | 🧪 | Polling receive on global TCP connection. |
| 86 | `net_close` | `net_close()` | 🧪 | Closes global TCP connection. |
| 87 | `net_ping` | `net_ping(ip)` | 🧪 | ICMP echo via kernel networking stack. |

## libc wrappers

Defined in `libc/include/unistd.h` and implemented in `libc/src/libc.c`:

```c
ssize_t write(int fd, const void *buf, size_t count);
ssize_t read(int fd, void *buf, size_t count);
int     open(const char *path);
int     close(int fd);
void    _exit(int code);
pid_t   getpid(void);
pid_t   fork(void);
int     execve(const char *path);
pid_t   wait(int *status);
pid_t   spawn(const char *path);
void    listdir(const char *path);
uint32_t dns_resolve(const char *hostname);
int     net_connect(uint32_t ip, uint16_t port);
int     net_send(const void *data, uint32_t len);
int     net_recv(void *buf, uint32_t bufsize);
int     net_close(void);
int     net_ping(uint32_t ip);
```

## File descriptor model

File descriptors are currently global kernel objects, not per-process objects.
This means:

- unrelated processes share the same FD namespace;
- there is no `dup`, `pipe`, close-on-exec or per-process descriptor lifetime;
- future work should move the FD table into a process structure.

## Planned or missing syscalls

| Name | Purpose |
|---|---|
| `mmap`, `munmap` | User memory mappings. |
| `brk` | User heap growth. |
| `pipe` | IPC pipe. |
| `socket`, `bind`, `connect`, `listen`, `accept`, `send`, `recv` | BSD-style socket API. |
| `readdir` | Structured directory iteration. |
| `stat` | File metadata. |
| `nanosleep` / `clock_gettime` | Time APIs. |

## Security / robustness TODOs

Before treating the syscall layer as robust, add:

1. canonical-address checks for all user pointers;
2. page-table permission checks before copying to/from user buffers;
3. `copy_from_user` / `copy_to_user` helpers;
4. per-process syscall state instead of global saved `RCX/R11/RSP`;
5. structured error codes (`errno`-style or negative error numbers).
