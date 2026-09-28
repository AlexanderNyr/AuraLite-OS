/* W32A-12: host unit test for the WS2_32 adapter's ABI-boundary logic.
 *
 * The in-guest QEMU gate (test_w32a12_winsock.sh) proves the socket verbs
 * REAL over the native stack; this test amalgamates ws2_32.c against
 * hand-written libc socket doubles and exercises the parts that are pure
 * boundary logic: version negotiation, byte order, the errno->WSA map,
 * the Windows<->POSIX fd_set translation, the Windows ADDRINFOA layout
 * translation, the getsockname/getpeername endpoint cache, ioctlsocket,
 * and the option/refusal surface.
 *
 * The W32A-12 full expansion adds the surface the pinned PuTTY/plink binaries
 * actually resolve (w32/tests/W32A12.probe.log): inet_ntoa, getservbyname/
 * getservbyport, getnameinfo, WSAAddressToStringA/WSAStringToAddressA, the
 * WSAIoctl control-code dispatch, non-blocking recv/send gating, and the
 * readiness-notification layer -- WSAEventSelect + WSAEnumNetworkEvents over
 * kernel32 event doubles, WSAWaitForMultipleEvents pumping readiness, and
 * WSAAsyncSelect delivering a window message through the pump (user32
 * PostMessageW double).  Sanitizers on.
 *
 * SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/select.h>

#include "w32/w32_errno.h"

/* ---- w32_errno doubles (the imm test's pattern) ----------------------- */
static W32_DWORD g_lasterr;
void      w32_set_last_error(W32_DWORD e) { g_lasterr = e; }
W32_DWORD w32_get_last_error_raw(void)    { return g_lasterr; }

/* ---- libc socket doubles --------------------------------------------- */
/* Enough state for the adapter to drive; each double records what it saw
 * so the test can assert the translation was correct. */
static int      dbl_next_fd = 3;
static int      dbl_fail_next;     /* if set, the next verb returns -1/errno */
static int      dbl_last_errno_set;

static int fail_with(int e) { extern int *__errno_location(void);
                              *__errno_location() = e; return -1; }

int socket(int d, int t, int p) {
    (void)d; (void)t; (void)p;
    if (dbl_fail_next) { dbl_fail_next = 0; return fail_with(dbl_last_errno_set); }
    return dbl_next_fd++;
}
int bind(int fd, const struct sockaddr *a, socklen_t l) {
    (void)fd; (void)a; (void)l;
    if (dbl_fail_next) { dbl_fail_next = 0; return fail_with(dbl_last_errno_set); }
    return 0;
}
int listen(int fd, int b) { (void)fd; (void)b; return 0; }

static uint32_t dbl_accept_ip;    /* network order */
static uint16_t dbl_accept_port;  /* network order */
int accept(int fd, struct sockaddr *a, socklen_t *l) {
    (void)fd;
    if (dbl_fail_next) { dbl_fail_next = 0; return fail_with(dbl_last_errno_set); }
    if (a && l && *l >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *sin = (struct sockaddr_in *)a;
        memset(sin, 0, sizeof *sin);
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = dbl_accept_ip;
        sin->sin_port = dbl_accept_port;
        *l = sizeof *sin;
    }
    return dbl_next_fd++;
}
int connectaddr(int fd, const struct sockaddr *a, unsigned l) {
    (void)fd; (void)a; (void)l;
    if (dbl_fail_next) { dbl_fail_next = 0; return fail_with(dbl_last_errno_set); }
    return 0;
}
int send(int fd, const void *b, uint32_t n) { (void)fd; (void)b;
    if (dbl_fail_next) { dbl_fail_next = 0; return fail_with(dbl_last_errno_set); }
    return (int)n; }
int recv(int fd, void *b, uint32_t n) { (void)fd; (void)b;
    if (dbl_fail_next) { dbl_fail_next = 0; return fail_with(dbl_last_errno_set); }
    return (int)n; }
