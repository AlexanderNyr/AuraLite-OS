/* W32APP_PLAN.md W32A-12: WS2_32 WinSock, REAL over AuraLite's native socket
 * stack.  SPDX-License-Identifier: Apache-2.0
 *
 * Every function here is a thin adapter: it translates the Windows ABI
 * (ms_abi, Windows struct layouts, WSA* error codes) onto the libc socket
 * surface (socket/bind/listen/accept/connect/send/recv/... on syscalls
 * 300-307, getaddrinfo/inet_* on the DNS + parser layer).  The libc layer
 * does the real work; this file owns only the boundary.
 *
 * WHAT IS REAL: startup/version negotiation, per-thread last-error (shares
 * the TEB slot with GetLastError via w32_errno.c), byte order, the whole
 * socket verb set, select() with the Windows-vs-POSIX fd_set translation,
 * getpeername/getsockname (served from a per-fd endpoint cache this file
 * keeps, since the kernel exposes no query syscall), getaddrinfo/
 * freeaddrinfo/gethostbyname/gethostbyaddr/getnameinfo/getservbyname/
 * getservbyport/inet_ntop/inet_pton/inet_addr/inet_ntoa, address<->string
 * (WSAAddressToStringA/WSAStringToAddressA), WSAIoctl for the FIONBIO/
 * FIONREAD/SIO_KEEPALIVE_VALS control codes, and the readiness-notification
 * layer PuTTY/plink actually drive: WSAEventSelect + WSAEnumNetworkEvents
 * over a kernel32 event object, WSAAsyncSelect + window-message delivery
 * through the user32 message pump, and WSAWaitForMultipleEvents.
 *
 * HOW THE ASYNC LAYER WORKS (surface discovered from the pinned binaries,
 * build/w32bins/{putty,plink}.exe -- see w32/tests/W32A12.probe.log): the
 * native transport is blocking, so readiness is measured with a zero-timeout
 * kernel select() over the fds carrying a live registration.  A single pump
 * (ws2_pump) walks those fds; for an event registration it SetEvent()s the
 * caller's kernel32 handle, for an async registration it PostMessageW()s the
 * caller's window.  The pump is driven from WSAWaitForMultipleEvents and,
 * for the WSAAsyncSelect path, from the user32 GetMessage/PeekMessage loop
 * (registered via w32_user32_set_socket_pump at WSAStartup).  Non-blocking
 * sockets (FIONBIO, or implied by WSAEventSelect) are honoured for real:
 * recv/send gate on a zero-timeout select() and return WSAEWOULDBLOCK when
 * the fd is not ready.
 *
 * DOCUMENTED NON-GOALS (D1 honesty, mirrored in the phase receipt):
 *   - Overlapped I/O (WSAOVERLAPPED + IOCP, WSASend/WSARecv/WSAConnect,
 *     WSAGetOverlappedResult) is refused by name -- neither pinned binary
 *     resolves it, and completion ports have no substrate here.
 *   - FIONREAD reports 0 (the kernel exposes no bytes-available query); a
 *     non-blocking recv still reports readiness correctly via select().
 *   - Non-blocking connect() completes synchronously (the kernel connect has
 *     no async mode); FD_CONNECT is then delivered on the next pump/enum with
 *     iErrorCode 0, which is what an event/async consumer waits for.
 *   - shutdown(): accepted; the half-close is approximated by the FIN that
 *     closesocket() already emits.  No independent SD_SEND half-close.
 *   - setsockopt/getsockopt: the observed option set (SO_REUSEADDR/
 *     SO_KEEPALIVE/SO_BROADCAST/SO_*BUF/SO_LINGER/SO_*TIMEO/TCP_NODELAY)
 *     succeeds as a no-op (libc's setsockopt is itself a no-op); any other
 *     option refuses by number with WSAENOPROTOOPT.
 *   - WSAIoctl beyond FIONBIO/FIONREAD/SIO_KEEPALIVE_VALS refuses:
 *     SIO_GET_EXTENSION_FUNCTION_POINTER with WSAEOPNOTSUPP (that is the
 *     overlapped-extension probe), anything else with WSAEINVAL.
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "w32/ws2_32.h"
#include "w32/w32_errno.h"

/* Cross-module exports this file drives.  Declared locally (rather than by
 * pulling in kernel32.h + user32.h, whose shared typedefs would tangle) so
 * the boundary stays explicit; the signatures match those headers exactly
 * and the linker resolves them against the same w32 archive. */
extern W32ABI void    *CreateEventW(void *sec, int manual, int initial,
                                    const void *name);
extern W32ABI int      SetEvent(void *h);
extern W32ABI int      ResetEvent(void *h);
extern W32ABI int      CloseHandle(void *h);
extern W32ABI uint32_t WaitForMultipleObjects(uint32_t n, void **hs,
                                              int wait_all, uint32_t ms);
extern W32ABI int      PostMessageW(void *hwnd, uint32_t msg,
                                    uint64_t wp, int64_t lp);
extern W32ABI void     w32_user32_set_socket_pump(void (W32ABI *fn)(void));

/* Wait-return / timeout constants (Win32 values; not in ws2_32.h). */
#define WS2_INFINITE          0xFFFFFFFFu
#define WS2_WAIT_TIMEOUT      0x00000102u
#define WS2_WAIT_FAILED       0xFFFFFFFFu

/* FD_* network-event bits and the WSANETWORKEVENTS.iErrorCode fan-out. */
#define WS2_FD_READ     0x01
#define WS2_FD_WRITE    0x02
#define WS2_FD_OOB      0x04
#define WS2_FD_ACCEPT   0x08
#define WS2_FD_CONNECT  0x10
#define WS2_FD_CLOSE    0x20
#define WS2_FD_MAX_EVENTS 10
#define WS2_FD_READ_BIT     0
#define WS2_FD_WRITE_BIT    1
#define WS2_FD_ACCEPT_BIT   3
#define WS2_FD_CONNECT_BIT  4
#define WS2_FD_CLOSE_BIT    5

/* WSANETWORKEVENTS, Win64 layout: the bitmask then a fixed error array. */
struct ws2_networkevents {
    long lNetworkEvents;
    int  iErrorCode[WS2_FD_MAX_EVENTS];
};

#ifndef W32_MAKELONG
#define W32_MAKELONG(lo, hi) ((int64_t)(((uint32_t)(uint16_t)(lo)) | \
                              ((uint32_t)(uint16_t)(hi) << 16)))
#endif

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <unistd.h>     /* socket/connect/connectaddr/send/recv/closesocket */

/* ---- process-wide startup refcount ------------------------------------ */

static int ws2_started;   /* >0 once WSAStartup has been called */

/* ---- per-fd endpoint cache (serves getsockname/getpeername) ----------- */

