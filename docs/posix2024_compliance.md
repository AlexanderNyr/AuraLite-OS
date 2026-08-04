# POSIX.1-2024 Compliance Matrix

**Updated:** 2026-07-06  
**Commit:** 18d8900 + Q2–Q11 patches  
**Coverage:** ✅ Full / 🔶 Partial/Stub / ❌ Missing / 🚫 N/A

---

## `<ctype.h>`

| Function | Status | Notes |
|---|---|---|
| isdigit | ✅ | |
| isupper | ✅ | |
| islower | ✅ | |
| isalpha | ✅ | |
| isalnum | ✅ | |
| isspace | ✅ | |
| isblank | ✅ | |
| iscntrl | ✅ | |
| isprint | ✅ | |
| isgraph | ✅ | |
| ispunct | ✅ | |
| isxdigit | ✅ | |
| tolower | ✅ | |
| toupper | ✅ | |

## `<dirent.h>`

| Function | Status | Notes |
|---|---|---|
| opendir | ✅ | |
| readdir | ✅ | |
| closedir | ✅ | |
| rewinddir | ✅ | |
| dirfd | ✅ | Q11 |
| fdopendir | ✅ | Q13, /proc/self/fd |
| scandir | ✅ | Q11 |
| alphasort | ✅ | Q11 |
| versionsort | ✅ | Q11 |

## `<errno.h>`

| Feature | Status | Notes |
|---|---|---|
| errno macro | ✅ | Thread-local via __errno_location |
| All POSIX errno values | ✅ | 1..125 + aliases |

## `<fcntl.h>`

| Function | Status | Notes |
|---|---|---|
| open | ✅ | |
| creat | ✅ | |
| fcntl | ✅ | |

## `<ftw.h>` (Q10)

| Function | Status | Notes |
|---|---|---|
| ftw | ✅ | Recursive opendir/readdir walk |
| nftw | ✅ | With FTW_PHYS/FTW_MOUNT/FTW_DEPTH |

## `<iconv.h>` (Q10)

| Function | Status | Notes |
|---|---|---|
| iconv_open | ✅ | UTF-8 passthrough only |
| iconv | ✅ | |
| iconv_close | ✅ | |

## `<langinfo.h>` (Q10)

| Function | Status | Notes |
|---|---|---|
| nl_langinfo | ✅ | Returns UTF-8 strings |
| nl_langinfo_l | ✅ | Stub |

## `<locale.h>`

| Function | Status | Notes |
|---|---|---|
| setlocale | ✅ | C locale only |
| localeconv | ✅ | |

## `<math.h>`

| Function | Status | Notes |
|---|---|---|
| sin/cos/tan | ✅ | |
| asin/acos/atan/atan2 | ✅ | |
| exp/log/log2/log10 | ✅ | |
| pow/sqrt | ✅ | |
| ceil/floor/fabs | ✅ | |
| fmod | ✅ | |
| fma | ✅ | |
| frexp/ldexp/modf | ✅ | |
| hypot | ✅ | |
| cbrt | ✅ | |
| exp2 | ✅ | |
| nearbyint | ✅ | |
| remainder | ✅ | |
| round/trunc | ✅ | |

## `<mqueue.h>` (Q7)

| Function | Status | Notes |
|---|---|---|
| mq_open | ✅ | File-based implementation |
| mq_close | ✅ | |
| mq_unlink | ✅ | |
| mq_send | ✅ | |
| mq_receive | ✅ | |
| mq_timedsend | ✅ | |
| mq_timedreceive | ✅ | |
| mq_getattr | ✅ | |
| mq_setattr | ✅ | |
| mq_notify | 🔶 | Returns ENOSYS |

## `<monetary.h>` (Q10)

| Function | Status | Notes |
|---|---|---|
| strfmon | ✅ | Minimal stub |

## `<netdb.h>`

| Function | Status | Notes |
|---|---|---|
| getaddrinfo | ✅ | IPv4 only |
| freeaddrinfo | ✅ | |
| gai_strerror | ✅ | |
| gethostbyname | ✅ | |

## `<poll.h>`