int closesocket(int fd) { (void)fd; return 0; }
ssize_t sendto(int fd, const void *b, size_t n, int f,
               const struct sockaddr *a, socklen_t l) {
    (void)fd; (void)b; (void)f; (void)a; (void)l; return (ssize_t)n; }
ssize_t recvfrom(int fd, void *b, size_t n, int f,
                 struct sockaddr *a, socklen_t *l) {
    (void)fd; (void)b; (void)f; (void)a; (void)l; return (ssize_t)n; }
int setsockopt(int fd, int lv, int on, const void *v, socklen_t l) {
    (void)fd; (void)lv; (void)on; (void)v; (void)l; return 0; }
int getsockopt(int fd, int lv, int on, void *v, socklen_t *l) {
    (void)fd; (void)lv; (void)on; (void)v; (void)l; return 0; }

/* select double: remember which POSIX fds were requested, then mark chosen
 * fds ready in readfds/writefds (the readiness pump probes both). */
static int   dbl_sel_maxfd;
static int   dbl_sel_ready_fd  = -1;   /* read-ready fd  */
static int   dbl_sel_wready_fd = -1;   /* write-ready fd */
int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t) {
    (void)e; (void)t;
    dbl_sel_maxfd = n;
    int cnt = 0;
    if (r) {
        fd_set keep; FD_ZERO(&keep);
        if (dbl_sel_ready_fd >= 0 && FD_ISSET(dbl_sel_ready_fd, r)) {
            FD_SET(dbl_sel_ready_fd, &keep); cnt++;
        }
        *r = keep;
    }
    if (w) {
        fd_set keep; FD_ZERO(&keep);
        if (dbl_sel_wready_fd >= 0 && FD_ISSET(dbl_sel_wready_fd, w)) {
            FD_SET(dbl_sel_wready_fd, &keep); cnt++;
        }
        *w = keep;
    }
    return cnt;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    (void)af;
    const unsigned char *b = (const unsigned char *)src;
    snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return dst;
}
int inet_pton(int af, const char *src, void *dst) {
    (void)af;
    unsigned a, b, c, d;
    if (sscanf(src, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    unsigned char *o = (unsigned char *)dst;
    o[0] = (unsigned char)a; o[1] = (unsigned char)b;
    o[2] = (unsigned char)c; o[3] = (unsigned char)d;
    return 1;
}
in_addr_t inet_addr(const char *cp) {
    unsigned char b[4];
    if (inet_pton(AF_INET, cp, b) != 1) return 0xffffffffu;
    uint32_t v; memcpy(&v, b, 4); return v;
}

/* getaddrinfo double: hand back one POSIX-layout node for a numeric addr. */
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    (void)service;
    if (!node || !res) return EAI_NONAME;
    struct sockaddr_in *sa = calloc(1, sizeof *sa);
    struct addrinfo *ai = calloc(1, sizeof *ai);
    if (!sa || !ai) { free(sa); free(ai); return EAI_MEMORY; }
    sa->sin_family = AF_INET;
    inet_pton(AF_INET, node, &sa->sin_addr);
    ai->ai_family = AF_INET;
    ai->ai_socktype = hints ? hints->ai_socktype : 0;
    ai->ai_addrlen = sizeof *sa;
    ai->ai_addr = (struct sockaddr *)sa;
    *res = ai;
    return 0;
}
void freeaddrinfo(struct addrinfo *res) {
    while (res) { struct addrinfo *n = res->ai_next; free(res->ai_addr);
                  free(res); res = n; }
}
struct hostent *gethostbyname(const char *name) {
    static struct hostent he;
    static uint32_t addr;
    static char *al[2];
    static char *alias[1] = { NULL };
    inet_pton(AF_INET, name, &addr);
    al[0] = (char *)&addr; al[1] = NULL;
    he.h_name = (char *)name; he.h_aliases = alias;
    he.h_addrtype = AF_INET; he.h_length = 4; he.h_addr_list = al;
    return &he;
}

/* Now pull in the unit under test. */
#include "../../w32/src/ws2_32.c"

/* ---- kernel32 / user32 doubles (the cross-module surface ws2_32 drives) --
 * ws2_32.c's readiness layer calls into kernel32's event objects and user32's
 * PostMessageW; model them just richly enough to assert the wiring.  W32ABI so
 * the calling convention matches the extern declarations in the unit. */
struct dbl_event { int signaled; };
W32ABI void *CreateEventW(void *sec, int manual, int initial, const void *nm) {
    (void)sec; (void)manual; (void)nm;
    struct dbl_event *e = calloc(1, sizeof *e);
    if (e) e->signaled = initial ? 1 : 0;
    return e;
}
W32ABI int SetEvent(void *h)   { if (h) ((struct dbl_event *)h)->signaled = 1; return 1; }
W32ABI int ResetEvent(void *h) { if (h) ((struct dbl_event *)h)->signaled = 0; return 1; }
W32ABI int CloseHandle(void *h){ free(h); return 1; }
W32ABI uint32_t WaitForMultipleObjects(uint32_t nn, void **hs, int all, uint32_t ms) {
    (void)ms;
    int any = 0, every = 1, first = -1;
    for (uint32_t i = 0; i < nn; i++) {
        int sig = hs[i] && ((struct dbl_event *)hs[i])->signaled;
        if (sig && first < 0) first = (int)i;
        any = any || sig; every = every && sig;
    }
    if (all ? every : any) return (uint32_t)(first < 0 ? 0 : first);  /* WAIT_OBJECT_0+i */
    return 0x00000102u;   /* WAIT_TIMEOUT */
}
static void   *dbl_post_hwnd;
static unsigned dbl_post_msg;
static uint64_t dbl_post_wp;
static int64_t  dbl_post_lp;
static int      dbl_post_count;
W32ABI int PostMessageW(void *hwnd, uint32_t msg, uint64_t wp, int64_t lp) {
    dbl_post_hwnd = hwnd; dbl_post_msg = msg; dbl_post_wp = wp; dbl_post_lp = lp;
    dbl_post_count++;
    return 1;
}
static void (W32ABI *dbl_registered_pump)(void);
W32ABI void w32_user32_set_socket_pump(void (W32ABI *fn)(void)) {
    dbl_registered_pump = fn;
}

/* ---- harness ---------------------------------------------------------- */
static int n, f;
#define CHECK(x) do { ++n; if (!(x)) { ++f; \
    fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); } } while (0)