#define WS2_MAXFD 64
static struct ws2_endpoint {
    unsigned char      have_local;
    unsigned char      have_peer;
    unsigned char      nonblocking;   /* FIONBIO, or implied by EventSelect */
    unsigned char      is_listener;   /* set by listen(): read-ready == accept */
    unsigned char      connect_pending; /* connect done, FD_CONNECT not drained */
    /* Readiness-notification registrations (WSAEventSelect / WSAAsyncSelect). */
    void              *ev_handle;     /* kernel32 event to SetEvent on a match */
    long               ev_mask;       /* FD_* the event registration selected */
    void              *async_hwnd;    /* window to PostMessage on a match */
    unsigned           async_msg;     /* the message id to post */
    long               async_mask;    /* FD_* the async registration selected */
    long               async_seen;    /* FD_* already delivered (edge latch) */
    struct sockaddr_in local;
    struct sockaddr_in peer;
} ws2_ep[WS2_MAXFD];

static void ws2_forget(int fd) {
    if (fd >= 0 && fd < WS2_MAXFD)
        memset(&ws2_ep[fd], 0, sizeof ws2_ep[fd]);
}

/* Zero-timeout readiness probe: which FD_* conditions hold on this fd right
 * now.  Listening sockets report FD_ACCEPT (not FD_READ) when readable. */
static long ws2_probe(int fd) {
    if (fd < 0 || fd >= FD_SETSIZE) return 0;
    fd_set rd, wr;
    FD_ZERO(&rd); FD_ZERO(&wr);
    FD_SET(fd, &rd); FD_SET(fd, &wr);
    struct timeval z = { 0, 0 };
    long ev = 0;
    if (select(fd + 1, &rd, &wr, NULL, &z) > 0) {
        if (FD_ISSET(fd, &rd))
            ev |= (fd < WS2_MAXFD && ws2_ep[fd].is_listener)
                      ? WS2_FD_ACCEPT : WS2_FD_READ;
        if (FD_ISSET(fd, &wr)) ev |= WS2_FD_WRITE;
    }
    return ev;
}

/* Returns non-zero if fd would block for the given direction (read=0/write=1)
 * -- used to honour non-blocking recv/send without a native O_NONBLOCK. */
static int ws2_would_block(int fd, int want_write) {
    long r = ws2_probe(fd);
    if (want_write) return (r & WS2_FD_WRITE) ? 0 : 1;
    return (r & (WS2_FD_READ | WS2_FD_ACCEPT | WS2_FD_CLOSE)) ? 0 : 1;
}

/* The single readiness pump.  For every fd carrying a live registration it
 * measures readiness and fires the caller's notification: SetEvent for an
 * event registration, PostMessageW (one FD_* per message, edge-latched) for
 * an async registration.  Driven by WSAWaitForMultipleEvents and the user32
 * message loop.  Registration-gated, so idle fds are never probed. */
static void ws2_pump(void) {
    for (int fd = 0; fd < WS2_MAXFD; fd++) {
        struct ws2_endpoint *e = &ws2_ep[fd];
        if (!e->ev_handle && !e->async_hwnd) continue;
        long r = ws2_probe(fd);
        if (e->connect_pending && (r & WS2_FD_WRITE)) r |= WS2_FD_CONNECT;

        if (e->ev_handle && (r & e->ev_mask))
            SetEvent(e->ev_handle);

        if (e->async_hwnd) {
            long fire = r & e->async_mask & ~e->async_seen;
            static const int bits[] = { WS2_FD_READ, WS2_FD_WRITE,
                WS2_FD_ACCEPT, WS2_FD_CONNECT, WS2_FD_CLOSE };
            static const int idx[]  = { WS2_FD_READ_BIT, WS2_FD_WRITE_BIT,
                WS2_FD_ACCEPT_BIT, WS2_FD_CONNECT_BIT, WS2_FD_CLOSE_BIT };
            for (unsigned i = 0; i < sizeof bits / sizeof bits[0]; i++) {
                if (fire & bits[i]) {
                    PostMessageW(e->async_hwnd, e->async_msg,
                                 (uint64_t)(unsigned)fd,
                                 W32_MAKELONG(idx[i], 0));
                    e->async_seen |= bits[i];
                }
            }
            /* Level conditions (read/accept/close) re-arm once drained. */
            e->async_seen &= r;
        }
    }
}

/* Exported so the user32 message loop can drive async-select delivery. */
W32ABI void w32_ws2_pump(void) { ws2_pump(); }

/* ---- error plumbing --------------------------------------------------- */

/* Translate a libc errno into the closest WSA* code.  Only errno values the
 * socket wrappers can actually surface are mapped; the rest fall through to
 * WSAEFAULT ("something went wrong at the boundary"). */
static W32_DWORD ws2_wsa_from_errno(int e) {
    switch (e) {
    case EBADF:         return W32_WSAENOTSOCK;
    case EINVAL:        return W32_WSAEINVAL;
    case EACCES:        return W32_WSAEACCES;
    case EFAULT:        return W32_WSAEFAULT;
    case EMFILE:        return W32_WSAEMFILE;
    case ENFILE:        return W32_WSAEMFILE;
    case EAGAIN:        return W32_WSAEWOULDBLOCK;   /* EWOULDBLOCK == EAGAIN */
    case EINPROGRESS:   return W32_WSAEINPROGRESS;
    case EALREADY:      return W32_WSAEALREADY;
    case EMSGSIZE:      return W32_WSAEMSGSIZE;
    case EOPNOTSUPP:    return W32_WSAEOPNOTSUPP;
    case EAFNOSUPPORT:  return W32_WSAEAFNOSUPPORT;
    case EADDRINUSE:    return W32_WSAEADDRINUSE;
    case EADDRNOTAVAIL: return W32_WSAEADDRNOTAVAIL;
    case ENETDOWN:      return W32_WSAENETDOWN;
    case ENETUNREACH:   return W32_WSAENETUNREACH;
    case ECONNRESET:    return W32_WSAECONNRESET;
    case ENOTCONN:      return W32_WSAENOTCONN;
    case ETIMEDOUT:     return W32_WSAETIMEDOUT;
    case ECONNREFUSED:  return W32_WSAECONNREFUSED;
    case EHOSTUNREACH:  return W32_WSAEHOSTUNREACH;
    case ENOMEM:        return W32_WSAENOBUFS;
    case EINTR:         return W32_WSAEINTR;
    default:            return W32_WSAEFAULT;
    }
}

static void ws2_fail(void) { w32_set_last_error(ws2_wsa_from_errno(errno)); }

/* WSANOTINITIALISED guard used by every socket verb. */
static int ws2_need_init(void) {
    if (ws2_started <= 0) {
        w32_set_last_error(W32_WSANOTINITIALISED);
        return -1;
    }
    return 0;
}

/* ---- startup / errors ------------------------------------------------- */

/* WSADATA, Win64 field order (WORDs, then the vendor pointer, then the two
 * fixed-size strings).  Widths are ABI facts, written here rather than
 * pulled from an SDK header (w32/PROVENANCE.md). */
struct ws2_wsadata {
    uint16_t wVersion;
    uint16_t wHighVersion;
    uint16_t iMaxSockets;
    uint16_t iMaxUdpDg;
    char    *lpVendorInfo;
    char     szDescription[257];
    char     szSystemStatus[129];
};