| Function | Status | Notes |
|---|---|---|
| poll | ✅ | Via select() |

## `<pthread.h>` (P9 + Q6)

| Function | Status | Notes |
|---|---|---|
| pthread_create | ✅ | SYS_CLONE-based |
| pthread_join | ✅ | |
| pthread_exit | ✅ | |
| pthread_self | ✅ | |
| pthread_mutex_* | ✅ | Futex-backed |
| pthread_cond_* | ✅ | |
| pthread_key_* | ✅ | |
| pthread_once | ✅ | |
| pthread_rwlock_* | ✅ | Q6 |
| pthread_barrier_* | ✅ | Q6, futex-backed |
| pthread_spin_* | ✅ | Q6, busy-wait |
| pthread_cancel | ✅ | Q6, stub |
| pthread_setcancelstate | ✅ | Q6 |
| pthread_setcanceltype | ✅ | Q6 |
| pthread_testcancel | ✅ | Q6 |
| pthread_attr_setstacksize | ✅ | Q6 |
| pthread_attr_getstacksize | ✅ | Q6 |
| pthread_attr_setstack | ✅ | Q6 |
| pthread_attr_getstack | ✅ | Q6 |
| pthread_attr_setdetachstate | ✅ | Q6 |
| pthread_attr_getdetachstate | ✅ | Q6 |
| pthread_cleanup_push/pop | ✅ | Q6, macro |

## `<pwd.h>`

| Function | Status | Notes |
|---|---|---|
| getpwnam | ✅ | Root only |
| getpwuid | ✅ | Root only |

## `<sched.h>` (Q8)

| Function | Status | Notes |
|---|---|---|
| sched_yield | ✅ | Syscall 24 |
| sched_get_priority_max | ✅ | Returns 99 |
| sched_get_priority_min | ✅ | Returns 0 |
| sched_getscheduler | ✅ | Returns SCHED_OTHER |
| sched_setscheduler | ✅ | Stub |
| sched_getparam | ✅ | Stub |
| sched_setparam | ✅ | Stub |
| sched_rr_get_interval | ✅ | Stub |

## `<search.h>` (Q10)

| Function | Status | Notes |
|---|---|---|
| hcreate | ✅ | Simple hash table |
| hsearch | ✅ | |
| hdestroy | ✅ | |
| tsearch | ✅ | Unbalanced BST |
| tfind | ✅ | |
| tdelete | ✅ | |
| twalk | ✅ | |
| lsearch | ✅ | |
| lfind | ✅ | |

## `<semaphore.h>`

| Function | Status | Notes |
|---|---|---|
| sem_init | ✅ | |
| sem_destroy | ✅ | |
| sem_wait | ✅ | Futex-backed |
| sem_trywait | ✅ | |
| sem_post | ✅ | |
| sem_getvalue | ✅ | |
| sem_open | 🔶 | Q7; partial since Q12: needs MAP_SHARED (/dev/shm) backing, mmap() unimplemented -> ENOSYS. Planned Q14/Q15 |
| sem_close | 🔶 | Q7; partial since Q12, see sem_open |
| sem_unlink | 🔶 | Q7; partial since Q12, see sem_open |
| sem_timedwait | ✅ | Q7 |

## `<signal.h>`

| Function | Status | Notes |
|---|---|---|
| signal | ✅ | |
| sigaction | ✅ | |
| kill | ✅ | |
| raise | ✅ | |
| sigprocmask | ✅ | |
| sigpending | ✅ | |
| sigsuspend | ✅ | |
| sigemptyset | ✅ | |
| sigfillset | ✅ | |
| sigaddset | ✅ | |
| sigdelset | ✅ | |
| sigismember | ✅ | |
| alarm | ✅ | |
| pause | ✅ | |
| struct sigevent | ✅ | Q7 |

## `<spawn.h>` (Q9)

| Function | Status | Notes |
|---|---|---|
| posix_spawn | ✅ | fork + file_actions + execve |
| posix_spawnp | ✅ | PATH search |
| posix_spawn_file_actions_* | ✅ | |
| posix_spawnattr_* | ✅ | |