/* mingw x64 fd_set / addrinfo layouts, as the adapter expects them. */
struct wfdset { unsigned int fd_count; uintptr_t fd_array[64]; };
struct waddrinfo { int ai_flags, ai_family, ai_socktype, ai_protocol;
                   size_t ai_addrlen; char *ai_canonname;
                   struct sockaddr *ai_addr; struct waddrinfo *ai_next; };

int main(void) {
    struct ws2_wsadata wsa;

    /* ---- version negotiation ---- */
    CHECK(w32_ws2_WSAStartup(0x0303, &wsa) == W32_WSAVERNOTSUPPORTED); /* 3.3 */
    CHECK(wsa.wHighVersion == 0x0202);
    CHECK(w32_ws2_WSAStartup(0x0202, &wsa) == 0);                      /* 2.2 */
    CHECK(wsa.wVersion == 0x0202 && wsa.wHighVersion == 0x0202);
    CHECK(w32_ws2_WSAStartup(0x0101, &wsa) == 0);                      /* 1.1 */
    CHECK(wsa.wVersion == 0x0101);

    /* ---- byte order ---- */
    CHECK(w32_ws2_htons(0x1234) == 0x3412);
    CHECK(w32_ws2_ntohs(0x3412) == 0x1234);
    CHECK(w32_ws2_ntohl(w32_ws2_htonl(0xdeadbeefu)) == 0xdeadbeefu);

    /* ---- socket + bind + getsockname cache ---- */
    W32_SOCKET s = w32_ws2_socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s != W32_INVALID_SOCKET);
    struct sockaddr_in la; memset(&la, 0, sizeof la);
    la.sin_family = AF_INET; la.sin_port = w32_ws2_htons(8080);
    la.sin_addr.s_addr = 0;
    CHECK(w32_ws2_bind(s, &la, sizeof la) == 0);
    struct sockaddr_in got; int gl = sizeof got;
    CHECK(w32_ws2_getsockname(s, &got, &gl) == 0);
    CHECK(w32_ws2_ntohs(got.sin_port) == 8080);

    /* getpeername before any peer -> WSAENOTCONN */
    g_lasterr = 0;
    CHECK(w32_ws2_getpeername(s, &got, &gl) == W32_SOCKET_ERROR);
    CHECK(g_lasterr == W32_WSAENOTCONN);

    /* ---- accept fills + caches the peer ---- */
    dbl_accept_ip = w32_ws2_htonl(0x0A000202u);   /* 10.0.2.2 */
    dbl_accept_port = w32_ws2_htons(54321);
    struct sockaddr_in pa; int pl = sizeof pa;
    W32_SOCKET c = w32_ws2_accept(s, &pa, &pl);
    CHECK(c != W32_INVALID_SOCKET);
    CHECK(w32_ws2_ntohl(pa.sin_addr.s_addr) == 0x0A000202u);
    CHECK(w32_ws2_ntohs(pa.sin_port) == 54321);
    struct sockaddr_in pn; int pnl = sizeof pn;
    CHECK(w32_ws2_getpeername(c, &pn, &pnl) == 0);
    CHECK(w32_ws2_ntohl(pn.sin_addr.s_addr) == 0x0A000202u);

    /* ---- errno -> WSA on a failing verb ---- */
    dbl_fail_next = 1; dbl_last_errno_set = ECONNREFUSED;
    CHECK(w32_ws2_connect(c, &la, sizeof la) == W32_SOCKET_ERROR);
    CHECK(g_lasterr == W32_WSAECONNREFUSED);

    /* ---- select fd_set translation ---- */
    struct wfdset rd; rd.fd_count = 1; rd.fd_array[0] = (uintptr_t)s;
    dbl_sel_ready_fd = (int)s;
    struct { int32_t sec, usec; } tv = { 0, 0 };
    int sr = w32_ws2_select(0, &rd, NULL, NULL, &tv);
    CHECK(sr == 1);
    CHECK(dbl_sel_maxfd == (int)s + 1);              /* nfds recomputed */
    CHECK(rd.fd_count == 1 && rd.fd_array[0] == (uintptr_t)s);
    /* nothing ready -> the set empties */
    rd.fd_count = 1; rd.fd_array[0] = (uintptr_t)s; dbl_sel_ready_fd = -1;
    CHECK(w32_ws2_select(0, &rd, NULL, NULL, &tv) == 0);
    CHECK(rd.fd_count == 0);

    /* __WSAFDIsSet */
    rd.fd_count = 1; rd.fd_array[0] = (uintptr_t)c;
    CHECK(w32_ws2___WSAFDIsSet(c, &rd) == 1);
    CHECK(w32_ws2___WSAFDIsSet(s, &rd) == 0);

    /* ---- ioctlsocket ---- */
    uint32_t nb = 1;
    CHECK(w32_ws2_ioctlsocket(c, W32_FIONBIO, &nb) == 0);
    uint32_t avail = 999;
    CHECK(w32_ws2_ioctlsocket(c, W32_FIONREAD, &avail) == 0 && avail == 0);
    g_lasterr = 0;
    CHECK(w32_ws2_ioctlsocket(c, 0x1234, &nb) == W32_SOCKET_ERROR);
    CHECK(g_lasterr == W32_WSAEINVAL);

    /* ---- option surface: known accepted, unknown refused by number ---- */
    int one = 1;
    CHECK(w32_ws2_setsockopt(s, 0xffff, 0x0004, (char *)&one, sizeof one) == 0);
    CHECK(w32_ws2_setsockopt(c, 6, 0x0001, (char *)&one, sizeof one) == 0);
    g_lasterr = 0;
    CHECK(w32_ws2_setsockopt(s, 0xffff, 0x7abc, (char *)&one, sizeof one)
          == W32_SOCKET_ERROR);
    CHECK(g_lasterr == W32_WSAENOPROTOOPT);

    /* ---- getaddrinfo -> Windows ADDRINFOA layout ---- */
    void *res = NULL;
    CHECK(w32_ws2_getaddrinfo("192.0.2.7", NULL, NULL, &res) == 0 && res);
    struct waddrinfo *w = (struct waddrinfo *)res;
    CHECK(w->ai_family == AF_INET);
    CHECK(w->ai_addrlen == sizeof(struct sockaddr_in));
    CHECK(w->ai_addr != NULL);
    {
        struct sockaddr_in *rin = (struct sockaddr_in *)w->ai_addr;
        char ip[32];
        CHECK(w32_ws2_inet_ntop(AF_INET, &rin->sin_addr, ip, sizeof ip) != NULL);
        CHECK(strcmp(ip, "192.0.2.7") == 0);
    }
    w32_ws2_freeaddrinfo(res);

    /* ---- inet_pton / inet_addr ---- */
    struct in_addr ia;
    CHECK(w32_ws2_inet_pton(AF_INET, "1.2.3.4", &ia) == 1);
    CHECK(w32_ws2_inet_pton(AF_INET, "bogus", &ia) == 0);
    CHECK(w32_ws2_inet_addr("255.255.255.255") == 0xffffffffu);

    /* ---- gethostname ---- */
    char hn[32];
    CHECK(w32_ws2_gethostname(hn, sizeof hn) == 0);
    CHECK(strcmp(hn, "auralite") == 0);

    /* ---- last-error round trip ---- */
    w32_ws2_WSASetLastError(4242);
    CHECK(w32_ws2_WSAGetLastError() == 4242);

    /* ---- inet_ntoa (in_addr by value) ---- */
    {
        uint32_t a; unsigned char nb2[4] = { 1, 2, 3, 4 };
        memcpy(&a, nb2, 4);
        CHECK(strcmp(w32_ws2_inet_ntoa(a), "1.2.3.4") == 0);
    }

    /* ---- getservbyname / getservbyport (well-known table) ---- */
    {
        struct ws2_servent *se =
            (struct ws2_servent *)w32_ws2_getservbyname("ssh", "tcp");
        CHECK(se != NULL);
        CHECK(se && __builtin_bswap16((unsigned short)se->s_port) == 22);
        CHECK(se && strcmp(se->s_name, "ssh") == 0);
        struct ws2_servent *sp = (struct ws2_servent *)
            w32_ws2_getservbyport((int)__builtin_bswap16(443), "tcp");
        CHECK(sp && strcmp(sp->s_name, "https") == 0);
        g_lasterr = 0;
        CHECK(w32_ws2_getservbyname("no-such-svc", "tcp") == NULL);
        CHECK(g_lasterr == W32_WSANO_DATA);
    }

    /* ---- getnameinfo: numeric host, named + numeric service ---- */
    {
        struct sockaddr_in a; memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = w32_ws2_htons(22);
        inet_pton(AF_INET, "10.0.0.5", &a.sin_addr);
        char host[64], serv[32];
        CHECK(w32_ws2_getnameinfo(&a, sizeof a, host, sizeof host,
                                  serv, sizeof serv, 0) == 0);
        CHECK(strcmp(host, "10.0.0.5") == 0);
        CHECK(strcmp(serv, "ssh") == 0);              /* named */
        CHECK(w32_ws2_getnameinfo(&a, sizeof a, host, sizeof host,
                                  serv, sizeof serv, WS2_NI_NUMERICSERV) == 0);
        CHECK(strcmp(serv, "22") == 0);               /* numeric */
    }

    /* ---- WSAAddressToStringA / WSAStringToAddressA round trip ---- */
    {
        struct sockaddr_in a; memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = w32_ws2_htons(443);
        inet_pton(AF_INET, "192.0.2.7", &a.sin_addr);
        char buf[64]; W32_DWORD bl = sizeof buf;
        CHECK(w32_ws2_WSAAddressToStringA(&a, sizeof a, NULL, buf, &bl) == 0);
        CHECK(strcmp(buf, "192.0.2.7:443") == 0);
        struct sockaddr_in b; int blen = sizeof b;
        char in[] = "10.0.0.1:22";
        CHECK(w32_ws2_WSAStringToAddressA(in, AF_INET, NULL, &b, &blen) == 0);
        CHECK(w32_ws2_ntohs(b.sin_port) == 22);
        char ip[32];
        CHECK(w32_ws2_inet_ntop(AF_INET, &b.sin_addr, ip, sizeof ip) &&
              strcmp(ip, "10.0.0.1") == 0);
    }

    /* ---- WSAIoctl dispatch ---- */
    {
        uint32_t on = 1, out = 7; W32_DWORD ret = 99;
        CHECK(w32_ws2_WSAIoctl(c, (W32_DWORD)W32_FIONBIO, &on, 4,
                               NULL, 0, &ret, NULL, NULL) == 0);
        CHECK(w32_ws2_WSAIoctl(c, (W32_DWORD)W32_FIONREAD, NULL, 0,
                               &out, 4, &ret, NULL, NULL) == 0);
        CHECK(out == 0 && ret == 4);
        CHECK(w32_ws2_WSAIoctl(c, 0x98000004u, NULL, 0, NULL, 0,
                               &ret, NULL, NULL) == 0);        /* KEEPALIVE_VALS */
        g_lasterr = 0;
        CHECK(w32_ws2_WSAIoctl(c, 0xC8000006u, NULL, 0, NULL, 0,
                               &ret, NULL, NULL) == W32_SOCKET_ERROR);
        CHECK(g_lasterr == W32_WSAEOPNOTSUPP);                 /* extension ptr */
        g_lasterr = 0;
        CHECK(w32_ws2_WSAIoctl(c, 0x1111u, NULL, 0, NULL, 0,
                               &ret, NULL, NULL) == W32_SOCKET_ERROR);
        CHECK(g_lasterr == W32_WSAEINVAL);
    }

    /* ---- a fresh socket for the readiness / notification surface ---- */
    W32_SOCKET c2 = w32_ws2_socket(AF_INET, SOCK_STREAM, 0);
    CHECK(c2 != W32_INVALID_SOCKET);
    int fd2 = (int)c2;

    /* non-blocking recv/send honour a zero-timeout select() ---- */
    {
        uint32_t on = 1;
        CHECK(w32_ws2_ioctlsocket(c2, W32_FIONBIO, &on) == 0);
        dbl_sel_ready_fd = -1; dbl_sel_wready_fd = -1;   /* nothing ready */
        char b[4] = {0};
        g_lasterr = 0;
        CHECK(w32_ws2_recv(c2, b, sizeof b, 0) == W32_SOCKET_ERROR);
        CHECK(g_lasterr == W32_WSAEWOULDBLOCK);
        g_lasterr = 0;
        CHECK(w32_ws2_send(c2, b, sizeof b, 0) == W32_SOCKET_ERROR);
        CHECK(g_lasterr == W32_WSAEWOULDBLOCK);
        /* now readable/writable -> the verbs proceed */
        dbl_sel_ready_fd = fd2; dbl_sel_wready_fd = fd2;
        CHECK(w32_ws2_recv(c2, b, sizeof b, 0) == (int)sizeof b);
        CHECK(w32_ws2_send(c2, b, sizeof b, 0) == (int)sizeof b);
    }

    /* ---- WSA event objects over the kernel32 doubles ---- */
    void *ev = w32_ws2_WSACreateEvent();
    CHECK(ev != NULL);
    CHECK(((struct dbl_event *)ev)->signaled == 0);
    CHECK(w32_ws2_WSASetEvent(ev) == 1 && ((struct dbl_event *)ev)->signaled == 1);
    CHECK(w32_ws2_WSAResetEvent(ev) == 1 && ((struct dbl_event *)ev)->signaled == 0);

    /* ---- WSAEventSelect + WSAEnumNetworkEvents (plink's model) ---- */
    {
        CHECK(w32_ws2_WSAEventSelect(c2, ev, 0x01 /*FD_READ*/) == 0);
        dbl_sel_ready_fd = fd2; dbl_sel_wready_fd = -1;
        struct ws2_networkevents ne;
        CHECK(w32_ws2_WSAEnumNetworkEvents(c2, ev, &ne) == 0);
        CHECK(ne.lNetworkEvents == 0x01);            /* FD_READ reported */
        CHECK(((struct dbl_event *)ev)->signaled == 0);  /* enum reset it */
    }

    /* ---- WSAWaitForMultipleEvents pumps readiness into the event ---- */
    {
        w32_ws2_WSAResetEvent(ev);
        dbl_sel_ready_fd = fd2;                       /* c2 read-ready */
        /* ev is bound to c2 via WSAEventSelect above; the pump must SetEvent */
        W32_DWORD r = w32_ws2_WSAWaitForMultipleEvents(1, &ev, 0, 100, 0);
        CHECK(r == 0);                               /* WSA_WAIT_EVENT_0 */
    }

    /* ---- WSAAsyncSelect delivers a window message through the pump ---- */
    {
        CHECK(w32_ws2_WSAEventSelect(c2, NULL, 0) == 0);   /* drop event reg */
        CHECK(dbl_registered_pump != NULL);                /* set at startup */
        void *hwnd = (void *)0x1234;
        CHECK(w32_ws2_WSAAsyncSelect(c2, hwnd, 0x0400 /*WM_USER*/,
                                     0x01 /*FD_READ*/) == 0);
        dbl_sel_ready_fd = fd2; dbl_sel_wready_fd = -1;
        int before = dbl_post_count;
        dbl_registered_pump();                             /* == w32_ws2_pump */
        CHECK(dbl_post_count == before + 1);
        CHECK(dbl_post_hwnd == hwnd && dbl_post_msg == 0x0400);
        CHECK(dbl_post_wp == (uint64_t)fd2);
        CHECK((dbl_post_lp & 0xffff) == 0);                /* FD_READ_BIT == 0 */
        /* edge-latched: a second pump with the level unchanged posts nothing */
        dbl_registered_pump();
        CHECK(dbl_post_count == before + 1);
        CHECK(w32_ws2_WSAAsyncSelect(c2, hwnd, 0x0400, 0) == 0);  /* cancel */
    }
    CHECK(w32_ws2_WSACloseEvent(ev) == 1);
    CHECK(w32_ws2_closesocket(c2) == 0);
    dbl_sel_ready_fd = -1; dbl_sel_wready_fd = -1;

    /* ---- close + cleanup + WSANOTINITIALISED ---- */
    CHECK(w32_ws2_closesocket(c) == 0);
    CHECK(w32_ws2_closesocket(s) == 0);
    /* two successful WSAStartup calls above (2.2 and 1.1; the 3.3 refusal
     * did not count) -> two cleanups, then a third reports
     * WSANOTINITIALISED. */
    CHECK(w32_ws2_WSACleanup() == 0);
    CHECK(w32_ws2_WSACleanup() == 0);
    g_lasterr = 0;
    CHECK(w32_ws2_WSACleanup() == W32_SOCKET_ERROR);
    CHECK(g_lasterr == W32_WSANOTINITIALISED);
    /* socket() now refuses: not initialised. */
    g_lasterr = 0;
    CHECK(w32_ws2_socket(AF_INET, SOCK_STREAM, 0) == W32_INVALID_SOCKET);
    CHECK(g_lasterr == W32_WSANOTINITIALISED);

    fprintf(stderr, "w32a12-ws2_32: %d checks, %d failures\n", n, f);
    return f ? 1 : 0;
}