W32ABI int w32_ws2_WSAStartup(W32_WORD wVersionRequested, void *lpWSAData) {
    struct ws2_wsadata *d = (struct ws2_wsadata *)lpWSAData;
    unsigned major = wVersionRequested & 0xff;
    unsigned minor = (wVersionRequested >> 8) & 0xff;

    /* We offer 2.2.  A request above that is refused BY VERSION; a request
     * at or below it is accepted and we agree to speak exactly what was
     * asked for (Windows semantics: wVersion echoes the negotiated word). */
    if (major == 0 || major > 2 || (major == 2 && minor > 2)) {
        if (d) {
            memset(d, 0, sizeof *d);
            d->wVersion = 0x0202;
            d->wHighVersion = 0x0202;
        }
        return W32_WSAVERNOTSUPPORTED;
    }

    if (d) {
        static const char desc[] =
            "AuraLite WS2_32 (WinSock 2.2 over native sockets)";
        static const char stat[] = "Running";
        memset(d, 0, sizeof *d);
        d->wVersion = wVersionRequested;
        d->wHighVersion = 0x0202;
        d->iMaxSockets = 0;   /* ignored for 2.x */
        d->iMaxUdpDg = 0;
        d->lpVendorInfo = 0;
        memcpy(d->szDescription, desc, sizeof desc);
        memcpy(d->szSystemStatus, stat, sizeof stat);
    }
    /* Hand the user32 message loop our readiness pump so a WSAAsyncSelect
     * registration delivers window messages while the app runs GetMessage. */
    if (ws2_started == 0)
        w32_user32_set_socket_pump(w32_ws2_pump);
    ws2_started++;
    return 0;
}

W32ABI int w32_ws2_WSACleanup(void) {
    if (ws2_started <= 0) {
        w32_set_last_error(W32_WSANOTINITIALISED);
        return W32_SOCKET_ERROR;
    }
    ws2_started--;
    return 0;
}

W32ABI int  w32_ws2_WSAGetLastError(void) {
    return (int)w32_get_last_error_raw();
}

W32ABI void w32_ws2_WSASetLastError(int err) {
    w32_set_last_error((W32_DWORD)err);
}

/* ---- byte order (pure) ------------------------------------------------ */

W32ABI uint32_t w32_ws2_htonl(uint32_t x) { return __builtin_bswap32(x); }
W32ABI uint32_t w32_ws2_ntohl(uint32_t x) { return __builtin_bswap32(x); }
W32ABI uint16_t w32_ws2_htons(uint16_t x) { return __builtin_bswap16(x); }
W32ABI uint16_t w32_ws2_ntohs(uint16_t x) { return __builtin_bswap16(x); }

/* ---- sockets ---------------------------------------------------------- */

W32ABI W32_SOCKET w32_ws2_socket(int af, int type, int protocol) {
    if (ws2_need_init()) return W32_INVALID_SOCKET;
    int fd = socket(af, type, protocol);
    if (fd < 0) { ws2_fail(); return W32_INVALID_SOCKET; }
    ws2_forget(fd);
    return (W32_SOCKET)(uintptr_t)(unsigned)fd;
}

W32ABI int w32_ws2_closesocket(W32_SOCKET s) {
    int fd = (int)s;
    int r = closesocket(fd);
    ws2_forget(fd);
    if (r < 0) { ws2_fail(); return W32_SOCKET_ERROR; }
    return 0;
}

W32ABI int w32_ws2_bind(W32_SOCKET s, const void *name, int namelen) {
    int fd = (int)s;
    int r = bind(fd, (const struct sockaddr *)name, (socklen_t)namelen);
    if (r < 0) { ws2_fail(); return W32_SOCKET_ERROR; }
    if (name && namelen >= (int)sizeof(struct sockaddr_in) &&
        ((const struct sockaddr *)name)->sa_family == AF_INET &&
        fd >= 0 && fd < WS2_MAXFD) {
        memcpy(&ws2_ep[fd].local, name, sizeof(struct sockaddr_in));
        ws2_ep[fd].have_local = 1;
    }
    return 0;
}

W32ABI int w32_ws2_listen(W32_SOCKET s, int backlog) {
    int fd = (int)s;
    int r = listen(fd, backlog);
    if (r < 0) { ws2_fail(); return W32_SOCKET_ERROR; }
    if (fd >= 0 && fd < WS2_MAXFD) ws2_ep[fd].is_listener = 1;
    return 0;
}

W32ABI W32_SOCKET w32_ws2_accept(W32_SOCKET s, void *addr, int *addrlen) {
    int lfd = (int)s;
    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    memset(&peer, 0, sizeof peer);
    int fd = accept(lfd, (struct sockaddr *)&peer, &plen);
    if (fd < 0) { ws2_fail(); return W32_INVALID_SOCKET; }

    /* Hand the peer address back to the caller if it asked. */
    if (addr && addrlen && *addrlen > 0) {
        int cp = (*addrlen < (int)sizeof peer) ? *addrlen : (int)sizeof peer;
        memcpy(addr, &peer, (size_t)cp);
        *addrlen = (int)sizeof peer;
    }

    /* Cache both endpoints for getpeername/getsockname on the new socket:
     * the peer is what accept() reported, the local name is inherited from
     * the listening socket's bind(). */
    if (fd >= 0 && fd < WS2_MAXFD) {
        ws2_forget(fd);
        memcpy(&ws2_ep[fd].peer, &peer, sizeof peer);
        ws2_ep[fd].have_peer = 1;
        if (lfd >= 0 && lfd < WS2_MAXFD && ws2_ep[lfd].have_local) {
            memcpy(&ws2_ep[fd].local, &ws2_ep[lfd].local, sizeof peer);
            ws2_ep[fd].have_local = 1;
        }
    }
    return (W32_SOCKET)(uintptr_t)(unsigned)fd;
}

W32ABI int w32_ws2_connect(W32_SOCKET s, const void *name, int namelen) {
    int fd = (int)s;
    int r = connectaddr(fd, (const struct sockaddr *)name, (unsigned)namelen);
    if (r < 0) { ws2_fail(); return W32_SOCKET_ERROR; }
    if (name && namelen >= (int)sizeof(struct sockaddr_in) &&
        ((const struct sockaddr *)name)->sa_family == AF_INET &&
        fd >= 0 && fd < WS2_MAXFD) {
        memcpy(&ws2_ep[fd].peer, name, sizeof(struct sockaddr_in));
        ws2_ep[fd].have_peer = 1;
    }
    /* The native connect completes synchronously; a non-blocking caller still
     * expects an FD_CONNECT edge, so latch one for the next pump/enum. */
    if (fd >= 0 && fd < WS2_MAXFD && ws2_ep[fd].nonblocking)
        ws2_ep[fd].connect_pending = 1;
    return 0;
}