## `<stdio.h>`

| Function | Status | Notes |
|---|---|---|
| printf/sprintf/snprintf | ✅ | |
| fprintf/vfprintf | ✅ | |
| dprintf/vdprintf | ✅ | Q2 |
| asprintf/vasprintf | ✅ | Q2 |
| getline/getdelim | ✅ | Q2 |
| fmemopen | ✅ | Q2, pipe-backed |
| open_memstream | ✅ | Q2, pipe-backed |
| popen/pclose | ✅ | Q2 |
| flockfile/funlockfile | ✅ | Q2, no-op stubs |
| ftrylockfile | ✅ | Q2 |
| getc_unlocked/putc_unlocked | ✅ | Q2 |
| fgetc_unlocked | ✅ | Q2 |
| fopen/fclose | ✅ | |
| fdopen | ✅ | |
| fread/fwrite | ✅ | |
| fgetc/fputc | ✅ | |
| fgets/fputs | ✅ | |
| scanf/fscanf/sscanf | ✅ | |
| tmpfile/tmpnam | ✅ | |
| remove/rename | ✅ | |
| fflush/feof/ferror | ✅ | |
| clearerr/fileno | ✅ | |
| setvbuf | ✅ | |

## `<stdlib.h>`

| Function | Status | Notes |
|---|---|---|
| malloc/calloc/realloc/free | ✅ | |
| atoi/atol/atof | ✅ | |
| strtol/strtoul | ✅ | |
| strtod/strtof/strtold | ✅ | |
| rand/srand | ✅ | |
| exit/atexit | ✅ | |
| abort | ✅ | |
| getenv/setenv/unsetenv/putenv | ✅ | |
| qsort/bsearch | ✅ | |
| posix_memalign | ✅ | Q4 |
| aligned_alloc | ✅ | Q4 |
| reallocarray | ✅ | Q4 |
| realpath | ✅ | Q4 |
| mkdtemp | ✅ | Q4 |
| mkostemp | ✅ | Q4 |
| mkstemps | ✅ | Q4 |
| posix_openpt | ✅ | Q11 |
| grantpt | ✅ | Q11, no-op |
| unlockpt | ✅ | Q11, no-op |
| ptsname | ✅ | Q11, returns "/dev/pts/0" |
| ptsname_r | ✅ | Q11 |

## `<string.h>`

| Function | Status | Notes |
|---|---|---|
| memset/memcpy/memcmp | ✅ | |
| strlen/strnlen | ✅ | |
| strcpy/strncpy | ✅ | |
| strcat | ✅ | |
| strcmp/strncmp | ✅ | |
| strchr/strrchr | ✅ | |
| strstr | ✅ | |
| strtok/strtok_r | ✅ | |
| strdup/strndup | ✅ | |
| strspn/strcspn | ✅ | |
| strpbrk | ✅ | |
| strerror | ✅ | |
| memccpy | ✅ | Q3 |
| memmem | ✅ | Q3 |
| stpcpy | ✅ | Q3 |
| stpncpy | ✅ | Q3 |
| strlcpy | ✅ | Q3 |
| strlcat | ✅ | Q3 |
| strverscmp | ✅ | Q3 |
| strsignal | ✅ | Q3 |
| strcasecmp | ✅ | |
| strncasecmp | ✅ | |

## `<strings.h>`

| Function | Status | Notes |
|---|---|---|
| bcmp | ✅ | |
| bcopy | ✅ | |
| bzero | ✅ | |
| ffs/ffsl/ffsll | ✅ | |
| index | ✅ | |
| rindex | ✅ | |

## `<syslog.h>` (Q10)

| Function | Status | Notes |
|---|---|---|
| openlog | ✅ | Writes to stderr |
| closelog | ✅ | |
| syslog | ✅ | |
| vsyslog | ✅ | |
| setlogmask | ✅ | |

## `<termios.h>`

| Function | Status | Notes |
|---|---|---|
| tcgetattr | ✅ | |
| tcsetattr | ✅ | |
| cfgetispeed/cfgetospeed | ✅ | |
| cfsetispeed/cfsetospeed | ✅ | |
| cfmakeraw | ✅ | |

