/* W32A-12: WS2_32 WinSock over the native socket stack (syscalls 300-307)
 * + the DNS resolver.  Typed Windows-ABI contracts; the implementations in
 * ws2_32.c are thin adapters over AuraLite's libc socket surface.
 *
 * Every symbol here is prefixed w32_ws2_ so the file can call the libc
 * socket API (socket/bind/connect/... share the Windows names) without a
 * definition clash; w32_bind.c maps the Windows export string onto the
 * prefixed C symbol.
 *
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef AURALITE_W32_WS2_32_H
#define AURALITE_W32_WS2_32_H

#include "w32_abi.h"
#include <stdint.h>
#include <stddef.h>

/* SOCKET is UINT_PTR on Windows; INVALID_SOCKET is all-ones, SOCKET_ERROR
 * is -1.  Native fds are small non-negative ints, which fit unchanged. */
typedef uintptr_t W32_SOCKET;
#define W32_INVALID_SOCKET  ((W32_SOCKET)~(uintptr_t)0)
#define W32_SOCKET_ERROR    (-1)

/* ioctlsocket commands (published _IOR/_IOW encodings). */
#define W32_FIONREAD  0x4004667FL   /* bytes available to read */
#define W32_FIONBIO   0x8004667EL   /* set/clear non-blocking */

/* WSAStartup version words: LOBYTE = major, HIBYTE = minor. */
#define W32_MAKEWORD(lo, hi) ((W32_WORD)(((lo) & 0xff) | (((hi) & 0xff) << 8)))

/* --- WSA error codes (published values, 10000-based; facts about the
 *     interface, written from documentation like w32_errno.h). --- */
#define W32_WSABASEERR            10000
#define W32_WSAEINTR              10004
#define W32_WSAEBADF              10009
#define W32_WSAEACCES             10013
#define W32_WSAEFAULT             10014
#define W32_WSAEINVAL             10022
#define W32_WSAEMFILE             10024
#define W32_WSAEWOULDBLOCK        10035
#define W32_WSAEINPROGRESS        10036
#define W32_WSAEALREADY           10037
#define W32_WSAENOTSOCK           10038
#define W32_WSAEDESTADDRREQ       10039
#define W32_WSAEMSGSIZE           10040
#define W32_WSAEPROTOTYPE         10041
#define W32_WSAENOPROTOOPT        10042
#define W32_WSAEPROTONOSUPPORT    10043
#define W32_WSAESOCKTNOSUPPORT    10044
#define W32_WSAEOPNOTSUPP         10045
#define W32_WSAEAFNOSUPPORT       10047
#define W32_WSAEADDRINUSE         10048
#define W32_WSAEADDRNOTAVAIL      10049
#define W32_WSAENETDOWN           10050
#define W32_WSAENETUNREACH        10051
#define W32_WSAENETRESET          10052
#define W32_WSAECONNABORTED       10053
#define W32_WSAECONNRESET         10054
#define W32_WSAENOBUFS            10055
#define W32_WSAEISCONN            10056
#define W32_WSAENOTCONN           10057
#define W32_WSAESHUTDOWN          10058
#define W32_WSAETIMEDOUT          10060
#define W32_WSAECONNREFUSED       10061
#define W32_WSAEHOSTDOWN          10064
#define W32_WSAEHOSTUNREACH       10065
#define W32_WSASYSNOTREADY        10091
#define W32_WSAVERNOTSUPPORTED    10092
#define W32_WSANOTINITIALISED     10093
#define W32_WSAHOST_NOT_FOUND     11001
#define W32_WSATRY_AGAIN          11002
#define W32_WSANO_RECOVERY        11003
#define W32_WSANO_DATA            11004
/* getaddrinfo's out-of-memory path reports the plain Win32 code (8), as
 * Windows does (WSA_NOT_ENOUGH_MEMORY == ERROR_NOT_ENOUGH_MEMORY). */
#define W32_WSA_NOT_ENOUGH_MEMORY_LOCAL 8

/* --- Startup / errors (REAL) --- */
W32ABI int  w32_ws2_WSAStartup(W32_WORD wVersionRequested, void *lpWSAData);
W32ABI int  w32_ws2_WSACleanup(void);
W32ABI int  w32_ws2_WSAGetLastError(void);
W32ABI void w32_ws2_WSASetLastError(int err);

/* --- Byte order (REAL, pure) --- */
W32ABI uint32_t w32_ws2_htonl(uint32_t x);
W32ABI uint32_t w32_ws2_ntohl(uint32_t x);
W32ABI uint16_t w32_ws2_htons(uint16_t x);
W32ABI uint16_t w32_ws2_ntohs(uint16_t x);

