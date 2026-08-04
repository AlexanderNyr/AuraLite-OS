/* socktest — FIX_R7 gate: socket syscalls return SPECIFIC negative errnos.
 *
 * Asserts, in one run (each CHECK prints its own verdict; a run is green
 * only if all of them pass):
 *
 *   1. connect() to a closed port          -> ECONNREFUSED   (test gate)
 *   2. connect() to an unroutable address  -> EHOSTUNREACH   (test gate)
 *   3. send/recv on a closed socket        -> EBADF          (test gate)
 *   4. socket table exhaustion             -> EMFILE
 *   5. an unsupported address family       -> EAFNOSUPPORT
 *   6. strerror()/perror() NAME the cause ("Connection refused"), so a
 *      bare -1-style "Unknown error" is a fail.
 *
 * Values are asserted on the SPECIFIC errno: a wrong-but-nonzero errno is a
 * FAIL, exactly as the R7 gate demands.
 *
 * Runs under QEMU user networking (SLIRP): the SLIRP host 10.0.2.2 answers
 * a closed-port TCP connect with RST, while no machine on the 10.0.2.0/24
 * virtual segment answers ARP for an unused address, which is what makes
 * leg 2 deterministic.
 */

#include "stdio.h"
#include "string.h"
#include "errno.h"
#include "unistd.h"
#include "sys/socket.h"

static int failures = 0;
#define CHECK(cond, name) do {                                  \
    if (cond) printf("SOCKTEST PASS %s\n", name);               \
    else      { printf("SOCKTEST FAIL %s\n", name); failures++; } \
} while (0)

#define MK_IP(a,b,c,d) ((uint32_t)((a) << 24) | (uint32_t)((b) << 16) | \
                        (uint32_t)((c) << 8) | (uint32_t)(d))

/* SLIRP (QEMU user-net) endpoints: host/gateway and an unused guest-side
 * address no ARP reply can ever come from. */
#define SLIRP_HOST_IP   MK_IP(10, 0, 2, 2)
#define DEAD_HOST_IP    MK_IP(10, 0, 2, 222)
#define CLOSED_PORT     65000

int main(void) {
    /* ---- 1. A closed port refuses, and names why ---- */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("SOCKTEST FAIL socket() itself failed (errno=%d)\n", errno);
        return 1;
    }
    errno = 0;
    int r = connect(s, SLIRP_HOST_IP, CLOSED_PORT);
    CHECK(r < 0 && errno == ECONNREFUSED,
          "connect() to a closed port yields ECONNREFUSED");
    perror("connect");   /* must print "connect: Connection refused" */

    CHECK(strstr(strerror(ECONNREFUSED), "refused") != NULL &&
          strstr(strerror(EHOSTUNREACH), "route") != NULL,
          "strerror names the network causes");

    /* ---- 2. An unroutable (unresolvable) address is EHOSTUNREACH ---- */
    int s2 = socket(AF_INET, SOCK_STREAM, 0);
    if (s2 < 0) {
        printf("SOCKTEST FAIL socket() for leg 2 failed (errno=%d)\n", errno);
        return 1;
    }
    errno = 0;
    r = connect(s2, DEAD_HOST_IP, 80);
    CHECK(r < 0 && errno == EHOSTUNREACH,
          "connect() to an unreachable host yields EHOSTUNREACH");

    /* ---- 3. Operating on a closed socket is EBADF ---- */
    closesocket(s);
    closesocket(s2);
    errno = 0;
    char b[8];
    r = send(s, "x", 1);
    int e1 = errno;
    errno = 0;
    int r2 = recv(s, b, sizeof b);
    int e2 = errno;
    CHECK(r < 0 && e1 == EBADF, "send() on a closed socket yields EBADF");
    CHECK(r2 < 0 && e2 == EBADF, "recv() on a closed socket yields EBADF");

    /* ---- 4. The per-process socket table reports EMFILE ---- */
    int opened[40];
    int n = 0;
    int en = 0;
    for (n = 0; n < 40; n++) {
        errno = 0;
        opened[n] = socket(AF_INET, SOCK_DGRAM, 0);
        if (opened[n] < 0) { en = errno; break; }
    }
    CHECK(n == 32 && en == EMFILE,
          "the 33rd socket() of a process fails with EMFILE");
    for (int i = 0; i < n; i++) closesocket(opened[i]);

    /* ---- 5. An unsupported address family is EAFNOSUPPORT ---- */
    errno = 0;
    r = socket(10 /* AF_INET6 */, SOCK_STREAM, 0);
    CHECK(r < 0 && errno == EAFNOSUPPORT,
          "socket(AF_INET6) yields EAFNOSUPPORT");

    if (failures == 0) {
        printf("SOCKTEST ALL PASS\n");
        return 0;
    }
    printf("SOCKTEST FAILURES: %d\n", failures);
    return 1;
}