W32ABI int w32_ws2_send(W32_SOCKET s, const char *buf, int len, int flags) {
    (void)flags;   /* MSG_* flags have no native syscall counterpart */
    int fd = (int)s;
    if (len < 0) { w32_set_last_error(W32_WSAEINVAL); return W32_SOCKET_ERROR; }
    /* Honour non-blocking mode: gate on a zero-timeout select() so a caller
     * that armed FIONBIO/WSAEventSelect gets WSAEWOULDBLOCK instead of a
     * blocking write (the native send has no O_NONBLOCK of its own). */
    if (fd >= 0 && fd < WS2_MAXFD && ws2_ep[fd].nonblocking &&
        ws2_would_block(fd, 1)) {
        w32_set_last_error(W32_WSAEWOULDBLOCK);
        return W32_SOCKET_ERROR;
    }
    int r = send(fd, buf, (uint32_t)len);
    if (r < 0) { ws2_fail(); return W32_SOCKET_ERROR; }
    return r;
}

W32ABI int w32_ws2_recv(W32_SOCKET s, char *buf, int len, int flags) {
    (void)flags;
    int fd = (int)s;
    if (len < 0) { w32_set_last_error(W32_WSAEINVAL); return W32_SOCKET_ERROR; }
    if (fd >= 0 && fd < WS2_MAXFD && ws2_ep[fd].nonblocking &&
        ws2_would_block(fd, 0)) {
        w32_set_last_error(W32_WSAEWOULDBLOCK);
        return W32_SOCKET_ERROR;
    }
    int r = recv(fd, buf, (uint32_t)len);
    if (r < 0) { ws2_fail(); return W32_SOCKET_ERROR; }
    return r;
}

W32ABI int w32_ws2_sendto(W32_SOCKET s, const char *buf, int len, int flags,
                          const void *to, int tolen) {
    (void)flags;
    if (len < 0) { w32_set_last_error(W32_WSAEINVAL); return W32_SOCKET_ERROR; }
    ssize_t r = sendto((int)s, buf, (size_t)len, 0,
                       (const struct sockaddr *)to, (socklen_t)tolen);
    if (r < 0) { ws2_fail(); return W32_SOCKET_ERROR; }
    return (int)r;
}

W32ABI int w32_ws2_recvfrom(W32_SOCKET s, char *buf, int len, int flags,
                            void *from, int *fromlen) {
    (void)flags;
    if (len < 0) { w32_set_last_error(W32_WSAEINVAL); return W32_SOCKET_ERROR; }
    socklen_t fl = 0, *flp = NULL;
    if (from && fromlen) { fl = (socklen_t)*fromlen; flp = &fl; }
    ssize_t r = recvfrom((int)s, buf, (size_t)len, 0,
                         (struct sockaddr *)from, flp);
    if (r < 0) { ws2_fail(); return W32_SOCKET_ERROR; }
    if (from && fromlen) *fromlen = (int)fl;
    return (int)r;
}

W32ABI int w32_ws2_shutdown(W32_SOCKET s, int how) {
    /* No native half-close syscall.  The FIN that closesocket() emits is the
     * only shutdown the stack can express, so this is accepted and treated
     * as advisory (documented non-goal).  Still validate the fd cheaply. */
    (void)how;
    int fd = (int)s;
    if (fd < 0) { w32_set_last_error(W32_WSAENOTSOCK); return W32_SOCKET_ERROR; }
    return 0;
}

/* ---- select() with the Windows fd_set translation --------------------- */

/* Windows fd_set is a counted array, not the POSIX bitmask.  A program built
 * against winsock2.h passes this shape; select() has to walk both. */
struct ws2_fdset {
    unsigned int fd_count;
    W32_SOCKET   fd_array[FD_SETSIZE];
};

/* Windows/mingw timeval on x64 is two 32-bit longs. */
struct ws2_timeval {
    int32_t tv_sec;
    int32_t tv_usec;
};

static int ws2_to_posix(const struct ws2_fdset *w, fd_set *p, int *maxfd) {
    if (!w) return 0;
    unsigned int n = w->fd_count;
    if (n > FD_SETSIZE) n = FD_SETSIZE;
    for (unsigned int i = 0; i < n; i++) {
        int fd = (int)w->fd_array[i];
        if (fd < 0 || fd >= FD_SETSIZE) continue;
        FD_SET(fd, p);
        if (fd > *maxfd) *maxfd = fd;
    }
    return 1;
}

static void ws2_from_posix(struct ws2_fdset *w, const fd_set *p) {
    if (!w) return;
    unsigned int n = w->fd_count, keep = 0;
    if (n > FD_SETSIZE) n = FD_SETSIZE;
    for (unsigned int i = 0; i < n; i++) {
        int fd = (int)w->fd_array[i];
        if (fd >= 0 && fd < FD_SETSIZE && FD_ISSET(fd, p))
            w->fd_array[keep++] = w->fd_array[i];
    }
    w->fd_count = keep;
}

W32ABI int w32_ws2_select(int nfds, void *readfds, void *writefds,
                          void *exceptfds, const void *timeout) {
    (void)nfds;   /* Windows ignores it; we recompute from the arrays. */
    struct ws2_fdset *wr = (struct ws2_fdset *)readfds;
    struct ws2_fdset *ww = (struct ws2_fdset *)writefds;
    struct ws2_fdset *we = (struct ws2_fdset *)exceptfds;
    fd_set pr, pw, pe;
    int maxfd = -1;

    FD_ZERO(&pr); FD_ZERO(&pw); FD_ZERO(&pe);
    ws2_to_posix(wr, &pr, &maxfd);
    ws2_to_posix(ww, &pw, &maxfd);
    ws2_to_posix(we, &pe, &maxfd);

    struct timeval tv, *tvp = NULL;
    if (timeout) {
        const struct ws2_timeval *t = (const struct ws2_timeval *)timeout;
        tv.tv_sec = t->tv_sec;
        tv.tv_usec = t->tv_usec;
        tvp = &tv;
    }

    int rc = select(maxfd + 1,
                    wr ? &pr : NULL, ww ? &pw : NULL, we ? &pe : NULL, tvp);
    if (rc < 0) { ws2_fail(); return W32_SOCKET_ERROR; }

    ws2_from_posix(wr, &pr);
    ws2_from_posix(ww, &pw);
    ws2_from_posix(we, &pe);
    return rc;
}

W32ABI int w32_ws2___WSAFDIsSet(W32_SOCKET s, void *set) {
    const struct ws2_fdset *w = (const struct ws2_fdset *)set;
    if (!w) return 0;
    unsigned int n = w->fd_count;
    if (n > FD_SETSIZE) n = FD_SETSIZE;
    for (unsigned int i = 0; i < n; i++)
        if (w->fd_array[i] == s) return 1;
    return 0;
}

W32ABI int w32_ws2_ioctlsocket(W32_SOCKET s, long cmd, uint32_t *argp) {
    int fd = (int)s;
    if (cmd == W32_FIONBIO) {
        /* Advisory: the native recv/send have no non-blocking mode, so we
         * remember the flag but the transport stays blocking (non-goal). */
        if (fd >= 0 && fd < WS2_MAXFD)
            ws2_ep[fd].nonblocking = (argp && *argp) ? 1 : 0;
        return 0;
    }
    if (cmd == W32_FIONREAD) {
        /* No bytes-available query on the native stack; report none. */
        if (argp) *argp = 0;
        return 0;
    }
    w32_set_last_error(W32_WSAEINVAL);
    return W32_SOCKET_ERROR;
}

