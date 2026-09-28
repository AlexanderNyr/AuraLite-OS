/* w32/tests/w32a12_winsock.c — W32APP_PLAN.md phase W32A-12 guest fixture.
 *
 * A single mingw-w64 TU linked -nostdlib with --entry=winstart (no CRT, no
 * main): it imports WS2_32 BY NAME (a real -lws2_32 link) and KERNEL32 for
 * output, exercises the WinSock surface REAL over AuraLite's native socket
 * stack, and exits 78 on success / 1 on failure.
 *
 * Beyond the core socket verbs it exercises the rest of the surface the
 * pinned PuTTY/plink binaries resolve dynamically (w32/tests/W32A12.probe.log):
 * inet_ntoa, getservbyname/getservbyport, getnameinfo, WSAAddressToStringA/
 * WSAStringToAddressA, the WSAIoctl control-code dispatch, the WSA event
 * objects (REAL over kernel32 events), and the readiness-notification pair
 * WSAEventSelect + WSAEnumNetworkEvents plus WSAAsyncSelect registration.
 *
 * The loopback echo is self-contained the way tcpserver's H5 gate is: the
 * kernel's integration-test fallback (kernel/net/tcp.c) synthesises a SYN
 * from 10.0.2.2:54321 for a listening port and, on the accepted socket,
 * hands back a canned "GET / HTTP/1.0" request — so bind/listen/accept/
 * recv/send run end to end with no external peer.
 *
 * Markers greppable by tests/integration/cases/test_w32a12_winsock.sh.
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>

/* -nostdlib: the compiler may still emit these for copies/inits. */
void *memset(void *d, int c, unsigned long long n) {
    volatile unsigned char *p = (volatile unsigned char *)d;
    while (n-- > 0) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long long n) {
    volatile unsigned char *p = (volatile unsigned char *)d;
    volatile const unsigned char *q = (volatile const unsigned char *)s;
    while (n-- > 0) *p++ = *q++;
    return d;
}
unsigned long long strlen(const char *s) {
    unsigned long long n = 0;
    while (s[n]) n++;
    return n;
}
static int streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int nstarts(const char *hay, const char *pre) {
    while (*pre) { if (*hay != *pre) return 0; hay++; pre++; }
    return 1;
}

static HANDLE out;
static DWORD  written;
static int    fails;

static void say(const char *s) {
    DWORD n = 0;
    while (s[n]) n++;
    WriteFile(out, s, n, &written, NULL);
}
static void mark_fail(const char *m) {
    say("FAIL-"); say(m); say("\r\n"); fails++;
}
#define CHECK(cond, mark) do { if (!(cond)) mark_fail(mark); } while (0)