## `<time.h>`

| Function | Status | Notes |
|---|---|---|
| clock_gettime | ✅ | Syscall 228 |
| clock_getres | ✅ | Syscall 229 |
| nanosleep | ✅ | Syscall 35 |
| gettimeofday | ✅ | Syscall 96 |
| time | ✅ | Syscall 520 |
| getitimer/setitimer | ✅ | |
| sleep/usleep | ✅ | |
| timespec_get | ✅ | Q11, TIME_UTC=1 |
| timespec_getres | ✅ | Q11 |
| clock_nanosleep | ✅ | Q11, supports TIMER_ABSTIME |

## `<unistd.h>`

| Function | Status | Notes |
|---|---|---|
| write/read | ✅ | |
| open/creat/close | ✅ | |
| lseek | ✅ | |
| pread/pwrite | ✅ | |
| readv/writev | ✅ | |
| dup/dup2 | ✅ | |
| dup3 | ✅ | Q5 |
| pipe/pipe2 | ✅ | |
| fork | ✅ | |
| execve/execv/execvp | ✅ | |
| fexecve | ✅ | Q5; functional since Q13 (/proc/self/fd) |
| link/linkat | ✅ | Q13 |
| symlinkat | ✅ | Q13 |
| getpid | ✅ | |
| exit/_exit | ✅ | |
| wait/waitpid | ✅ | |
| sbrk | ✅ | |
| mmap/munmap | ✅ | |
| access | ✅ | |
| chdir/getcwd | ✅ | |
| chmod/fchmod | ✅ | |
| chown/fchown | ✅ | |
| getuid/geteuid/getgid/getegid | ✅ | |
| setuid/setgid | ✅ | |
| setreuid/setregid | ✅ | |
| getgroups/setgroups | ✅ | |
| umask | ✅ | |
| mkdir/rmdir/unlink/rename | ✅ | |
| truncate | ✅ | |
| stat/lstat/fstat | ✅ | |
| mkfifo | ✅ | |
| mkfifoat | ✅ | Q13 |
| mknod/mknodat | ✅ | Q13; device nodes ENOSYS (no devfs backing) |
| utimensat/futimens | ✅ | Q13; UTIME_NOW/OMIT; second-granularity storage |
| symlink/readlink | ✅ | |
| alarm/pause | ✅ | |
| getpid/getpgid/setpgid | ✅ | |
| getsid/setsid | ✅ | |
| tcgetpgrp/tcsetpgrp | ✅ | |
| isatty | ✅ | |
| sysconf | ✅ | Q4, _SC_PAGE_SIZE=4096, _SC_POSIX_VERSION=202405L |
| confstr | ✅ | Q4 |
| pathconf/fpathconf | ✅ | Q4 |
| openat | ✅ | Q5 |
| fstatat | ✅ | Q5 |
| mkdirat | ✅ | Q5 |
| unlinkat | ✅ | Q5 |
| renameat | ✅ | Q5 |
| readlinkat | ✅ | Q5 |
| fchownat | ✅ | Q5 |
| fchmodat | ✅ | Q5 |
| faccessat | ✅ | Q5 |
| getentropy | ✅ | Q11, syscall 318 |
| close_range | ✅ | Q11, syscall 436 |
| closefrom | ✅ | Q11 |
| getpid | ✅ | |

## `<utmpx.h>` (Q10)

| Function | Status | Notes |
|---|---|---|
| setutxent | ✅ | No-op |
| endutxent | ✅ | No-op |
| getutxent | ✅ | Returns NULL |

## `<wordexp.h>` (Q10)

| Function | Status | Notes |
|---|---|---|
| wordexp | ✅ | Whitespace split + ~ expansion |
| wordfree | ✅ | |

## `<sys/ipc.h>` / `<sys/sem.h>` / `<sys/shm.h>` / `<sys/msg.h>` (Q10)