/* ---- socket options --------------------------------------------------- */

/* Windows option constants (level/name), distinct from the libc numbers. */
#define WS2_SOL_SOCKET   0xffff
#define WS2_IPPROTO_TCP  6
#define WS2_SO_REUSEADDR 0x0004
#define WS2_SO_KEEPALIVE 0x0008
#define WS2_SO_BROADCAST 0x0020
#define WS2_SO_LINGER    0x0080
#define WS2_SO_SNDBUF    0x1001
#define WS2_SO_RCVBUF    0x1002
#define WS2_SO_SNDTIMEO  0x1005
#define WS2_SO_RCVTIMEO  0x1006
#define WS2_SO_ERROR     0x1007
#define WS2_SO_TYPE      0x1008
#define WS2_TCP_NODELAY  0x0001

static int ws2_opt_known(int level, int optname) {
    if (level == WS2_SOL_SOCKET) {
        switch (optname) {
        case WS2_SO_REUSEADDR: case WS2_SO_KEEPALIVE: case WS2_SO_BROADCAST:
        case WS2_SO_LINGER:    case WS2_SO_SNDBUF:    case WS2_SO_RCVBUF:
        case WS2_SO_SNDTIMEO:  case WS2_SO_RCVTIMEO:
        case WS2_SO_ERROR:     case WS2_SO_TYPE:
            return 1;
        }
    }
    if (level == WS2_IPPROTO_TCP && optname == WS2_TCP_NODELAY) return 1;
    return 0;
}

W32ABI int w32_ws2_setsockopt(W32_SOCKET s, int level, int optname,
                              const char *optval, int optlen) {
    (void)s; (void)optval; (void)optlen;
    if (!ws2_opt_known(level, optname)) {
        w32_set_last_error(W32_WSAENOPROTOOPT);
        return W32_SOCKET_ERROR;
    }
    /* libc setsockopt is itself a no-op; mirror the success. */
    return 0;
}

W32ABI int w32_ws2_getsockopt(W32_SOCKET s, int level, int optname,
                              char *optval, int *optlen) {
    (void)s;
    if (!ws2_opt_known(level, optname)) {
        w32_set_last_error(W32_WSAENOPROTOOPT);
        return W32_SOCKET_ERROR;
    }
    if (optval && optlen && *optlen >= (int)sizeof(int)) {
        int v = 0;
        if (level == WS2_SOL_SOCKET && optname == WS2_SO_TYPE)
            v = SOCK_STREAM;      /* the only type this stack serves */
        /* SO_ERROR and the rest report the benign default (0/no error). */
        memcpy(optval, &v, sizeof v);
        *optlen = (int)sizeof(int);
    }
    return 0;
}

W32ABI int w32_ws2_getsockname(W32_SOCKET s, void *name, int *namelen) {
    int fd = (int)s;
    if (fd < 0 || fd >= WS2_MAXFD || !ws2_ep[fd].have_local) {
        w32_set_last_error(W32_WSAEINVAL);
        return W32_SOCKET_ERROR;
    }
    if (name && namelen && *namelen > 0) {
        int cp = (*namelen < (int)sizeof(struct sockaddr_in))
                     ? *namelen : (int)sizeof(struct sockaddr_in);
        memcpy(name, &ws2_ep[fd].local, (size_t)cp);
        *namelen = (int)sizeof(struct sockaddr_in);
    }
    return 0;
}

W32ABI int w32_ws2_getpeername(W32_SOCKET s, void *name, int *namelen) {
    int fd = (int)s;
    if (fd < 0 || fd >= WS2_MAXFD || !ws2_ep[fd].have_peer) {
        w32_set_last_error(W32_WSAENOTCONN);
        return W32_SOCKET_ERROR;
    }
    if (name && namelen && *namelen > 0) {
        int cp = (*namelen < (int)sizeof(struct sockaddr_in))
                     ? *namelen : (int)sizeof(struct sockaddr_in);
        memcpy(name, &ws2_ep[fd].peer, (size_t)cp);
        *namelen = (int)sizeof(struct sockaddr_in);
    }
    return 0;
}

/* ---- resolution ------------------------------------------------------- */

/* Windows ADDRINFOA field order differs from POSIX (ai_canonname and ai_addr
 * are swapped, ai_addrlen is size_t): a mingw program reads exactly this. */
struct ws2_addrinfo {
    int                  ai_flags;
    int                  ai_family;
    int                  ai_socktype;
    int                  ai_protocol;
    size_t               ai_addrlen;
    char                *ai_canonname;
    struct sockaddr     *ai_addr;
    struct ws2_addrinfo *ai_next;
};

W32ABI int w32_ws2_getaddrinfo(const char *node, const char *service,
                               const void *hints, void **res) {
    if (res) *res = NULL;

    struct addrinfo lhints, *lres = NULL, *lp;
    struct addrinfo *hp = NULL;
    if (hints) {
        const struct ws2_addrinfo *wh = (const struct ws2_addrinfo *)hints;
        memset(&lhints, 0, sizeof lhints);
        lhints.ai_flags    = wh->ai_flags;
        lhints.ai_family   = wh->ai_family;
        lhints.ai_socktype = wh->ai_socktype;
        lhints.ai_protocol = wh->ai_protocol;
        hp = &lhints;
    }

    int rc = getaddrinfo(node, service, hp, &lres);
    if (rc != 0 || !lres) {
        W32_DWORD w = (rc == EAI_MEMORY) ? W32_WSA_NOT_ENOUGH_MEMORY_LOCAL
                    : (rc == EAI_FAIL)   ? W32_WSANO_RECOVERY
                                         : W32_WSAHOST_NOT_FOUND;
        w32_set_last_error(w);
        return (int)w;
    }

    /* Translate the POSIX list into a Windows-layout list. */
    struct ws2_addrinfo *head = NULL, *tail = NULL;
    for (lp = lres; lp; lp = lp->ai_next) {
        struct ws2_addrinfo *w = (struct ws2_addrinfo *)malloc(sizeof *w);
        if (!w) break;
        memset(w, 0, sizeof *w);
        w->ai_flags    = lp->ai_flags;
        w->ai_family   = lp->ai_family;
        w->ai_socktype = lp->ai_socktype;
        w->ai_protocol = lp->ai_protocol;
        w->ai_addrlen  = lp->ai_addrlen;
        if (lp->ai_addr && lp->ai_addrlen) {
            w->ai_addr = (struct sockaddr *)malloc(lp->ai_addrlen);
            if (w->ai_addr)
                memcpy(w->ai_addr, lp->ai_addr, lp->ai_addrlen);
        }
        if (head == NULL) head = w; else tail->ai_next = w;
        tail = w;
    }
    freeaddrinfo(lres);

    if (!head) {
        w32_set_last_error(W32_WSA_NOT_ENOUGH_MEMORY_LOCAL);
        return (int)W32_WSA_NOT_ENOUGH_MEMORY_LOCAL;
    }
    if (res) *res = head;
    return 0;
}