void __stdcall winstart(void) {
    WSADATA wsa;
    int r;

    out = GetStdHandle(STD_OUTPUT_HANDLE);
    say("W32A12: winsock fixture start\r\n");

    /* ---- version negotiation ---------------------------------------- */
    r = WSAStartup(MAKEWORD(3, 3), &wsa);          /* above 2.2 -> refuse */
    CHECK(r == WSAVERNOTSUPPORTED, "ver-refuse");

    r = WSAStartup(MAKEWORD(2, 2), &wsa);          /* 2.2 -> accept */
    CHECK(r == 0, "startup");
    CHECK(wsa.wVersion == 0x0202, "startup-wVersion");
    CHECK(wsa.wHighVersion == 0x0202, "startup-wHighVersion");

    /* ---- byte order ------------------------------------------------- */
    CHECK(htons(0x1234) == 0x3412, "htons");
    CHECK(ntohs(0x3412) == 0x1234, "ntohs");
    CHECK(ntohl(htonl(0x01020304u)) == 0x01020304u, "htonl-roundtrip");

    /* ---- socket / bind / getsockname -------------------------------- */
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s != INVALID_SOCKET, "socket");

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(8080);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    CHECK(bind(s, (struct sockaddr *)&sa, sizeof sa) == 0, "bind");

    struct sockaddr_in local;
    int locallen = sizeof local;
    memset(&local, 0, sizeof local);
    CHECK(getsockname(s, (struct sockaddr *)&local, &locallen) == 0, "getsockname");
    CHECK(ntohs(local.sin_port) == 8080, "getsockname-port");

    CHECK(listen(s, 5) == 0, "listen");

    /* ---- accept (kernel test fallback: peer 10.0.2.2:54321) --------- */
    struct sockaddr_in cli;
    int clilen = sizeof cli;
    memset(&cli, 0, sizeof cli);
    SOCKET c = accept(s, (struct sockaddr *)&cli, &clilen);
    CHECK(c != INVALID_SOCKET, "accept");

    /* accept filled the peer address; getpeername must agree. */
    CHECK(ntohl(cli.sin_addr.s_addr) == 0x0A000202u, "accept-peer-ip");
    CHECK(ntohs(cli.sin_port) == 54321, "accept-peer-port");

    struct sockaddr_in peer;
    int peerlen = sizeof peer;
    memset(&peer, 0, sizeof peer);
    CHECK(getpeername(c, (struct sockaddr *)&peer, &peerlen) == 0, "getpeername");
    CHECK(ntohl(peer.sin_addr.s_addr) == 0x0A000202u, "getpeername-ip");
    CHECK(ntohs(peer.sin_port) == 54321, "getpeername-port");

    /* ---- recv (canned request) + send ------------------------------- */
    char buf[512];
    memset(buf, 0, sizeof buf);
    int n = recv(c, buf, sizeof buf - 1, 0);
    CHECK(n > 0, "recv");
    CHECK(nstarts(buf, "GET /"), "recv-content");

    const char *resp = "HTTP/1.0 200 OK\r\n\r\nhi";
    int sent = send(c, resp, (int)strlen(resp), 0);
    CHECK(sent == (int)strlen(resp), "send");

    /* ---- select + __WSAFDIsSet -------------------------------------- */
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    struct timeval tv;
    tv.tv_sec = 0; tv.tv_usec = 0;
    int sel = select(0, &rd, NULL, NULL, &tv);
    CHECK(sel >= 0, "select");   /* 0 (nothing pending) or a ready count */

    /* ---- ioctlsocket ------------------------------------------------ */
    u_long nb = 1;
    CHECK(ioctlsocket(c, FIONBIO, &nb) == 0, "fionbio");
    u_long avail = 123;
    CHECK(ioctlsocket(c, FIONREAD, &avail) == 0, "fionread");

    /* ---- setsockopt: observed accepted, unobserved refused --------- */
    int one = 1;
    CHECK(setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                     (const char *)&one, sizeof one) == 0, "so-reuseaddr");
    CHECK(setsockopt(c, IPPROTO_TCP, TCP_NODELAY,
                     (const char *)&one, sizeof one) == 0, "tcp-nodelay");
    r = setsockopt(s, SOL_SOCKET, 0x7abc, (const char *)&one, sizeof one);
    CHECK(r == SOCKET_ERROR, "opt-refuse-ret");
    CHECK(WSAGetLastError() == WSAENOPROTOOPT, "opt-refuse-err");

    /* ---- shutdown + close ------------------------------------------ */
    CHECK(shutdown(c, SD_SEND) == 0, "shutdown");
    CHECK(closesocket(c) == 0, "close-client");
    CHECK(closesocket(s) == 0, "close-server");

    /* ---- resolution: getaddrinfo (Windows layout) + inet_ntop ------ */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    r = getaddrinfo("127.0.0.1", NULL, &hints, &res);
    CHECK(r == 0 && res != NULL, "getaddrinfo");
    if (r == 0 && res) {
        CHECK(res->ai_family == AF_INET, "gai-family");
        CHECK(res->ai_addrlen == sizeof(struct sockaddr_in), "gai-addrlen");
        struct sockaddr_in *rin = (struct sockaddr_in *)res->ai_addr;
        char ipbuf[64];
        memset(ipbuf, 0, sizeof ipbuf);
        const char *p = inet_ntop(AF_INET, &rin->sin_addr, ipbuf, sizeof ipbuf);
        CHECK(p != NULL && streq(ipbuf, "127.0.0.1"), "gai-inet-ntop");
        freeaddrinfo(res);
    }

    /* ---- inet_pton / inet_ntop round trip -------------------------- */
    struct in_addr ia;
    CHECK(inet_pton(AF_INET, "1.2.3.4", &ia) == 1, "inet-pton");
    char pbuf[64];
    memset(pbuf, 0, sizeof pbuf);
    CHECK(inet_ntop(AF_INET, &ia, pbuf, sizeof pbuf) != NULL &&
          streq(pbuf, "1.2.3.4"), "inet-ntop-roundtrip");
    CHECK(inet_pton(AF_INET, "not.an.ip", &ia) == 0, "inet-pton-bad");

    /* ---- gethostname ----------------------------------------------- */
    char hn[64];
    memset(hn, 0, sizeof hn);
    CHECK(gethostname(hn, sizeof hn) == 0, "gethostname");

    /* ---- per-thread WSAGetLastError / WSASetLastError -------------- */
    WSASetLastError(0);
    CHECK(WSAGetLastError() == 0, "setlasterror-clear");
    r = recv((SOCKET)0x7fffffff, buf, 1, 0);   /* not a socket */
    CHECK(r == SOCKET_ERROR, "recv-badsock-ret");
    CHECK(WSAGetLastError() != 0, "recv-badsock-err");
    WSASetLastError(12345);
    CHECK(WSAGetLastError() == 12345, "setlasterror-roundtrip");

    /* ---- inet_ntoa (in_addr by value) ------------------------------ */
    {
        struct in_addr t; t.s_addr = htonl(0x0A000205u);   /* 10.0.2.5 */
        char *d = inet_ntoa(t);
        CHECK(d != NULL && streq(d, "10.0.2.5"), "inet-ntoa");
    }

    /* ---- getservbyname / getservbyport (well-known table) ---------- */
    {
        struct servent *se = getservbyname("ssh", "tcp");
        CHECK(se != NULL && ntohs((u_short)se->s_port) == 22, "getservbyname-ssh");
        struct servent *sp = getservbyport((int)htons(443), "tcp");
        CHECK(sp != NULL && streq(sp->s_name, "https"), "getservbyport-https");
    }

    /* ---- getnameinfo: numeric host + named/numeric service --------- */
    {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons(22);
        inet_pton(AF_INET, "10.0.0.5", &a.sin_addr);
        char host[64], serv[32];
        memset(host, 0, sizeof host); memset(serv, 0, sizeof serv);
        r = getnameinfo((struct sockaddr *)&a, sizeof a,
                        host, sizeof host, serv, sizeof serv, 0);
        CHECK(r == 0 && streq(host, "10.0.0.5") && streq(serv, "ssh"),
              "getnameinfo-named");
        memset(serv, 0, sizeof serv);
        r = getnameinfo((struct sockaddr *)&a, sizeof a,
                        host, sizeof host, serv, sizeof serv, NI_NUMERICSERV);
        CHECK(r == 0 && streq(serv, "22"), "getnameinfo-numeric");
    }

    /* ---- WSAAddressToStringA / WSAStringToAddressA round trip ------- */
    {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons(443);
        inet_pton(AF_INET, "192.0.2.7", &a.sin_addr);
        char sbuf[64]; DWORD sl = sizeof sbuf;
        memset(sbuf, 0, sizeof sbuf);
        r = WSAAddressToStringA((struct sockaddr *)&a, sizeof a, NULL, sbuf, &sl);
        CHECK(r == 0 && streq(sbuf, "192.0.2.7:443"), "addr-to-string");
        struct sockaddr_in b; int bl = sizeof b;
        memset(&b, 0, sizeof b);
        char in[] = "10.0.0.1:22";
        r = WSAStringToAddressA(in, AF_INET, NULL, (struct sockaddr *)&b, &bl);
        CHECK(r == 0 && ntohs(b.sin_port) == 22, "string-to-addr");
    }

    /* ---- WSAIoctl control-code dispatch ---------------------------- */
    {
        SOCKET t = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(t != INVALID_SOCKET, "ioctl-socket");
        u_long on = 1; DWORD ret = 0;
        CHECK(WSAIoctl(t, FIONBIO, &on, sizeof on, NULL, 0, &ret, NULL, NULL) == 0,
              "wsaioctl-fionbio");
        DWORD keep[3] = { 1, 1000, 1000 };
        CHECK(WSAIoctl(t, SIO_KEEPALIVE_VALS, keep, sizeof keep,
                       NULL, 0, &ret, NULL, NULL) == 0, "wsaioctl-keepalive");
        closesocket(t);
    }

    /* ---- WSA event objects (REAL over kernel32) -------------------- */
    WSAEVENT ev = WSACreateEvent();
    CHECK(ev != WSA_INVALID_EVENT && ev != NULL, "wsacreateevent");
    CHECK(WSASetEvent(ev), "wsasetevent");
    CHECK(WSAResetEvent(ev), "wsaresetevent");

    /* ---- WSAEventSelect + WSAEnumNetworkEvents (plink's model) ----- */
    {
        SOCKET t = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(t != INVALID_SOCKET, "es-socket");
        CHECK(WSAEventSelect(t, ev, FD_READ | FD_WRITE | FD_CONNECT) == 0,
              "eventselect");
        WSANETWORKEVENTS ne;
        memset(&ne, 0, sizeof ne);
        CHECK(WSAEnumNetworkEvents(t, ev, &ne) == 0, "enumnetworkevents");
        CHECK(WSAEventSelect(t, NULL, 0) == 0, "eventselect-cancel");
        closesocket(t);
    }

    /* ---- WSAAsyncSelect registration + cancel (no msg loop here) --- */
    {
        SOCKET t = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(t != INVALID_SOCKET, "as-socket");
        CHECK(WSAAsyncSelect(t, (HWND)0, 0x0400 /*WM_USER*/, FD_READ) == 0,
              "asyncselect");
        CHECK(WSAAsyncSelect(t, (HWND)0, 0x0400, 0) == 0, "asyncselect-cancel");
        closesocket(t);
    }

    CHECK(WSACloseEvent(ev), "wsacloseevent");

    /* ---- cleanup ---------------------------------------------------- */
    CHECK(WSACleanup() == 0, "cleanup");

    if (fails == 0) {
        say("W32A12-WINSOCK-OK\r\n");
        ExitProcess(78);
    }
    say("W32A12-WINSOCK-FAIL\r\n");
    ExitProcess(1);
}