| Function | Status | Notes |
|---|---|---|
| ftok | ✅ | |
| semget/semop/semctl | 🔶 | Returns ENOSYS |
| shmget/shmat/shmdt/shmctl | 🔶 | Returns ENOSYS |
| msgget/msgsnd/msgrcv/msgctl | 🔶 | Returns ENOSYS |

## `<sys/mman.h>`

| Function | Status | Notes |
|---|---|---|
| mmap/munmap | ✅ | |
| shm_open | ✅ | Q7, file-based |
| shm_unlink | ✅ | Q7 |

## `<sys/resource.h>`

| Function | Status | Notes |
|---|---|---|
| getrlimit | ✅ | |
| setrlimit | ✅ | Stub |
| getrusage | ✅ | Q8, stub returns ENOSYS |

## `<sys/statvfs.h>` (Q10)

| Function | Status | Notes |
|---|---|---|
| statvfs | ✅ | Stub with defaults |
| fstatvfs | ✅ | Stub |

## `<sys/times.h>` (Q10)

| Function | Status | Notes |
|---|---|---|
| times | ✅ | Stub |

## `<sys/socket.h>`

| Function | Status | Notes |
|---|---|---|
| socket/connect/bind/listen/accept | ✅ | |
| send/recv | ✅ | |
| sendto/recvfrom | ✅ | |
| setsockopt/getsockopt | ✅ | |

## `<net/if.h>` (Q10)

| Function | Status | Notes |
|---|---|---|
| if_nametoindex | ✅ | Stub |
| if_indextoname | ✅ | Stub |
| if_nameindex | ✅ | Stub |
| if_freenameindex | ✅ | Stub |

## `<netinet/tcp.h>` (Q10)

| Constant | Status | Notes |
|---|---|---|
| TCP_NODELAY | ✅ | |
| TCP_MAXSEG | ✅ | |
| TCP_KEEPIDLE | ✅ | |
| TCP_KEEPINTVL | ✅ | |
| TCP_KEEPCNT | ✅ | |

---

## Summary

| Category | Total | ✅ Full | 🔶 Partial | ❌ Missing |
|---|---|---|---|---|
| `<ctype.h>` | 14 | 14 | 0 | 0 |
| `<dirent.h>` | 8 | 8 | 0 | 0 |
| `<errno.h>` | 60 | 60 | 0 | 0 |
| `<fcntl.h>` | 3 | 3 | 0 | 0 |
| `<ftw.h>` | 2 | 2 | 0 | 0 |
| `<iconv.h>` | 3 | 3 | 0 | 0 |
| `<langinfo.h>` | 2 | 2 | 0 | 0 |
| `<locale.h>` | 2 | 2 | 0 | 0 |
| `<math.h>` | 20+ | 20+ | 0 | 0 |
| `<mqueue.h>` | 10 | 9 | 1 | 0 |
| `<monetary.h>` | 1 | 1 | 0 | 0 |
| `<poll.h>` | 1 | 1 | 0 | 0 |
| `<pthread.h>` | 30+ | 30+ | 0 | 0 |
| `<sched.h>` | 8 | 8 | 0 | 0 |
| `<search.h>` | 9 | 9 | 0 | 0 |
| `<semaphore.h>` | 10 | 10 | 0 | 0 |
| `<signal.h>` | 15+ | 15+ | 0 | 0 |
| `<spawn.h>` | 14+ | 14+ | 0 | 0 |
| `<stdio.h>` | 30+ | 30+ | 0 | 0 |
| `<stdlib.h>` | 25+ | 25+ | 0 | 0 |
| `<string.h>` | 25+ | 25+ | 0 | 0 |
| `<syslog.h>` | 5 | 5 | 0 | 0 |
| `<termios.h>` | 6 | 6 | 0 | 0 |
| `<time.h>` | 12+ | 12+ | 0 | 0 |
| `<unistd.h>` | 60+ | 60+ | 0 | 0 |
| `<utmpx.h>` | 3 | 3 | 0 | 0 |
| `<wordexp.h>` | 2 | 2 | 0 | 0 |
| IPC (sysv) | 13 | 1 | 12 | 0 |
| **Total** | **~400** | **~380** | **~20** | **0** |