W32ABI void w32_ws2_freeaddrinfo(void *res) {
    struct ws2_addrinfo *w = (struct ws2_addrinfo *)res;
    while (w) {
        struct ws2_addrinfo *n = w->ai_next;
        if (w->ai_addr) free(w->ai_addr);
        free(w);
        w = n;
    }
}

/* gethostbyname returns a pointer into static storage (Windows semantics:
 * per-thread static, single outstanding result).  Windows hostent has short
 * h_addrtype/h_length, distinct from the POSIX int fields. */
struct ws2_hostent {
    char  *h_name;
    char **h_aliases;
    short  h_addrtype;
    short  h_length;
    char **h_addr_list;
};

W32ABI void *w32_ws2_gethostbyname(const char *name) {
    static struct ws2_hostent he;
    static char   namebuf[256];
    static uint32_t addr;
    static char  *addr_list[2];
    static char  *aliases[1];

    struct hostent *h = gethostbyname(name);
    if (!h || !h->h_addr_list || !h->h_addr_list[0]) {
        w32_set_last_error(W32_WSAHOST_NOT_FOUND);
        return NULL;
    }
    size_t nl = 0;
    if (h->h_name) { nl = strlen(h->h_name); if (nl > 255) nl = 255; }
    memcpy(namebuf, h->h_name ? h->h_name : "", nl);
    namebuf[nl] = 0;
    memcpy(&addr, h->h_addr_list[0], 4);

    addr_list[0] = (char *)&addr;
    addr_list[1] = NULL;
    aliases[0]   = NULL;
    he.h_name      = namebuf;
    he.h_aliases   = aliases;
    he.h_addrtype  = AF_INET;
    he.h_length    = 4;
    he.h_addr_list = addr_list;
    return &he;
}

W32ABI const char *w32_ws2_inet_ntop(int af, const void *src, char *dst,
                                     size_t size) {
    const char *r = inet_ntop(af, src, dst, (socklen_t)size);
    if (!r) { w32_set_last_error(W32_WSAEAFNOSUPPORT); return NULL; }
    return r;
}

W32ABI int w32_ws2_inet_pton(int af, const char *src, void *dst) {
    int r = inet_pton(af, src, dst);
    if (r < 0) { w32_set_last_error(W32_WSAEAFNOSUPPORT); return -1; }
    return r;   /* 1 parsed, 0 not a valid string (Windows semantics) */
}

W32ABI uint32_t w32_ws2_inet_addr(const char *cp) {
    return (uint32_t)inet_addr(cp);   /* network order; 0xffffffff on error */
}

W32ABI int w32_ws2_gethostname(char *name, int namelen) {
    /* The native stack has no name query; report a stable local name. */
    static const char host[] = "auralite";
    if (!name || namelen <= 0) {
        w32_set_last_error(W32_WSAEFAULT);
        return W32_SOCKET_ERROR;
    }
    size_t n = sizeof host - 1;
    if (n >= (size_t)namelen) { w32_set_last_error(W32_WSAEFAULT);
                                return W32_SOCKET_ERROR; }
    memcpy(name, host, n + 1);
    return 0;
}

W32ABI char *w32_ws2_inet_ntoa(uint32_t addr) {
    /* in_addr is a 4-byte struct passed by value == one 32-bit register.
     * Windows returns a pointer into per-thread static storage. */
    static char buf[16];
    const unsigned char *b = (const unsigned char *)&addr;
    unsigned p = 0;
    for (int i = 0; i < 4; i++) {
        unsigned v = b[i];
        if (v >= 100) buf[p++] = (char)('0' + v / 100);
        if (v >= 10)  buf[p++] = (char)('0' + (v / 10) % 10);
        buf[p++] = (char)('0' + v % 10);
        if (i < 3) buf[p++] = '.';
    }
    buf[p] = 0;
    return buf;
}

/* ---- name/service resolution ------------------------------------------ */

/* A tiny well-known services table.  The native stack has no /etc/services,
 * so getservbyname/getservbyport (and the named-service path of getnameinfo)
 * answer from the ports the pinned clients ask for (ssh first -- it is why
 * PuTTY resolves the name at all). */
struct ws2_service { const char *name; unsigned short port; const char *proto; };
static const struct ws2_service ws2_services[] = {
    { "ftp-data", 20, "tcp" }, { "ftp",   21, "tcp" },
    { "ssh",      22, "tcp" }, { "telnet",23, "tcp" },
    { "smtp",     25, "tcp" }, { "domain",53, "tcp" },
    { "domain",   53, "udp" }, { "http",  80, "tcp" },
    { "pop3",    110, "tcp" }, { "nntp", 119, "tcp" },
    { "ntp",     123, "udp" }, { "imap", 143, "tcp" },
    { "https",   443, "tcp" }, { "submission", 587, "tcp" },
    { "rlogin",  513, "tcp" }, { "imaps", 993, "tcp" },
    { "pop3s",   995, "tcp" }, { "socks",1080, "tcp" },
};

/* Windows servent.  On Win64 WinSock's SERVENT SWAPS the last two members --
 * s_proto comes BEFORE s_port (psdk_inc/_ip_types.h, #ifdef _WIN64) -- so the
 * layout must be written in that order, not the classic BSD one. */
struct ws2_servent {
    char  *s_name;
    char **s_aliases;
    char  *s_proto;
    short  s_port;      /* network byte order */
};

/* getnameinfo.  The native resolver has no reverse (PTR) path, so the host
 * is always rendered numerically -- honest, and exactly what a client that
 * passes NI_NUMERICHOST expects.  The service is named from the well-known
 * table below unless NI_NUMERICSERV is set. */
#define WS2_NI_NUMERICHOST 0x02
#define WS2_NI_NUMERICSERV 0x08

W32ABI int w32_ws2_getnameinfo(const void *sa, int salen, char *host,
                               W32_DWORD hostlen, char *serv,
                               W32_DWORD servlen, int flags) {
    const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
    if (!sa || salen < (int)sizeof(struct sockaddr_in) ||
        in->sin_family != AF_INET) {
        w32_set_last_error(W32_WSAEFAULT);
        return (int)W32_WSAEFAULT;
    }
    if (host && hostlen) {
        char tmp[16];
        if (!inet_ntop(AF_INET, &in->sin_addr, tmp, sizeof tmp)) {
            w32_set_last_error(W32_WSAEFAULT);
            return (int)W32_WSAEFAULT;
        }
        size_t n = strlen(tmp);
        if (n >= hostlen) { w32_set_last_error(W32_WSAEFAULT);
                            return (int)W32_WSAEFAULT; }
        memcpy(host, tmp, n + 1);
    }
    if (serv && servlen) {
        unsigned port = __builtin_bswap16(in->sin_port);
        char pb[16]; const char *out = NULL;
        if (!(flags & WS2_NI_NUMERICSERV)) {
            for (unsigned i = 0;
                 i < sizeof ws2_services / sizeof ws2_services[0]; i++)
                if (ws2_services[i].port == port &&
                    strcmp(ws2_services[i].proto, "tcp") == 0) {
                    out = ws2_services[i].name; break;
                }
        }
        if (!out) {   /* numeric fall-back */
            int pn = 0; if (port == 0) pb[pn++] = '0';
            else { char rev[6]; int rn = 0;
                   while (port) { rev[rn++] = (char)('0'+port%10); port/=10; }
                   while (rn) pb[pn++] = rev[--rn]; }
            pb[pn] = 0; out = pb;
        }
        size_t n = strlen(out);
        if (n >= servlen) { w32_set_last_error(W32_WSAEFAULT);
                            return (int)W32_WSAEFAULT; }
        memcpy(serv, out, n + 1);
    }
    return 0;
}

