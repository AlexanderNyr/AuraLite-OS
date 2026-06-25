# AuraLite OS Syscall ABI

AuraLite uses the x86_64 `SYSCALL`/`SYSRET` mechanism with a Linux-like register
calling convention. The ABI is intentionally small and currently tailored to the
init shell, bundled user programs, networking demos, filesystem tests and GUI
applications.

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

AuraLite does not yet implement `errno`; user-space wrappers generally expose
`-1` as failure.

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
- syscall handlers use `validate_user_range`, `copy_from_user` and
  `copy_to_user`, but this is not yet a fault-recovering uaccess layer;
- blocking syscalls are mostly polling/spin-based.

## Syscall table

| Number | Name | Prototype | Status | Notes |
|---:|---|---|---|---|
| 0 | `read` | `read(fd, buf, count)` | ✅ | `fd=0` reads line input from PS/2 keyboard and/or serial; `fd>=3` reads VFS files. |
| 1 | `write` | `write(fd, buf, count)` | ✅ | `fd=1/2` console; `fd>=3` VFS write. |
| 2 | `open` | `open(path)` | ✅ | Opens or creates a VFS path when the mounted FS supports creation; returns a per-process FD. |
| 3 | `close` | `close(fd)` | ✅ | Closes a per-process FD. |
| 12 | `brk` | `sbrk(increment)` | ✅ | Adjusts the program break (heap). |
| 39 | `getpid` | `getpid()` | ✅ | Returns current TCB ID. |
| 57 | `fork` | `fork()` | 🧪 | Deep-copies user address space; simplified semantics. |
| 59 | `execve` | `execve(path)` | 🧪 | Replaces current address space with a new ELF. No argv/envp. |
| 60 | `exit` | `exit(code)` | ✅/🧪 | Terminates current thread; exit-code reporting is incomplete. |
| 61 | `wait4` | `wait4(status)` | 🧪 | Yield-polling wait; not POSIX-complete and not PID-specific. |
| 80 | `listdir` | `readdir(path, out, max)` | 🧪 | Returns or prints a directory listing through the kernel/VFS path. |
| 81 | `spawn` | `spawn(path)` | 🧪 | Non-standard; creates a process and loads an ELF from VFS. |
| 82 | `dns` | `dns_resolve(hostname)` | 🧪 | Returns IPv4 A record in host-order integer form. |
| 83 | `net_connect` | `net_connect(ip, port)` | 🧪 | Opens the single global TCP client connection. |
| 84 | `net_send` | `net_send(buf, len)` | 🧪 | Sends on the global TCP connection. |
| 85 | `net_recv` | `net_recv(buf, len)` | 🧪 | Polling receive on the global TCP connection. |
| 86 | `net_close` | `net_close()` | 🧪 | Closes the global TCP connection. |
| 87 | `net_ping` | `net_ping(ip)` | 🧪 | Legacy ICMP echo via kernel networking stack. |
| 100 | `mkdir` | `mkdir(path)` | ✅/🧪 | Creates a directory when the mounted FS supports it (`/fat`, `/ext2`). |
| 101 | `rmdir` | `rmdir(path)` | ✅/🧪 | Removes an empty directory. |
| 102 | `unlink` | `unlink(path)` | ✅/🧪 | Removes a regular file. |
| 103 | `rename` | `rename(from, to)` | ✅/🧪 | Renames/moves within supported filesystems. |
| 104 | `truncate` | `truncate(path, size)` | ✅/🧪 | Shrinks or extends supported regular files. |
| 105 | `stat` | `stat(path, struct stat*)` | ✅/🧪 | Fills the AuraLite `struct stat` subset. |
| 200 | `gui_call` | packed GUI dispatcher | 🧪 | Window lifecycle, drawing, invalidation, render and cursor operations. Used through `libauragui`. |
| 201 | `gui_event` | `gui_event(wid, out, block)` | 🧪 | Polls or waits for GUI events for a window. Used through `libauragui`. |
| 300 | `socket` | `socket(domain, type, protocol)` | 🧪 | Creates a process-owned AF_INET/SOCK_STREAM socket handle. |
| 301 | `socket_connect` | `connect(sock, ip, port)` | 🧪 | Connects a socket to an IPv4 endpoint. |
| 302 | `socket_send` | `send(sock, buf, len)` | 🧪 | Sends bytes on a connected socket with user-copy validation. |
| 303 | `socket_recv` | `recv(sock, buf, len)` | 🧪 | Receives bytes from a connected socket. |
| 304 | `socket_close` | `closesocket(sock)` | 🧪 | Closes a process-owned socket and underlying TCP stream if active. |

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
int     socket(int domain, int type, int protocol);
int     connect(int sock, uint32_t ip, uint16_t port);
int     send(int sock, const void *data, uint32_t len);
int     recv(int sock, void *buf, uint32_t bufsize);
int     closesocket(int sock);
int     mkdir(const char *path);
int     rmdir(const char *path);
int     unlink(const char *path);
int     rename(const char *from, const char *to);
int     truncate(const char *path, uint64_t new_size);
int     stat(const char *path, struct stat *out);
```

GUI wrappers are defined in `libauragui/include/auragui.h` and implemented in
`libauragui/src/auragui.c`. Applications should use the `ag_*` window, drawing,
event and widget APIs rather than invoking `SYS_GUI_CALL` directly.

## File descriptor model

File descriptors are stored in each `tcb_t` / process.  FD numbers are therefore
local to the current process; unrelated processes can both use fd `3` without
colliding.  `fork()` currently shallow-copies the parent's table into the child,
so both processes initially refer to the same vnodes with copied offsets.  New
`spawn()`ed processes start with an empty table, while fd `0/1/2` remain special
syscall-level stdin/stdout/stderr handles rather than VFS entries.

Current caveats:

- no `dup`, `pipe` or close-on-exec;
- `fork()` does not yet model POSIX shared open-file descriptions precisely;
- process-exit cleanup now closes process FDs and deferred-reaps TCBs/stacks,
  but full page-table/address-space reaping is still future work.

## Planned or missing syscalls

| Name | Purpose |
|---|---|
| `mmap`, `munmap` | User memory mappings. |
| `pipe` | IPC pipe. |
| `bind`, `listen`, `accept` | Server-side socket API. |
| full BSD `sockaddr` ABI | Current socket calls pass IPv4/port directly. |
| `nanosleep` / `clock_gettime` | Time APIs. |

## Security / robustness TODOs

Before treating the syscall layer as robust, add:

1. fault-recovering user access so a race/unmap cannot still fault the kernel;
2. a full audit of any remaining direct user-pointer paths outside syscall dispatch;
3. per-process syscall state instead of global saved `RCX/R11/RSP`;
4. structured error codes (`errno`-style or negative error numbers).