/* --- Sockets (REAL over syscalls 300-307 + the net stack) --- */
W32ABI W32_SOCKET w32_ws2_socket(int af, int type, int protocol);
W32ABI int  w32_ws2_closesocket(W32_SOCKET s);
W32ABI int  w32_ws2_bind(W32_SOCKET s, const void *name, int namelen);
W32ABI int  w32_ws2_listen(W32_SOCKET s, int backlog);
W32ABI W32_SOCKET w32_ws2_accept(W32_SOCKET s, void *addr, int *addrlen);
W32ABI int  w32_ws2_connect(W32_SOCKET s, const void *name, int namelen);
W32ABI int  w32_ws2_send(W32_SOCKET s, const char *buf, int len, int flags);
W32ABI int  w32_ws2_recv(W32_SOCKET s, char *buf, int len, int flags);
W32ABI int  w32_ws2_sendto(W32_SOCKET s, const char *buf, int len, int flags,
                           const void *to, int tolen);
W32ABI int  w32_ws2_recvfrom(W32_SOCKET s, char *buf, int len, int flags,
                             void *from, int *fromlen);
W32ABI int  w32_ws2_shutdown(W32_SOCKET s, int how);
W32ABI int  w32_ws2_select(int nfds, void *readfds, void *writefds,
                           void *exceptfds, const void *timeout);
W32ABI int  w32_ws2___WSAFDIsSet(W32_SOCKET s, void *set);
W32ABI int  w32_ws2_ioctlsocket(W32_SOCKET s, long cmd, uint32_t *argp);
W32ABI int  w32_ws2_getsockopt(W32_SOCKET s, int level, int optname,
                               char *optval, int *optlen);
W32ABI int  w32_ws2_setsockopt(W32_SOCKET s, int level, int optname,
                               const char *optval, int optlen);
W32ABI int  w32_ws2_getpeername(W32_SOCKET s, void *name, int *namelen);
W32ABI int  w32_ws2_getsockname(W32_SOCKET s, void *name, int *namelen);

/* --- Resolution / formatting (REAL over the DNS stack + a services table) --- */
W32ABI int  w32_ws2_getaddrinfo(const char *node, const char *service,
                                const void *hints, void **res);
W32ABI void w32_ws2_freeaddrinfo(void *res);
W32ABI void *w32_ws2_gethostbyname(const char *name);
W32ABI void *w32_ws2_gethostbyaddr(const char *addr, int len, int type);
W32ABI int  w32_ws2_getnameinfo(const void *sa, int salen, char *host,
                                W32_DWORD hostlen, char *serv,
                                W32_DWORD servlen, int flags);
W32ABI void *w32_ws2_getservbyname(const char *name, const char *proto);
W32ABI void *w32_ws2_getservbyport(int port, const char *proto);
W32ABI const char *w32_ws2_inet_ntop(int af, const void *src, char *dst,
                                     size_t size);
W32ABI int  w32_ws2_inet_pton(int af, const char *src, void *dst);
W32ABI uint32_t w32_ws2_inet_addr(const char *cp);
W32ABI char *w32_ws2_inet_ntoa(uint32_t addr);
W32ABI int  w32_ws2_gethostname(char *name, int namelen);
W32ABI int  w32_ws2_WSAAddressToStringA(void *sa, W32_DWORD salen,
                                        void *protoinfo, char *buf,
                                        W32_DWORD *buflen);
W32ABI int  w32_ws2_WSAStringToAddressA(char *str, int family,
                                        void *protoinfo, void *sa,
                                        int *salen);

/* --- Control codes --- */
W32ABI int  w32_ws2_WSAIoctl(W32_SOCKET s, W32_DWORD code, void *inbuf,
                             W32_DWORD inlen, void *outbuf, W32_DWORD outlen,
                             W32_DWORD *retlen, void *overlapped,
                             void *completion);

/* --- Readiness notification (REAL over the ws2_pump engine) ---
 * PuTTY's console tool (plink) drives WSAEventSelect + WSAEnumNetworkEvents
 * over a kernel32 event; the GUI drives WSAAsyncSelect, whose window messages
 * are posted from the user32 message loop via w32_ws2_pump.  The WSAEvent
 * object family maps onto kernel32 manual-reset events.  Overlapped/IOCP is
 * the only remaining documented non-goal (refused by name). */
W32ABI void w32_ws2_pump(void);   /* readiness pump; called by the msg loop */
W32ABI int  w32_ws2_WSAAsyncSelect(W32_SOCKET s, void *hWnd, unsigned msg,
                                   long ev);
W32ABI int  w32_ws2_WSAEventSelect(W32_SOCKET s, void *hEvent, long ev);
W32ABI int  w32_ws2_WSAEnumNetworkEvents(W32_SOCKET s, void *hEvent,
                                         void *out);
W32ABI void *w32_ws2_WSACreateEvent(void);
W32ABI int  w32_ws2_WSACloseEvent(void *h);
W32ABI int  w32_ws2_WSASetEvent(void *h);
W32ABI int  w32_ws2_WSAResetEvent(void *h);
W32ABI W32_DWORD w32_ws2_WSAWaitForMultipleEvents(W32_DWORD count,
                                                  const void *events,
                                                  W32_BOOL waitAll,
                                                  W32_DWORD ms,
                                                  W32_BOOL alertable);

#endif /* AURALITE_W32_WS2_32_H */