static void *ws2_fill_servent(const struct ws2_service *svc) {
    static struct ws2_servent se;
    static char  namebuf[32];
    static char  protobuf[8];
    static char *aliases[1] = { NULL };
    size_t nl = strlen(svc->name); if (nl > 31) nl = 31;
    memcpy(namebuf, svc->name, nl); namebuf[nl] = 0;
    size_t pl = strlen(svc->proto); if (pl > 7) pl = 7;
    memcpy(protobuf, svc->proto, pl); protobuf[pl] = 0;
    se.s_name    = namebuf;
    se.s_aliases = aliases;
    se.s_port    = (short)__builtin_bswap16(svc->port);
    se.s_proto   = protobuf;
    return &se;
}

W32ABI void *w32_ws2_getservbyname(const char *name, const char *proto) {
    if (!name) { w32_set_last_error(W32_WSAEFAULT); return NULL; }
    for (unsigned i = 0; i < sizeof ws2_services / sizeof ws2_services[0]; i++) {
        if (strcmp(ws2_services[i].name, name) == 0 &&
            (!proto || !*proto || strcmp(ws2_services[i].proto, proto) == 0))
            return ws2_fill_servent(&ws2_services[i]);
    }
    w32_set_last_error(W32_WSANO_DATA);
    return NULL;
}

W32ABI void *w32_ws2_getservbyport(int port, const char *proto) {
    unsigned short host_port = __builtin_bswap16((unsigned short)port);
    for (unsigned i = 0; i < sizeof ws2_services / sizeof ws2_services[0]; i++) {
        if (ws2_services[i].port == host_port &&
            (!proto || !*proto || strcmp(ws2_services[i].proto, proto) == 0))
            return ws2_fill_servent(&ws2_services[i]);
    }
    w32_set_last_error(W32_WSANO_DATA);
    return NULL;
}

W32ABI void *w32_ws2_gethostbyaddr(const char *addr, int len, int type) {
    static struct ws2_hostent he;
    static char   namebuf[256];
    static uint32_t a;
    static char  *addr_list[2];
    static char  *aliases[1];
    if (!addr || len != 4 || type != AF_INET) {
        w32_set_last_error(W32_WSAEFAULT);
        return NULL;
    }
    memcpy(&a, addr, 4);
    /* The native resolver has no reverse (PTR) path, so the canonical name is
     * the dotted-quad -- honest, and the address list still round-trips. */
    {
        char *d = w32_ws2_inet_ntoa(a);
        size_t nl = strlen(d); if (nl > 255) nl = 255;
        memcpy(namebuf, d, nl); namebuf[nl] = 0;
    }
    addr_list[0] = (char *)&a; addr_list[1] = NULL; aliases[0] = NULL;
    he.h_name = namebuf; he.h_aliases = aliases;
    he.h_addrtype = AF_INET; he.h_length = 4; he.h_addr_list = addr_list;
    return &he;
}

/* ---- address <-> string ----------------------------------------------- */

W32ABI int w32_ws2_WSAAddressToStringA(void *sa, W32_DWORD salen,
                                       void *protoinfo, char *buf,
                                       W32_DWORD *buflen) {
    (void)protoinfo;
    const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
    if (!sa || salen < (W32_DWORD)sizeof(struct sockaddr_in) ||
        in->sin_family != AF_INET || !buf || !buflen) {
        w32_set_last_error(W32_WSAEFAULT);
        return W32_SOCKET_ERROR;
    }
    char tmp[32];
    char *ip = w32_ws2_inet_ntoa(*(const uint32_t *)&in->sin_addr);
    unsigned port = __builtin_bswap16(in->sin_port);
    /* WinSock always renders "a.b.c.d:port" for AF_INET. */
    size_t n = 0;
    for (const char *p = ip; *p; p++) tmp[n++] = *p;
    tmp[n++] = ':';
    char pb[6]; int pn = 0;
    if (port == 0) pb[pn++] = '0';
    else { char rev[6]; int rn = 0; while (port) { rev[rn++] = (char)('0'+port%10); port/=10; }
           while (rn) pb[pn++] = rev[--rn]; }
    for (int i = 0; i < pn; i++) tmp[n++] = pb[i];
    tmp[n] = 0;
    if (*buflen <= n) { *buflen = (W32_DWORD)(n + 1);
                        w32_set_last_error(W32_WSAEFAULT);
                        return W32_SOCKET_ERROR; }
    memcpy(buf, tmp, n + 1);
    *buflen = (W32_DWORD)n;
    return 0;
}

W32ABI int w32_ws2_WSAStringToAddressA(char *str, int family, void *protoinfo,
                                       void *sa, int *salen) {
    (void)protoinfo;
    if (!str || family != AF_INET || !sa || !salen ||
        *salen < (int)sizeof(struct sockaddr_in)) {
        w32_set_last_error(W32_WSAEFAULT);
        return W32_SOCKET_ERROR;
    }
    char ipbuf[64]; int i = 0;
    while (str[i] && str[i] != ':' && i < 63) { ipbuf[i] = str[i]; i++; }
    ipbuf[i] = 0;
    struct sockaddr_in in;
    memset(&in, 0, sizeof in);
    in.sin_family = AF_INET;
    if (inet_pton(AF_INET, ipbuf, &in.sin_addr) != 1) {
        w32_set_last_error(W32_WSAEINVAL);
        return W32_SOCKET_ERROR;
    }
    if (str[i] == ':') {
        unsigned port = 0;
        for (int j = i + 1; str[j] >= '0' && str[j] <= '9'; j++)
            port = port * 10 + (unsigned)(str[j] - '0');
        in.sin_port = __builtin_bswap16((unsigned short)port);
    }
    memcpy(sa, &in, sizeof in);
    *salen = (int)sizeof in;
    return 0;
}

/* ---- WSAIoctl ---------------------------------------------------------- */

#define WS2_SIO_KEEPALIVE_VALS              0x98000004u
#define WS2_SIO_GET_EXTENSION_FUNCTION_PTR  0xC8000006u

W32ABI int w32_ws2_WSAIoctl(W32_SOCKET s, W32_DWORD code, void *inbuf,
                            W32_DWORD inlen, void *outbuf, W32_DWORD outlen,
                            W32_DWORD *retlen, void *overlapped,
                            void *completion) {
    (void)inbuf; (void)inlen; (void)outbuf; (void)outlen;
    if (overlapped || completion) {   /* no overlapped completion substrate */
        w32_set_last_error(W32_WSAEOPNOTSUPP);
        return W32_SOCKET_ERROR;
    }
    if (retlen) *retlen = 0;
    switch (code) {
    case (W32_DWORD)W32_FIONBIO: {
        uint32_t v = (inbuf && inlen >= 4) ? *(uint32_t *)inbuf : 0;
        return w32_ws2_ioctlsocket(s, W32_FIONBIO, &v);
    }
    case (W32_DWORD)W32_FIONREAD: {
        uint32_t v = 0;
        int r = w32_ws2_ioctlsocket(s, W32_FIONREAD, &v);
        if (outbuf && outlen >= 4) { *(uint32_t *)outbuf = v;
                                     if (retlen) *retlen = 4; }
        return r;
    }
    case WS2_SIO_KEEPALIVE_VALS:
        /* Accept the keepalive tuning PuTTY sends; the native stack has no
         * per-socket keepalive knob, so this is an accepted no-op. */
        return 0;
    case WS2_SIO_GET_EXTENSION_FUNCTION_PTR:
        /* The overlapped-extension probe (ConnectEx/AcceptEx): refused. */
        w32_set_last_error(W32_WSAEOPNOTSUPP);
        return W32_SOCKET_ERROR;
    default:
        w32_set_last_error(W32_WSAEINVAL);
        return W32_SOCKET_ERROR;
    }
}

/* ---- readiness notification: WSAEventSelect / WSAEnumNetworkEvents ----- *
 * These are REAL over the ws2_pump readiness engine defined above.  PuTTY's
 * console tool (plink) drives exactly this pair; the GUI drives the async
 * variant below. */

W32ABI int w32_ws2_WSAEventSelect(W32_SOCKET s, void *hEvent, long ev) {
    int fd = (int)s;
    if (fd < 0 || fd >= WS2_MAXFD) {
        w32_set_last_error(W32_WSAENOTSOCK);
        return W32_SOCKET_ERROR;
    }
    ws2_ep[fd].ev_handle = hEvent;
    ws2_ep[fd].ev_mask   = ev;
    /* WSAEventSelect implies non-blocking mode (Windows contract). */
    ws2_ep[fd].nonblocking = hEvent ? 1 : ws2_ep[fd].nonblocking;
    if (!hEvent || ev == 0) {   /* deselect */
        ws2_ep[fd].ev_handle = NULL; ws2_ep[fd].ev_mask = 0;
        return 0;
    }
    /* Signal immediately if a selected condition already holds. */
    if (ws2_probe(fd) & ev) SetEvent(hEvent);
    return 0;
}

W32ABI int w32_ws2_WSAEnumNetworkEvents(W32_SOCKET s, void *hEvent,
                                        void *out) {
    int fd = (int)s;
    struct ws2_networkevents *ne = (struct ws2_networkevents *)out;
    if (fd < 0 || fd >= WS2_MAXFD) {
        w32_set_last_error(W32_WSAENOTSOCK);
        return W32_SOCKET_ERROR;
    }
    long sel = ws2_ep[fd].ev_mask ? ws2_ep[fd].ev_mask : ~0L;
    long r = ws2_probe(fd) & sel;
    if (ws2_ep[fd].connect_pending) {
        r |= WS2_FD_CONNECT;
        ws2_ep[fd].connect_pending = 0;
    }
    if (ne) {
        memset(ne, 0, sizeof *ne);
        ne->lNetworkEvents = r;   /* iErrorCode stays 0: all conditions clean */
    }
    /* Reset the associated auto-report event (arg wins, else the bound one). */
    void *h = hEvent ? hEvent : ws2_ep[fd].ev_handle;
    if (h) ResetEvent(h);
    return 0;
}

/* ---- readiness notification: WSAAsyncSelect --------------------------- */

W32ABI int w32_ws2_WSAAsyncSelect(W32_SOCKET s, void *hWnd, unsigned msg,
                                  long ev) {
    int fd = (int)s;
    if (fd < 0 || fd >= WS2_MAXFD) {
        w32_set_last_error(W32_WSAENOTSOCK);
        return W32_SOCKET_ERROR;
    }
    if (ev == 0) {   /* cancel notification */
        ws2_ep[fd].async_hwnd = NULL; ws2_ep[fd].async_msg = 0;
        ws2_ep[fd].async_mask = 0;    ws2_ep[fd].async_seen = 0;
        return 0;
    }
    ws2_ep[fd].async_hwnd = hWnd;
    ws2_ep[fd].async_msg  = msg;
    ws2_ep[fd].async_mask = ev;
    ws2_ep[fd].async_seen = 0;
    ws2_ep[fd].nonblocking = 1;   /* async-select implies non-blocking */
    /* Deliver any already-satisfied condition on the next pump tick. */
    return 0;
}

/* ---- WSA event objects: REAL over kernel32 manual-reset events -------- *
 * PuTTY builds its own events with kernel32 CreateEvent, but a WSAEvent is
 * simply a manual-reset kernel event, so these thin wrappers make the whole
 * family coherent and are exercised by the W32A-12 async fixtures. */

W32ABI void *w32_ws2_WSACreateEvent(void) {
    void *h = CreateEventW(NULL, 1 /*manual*/, 0 /*non-signalled*/, NULL);
    if (!h) w32_set_last_error(W32_WSAENOBUFS);
    return h;   /* WSA_INVALID_EVENT == NULL on failure */
}
W32ABI int w32_ws2_WSACloseEvent(void *h) {
    if (!h) { w32_set_last_error(W32_WSAEINVAL); return 0; }
    return CloseHandle(h) ? 1 : 0;
}
W32ABI int w32_ws2_WSASetEvent(void *h) {
    if (!h) { w32_set_last_error(W32_WSAEINVAL); return 0; }
    return SetEvent(h) ? 1 : 0;
}
W32ABI int w32_ws2_WSAResetEvent(void *h) {
    if (!h) { w32_set_last_error(W32_WSAEINVAL); return 0; }
    return ResetEvent(h) ? 1 : 0;
}

W32ABI W32_DWORD w32_ws2_WSAWaitForMultipleEvents(W32_DWORD count,
                                                  const void *events,
                                                  W32_BOOL waitAll,
                                                  W32_DWORD ms,
                                                  W32_BOOL alertable) {
    (void)alertable;
    if (!events || count == 0) {
        w32_set_last_error(W32_WSAEINVAL);
        return WS2_WAIT_FAILED;
    }
    /* Pump socket readiness into the kernel events, then wait in slices so a
     * socket that becomes ready mid-wait still signals its bound event. */
    W32_DWORD waited = 0;
    for (;;) {
        ws2_pump();
        W32_DWORD slice = 20;
        if (ms != WS2_INFINITE) {
            W32_DWORD left = ms - waited;
            if (left < slice) slice = left;
        }
        W32_DWORD r = WaitForMultipleObjects(count, (void **)events,
                                             waitAll ? 1 : 0, slice);
        if (r != WS2_WAIT_TIMEOUT) return r;   /* signalled (WSA_WAIT_EVENT_0+) */
        if (ms != WS2_INFINITE) {
            waited += slice;
            if (waited >= ms) return WS2_WAIT_TIMEOUT;   /* WSA_WAIT_TIMEOUT */
        }
    }
}
