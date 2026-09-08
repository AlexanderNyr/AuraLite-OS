/*
 * ota.c — OTA_PLAN O4: the over-the-air update tool.
 *
 * The full check -> download -> verify -> swap -> sync flow, driven from
 * the shell:  `run ota check <manifest-url>` / `run ota apply <m-url>` /
 * `run ota rollback` / `run ota status`.
 *
 * Two transports, on purpose:
 *  - the MANIFEST goes through libahttp (HTTPS-capable, 1 MiB body cap —
 *    a manifest is a few hundred bytes);
 *  - the PAYLOAD is downloaded by a minimal in-app HTTP GET that streams
 *    4 KiB chunks straight into /fat/KERNEL.NEW and hashes them on the
 *    fly with libatls — nothing ever buffers the ~2.9 MiB kernel
 *    (AHTTP_MAX_BODY is 1 MiB, and even if it were bigger, a
 *    buffer-the-whole-file design would just be a worse idea at 2x the
 *    footprint).  Streaming TLS is a named deferral in the plan (§2);
 *    an https:// payload URL is refused with a clear message here.
 *
 * The A/B swap is three renames on the ESP O1 taught the kernel to see:
 *   apply:    KERNEL.ELF -> KERNEL.OLD ; KERNEL.NEW -> KERNEL.ELF
 *   rollback: KERNEL.ELF -> KERNEL.NEW ; KERNEL.OLD -> KERNEL.ELF ;
 *             unlink KERNEL.NEW
 * so at every instant exactly one of the slots is the bootable KERNEL.ELF
 * (stage2's O3 fallback reads KERNEL.OLD if the new image is bad).
 *
 * Free-space honesty: this tree's statvfs(3) is a hardcoded stub
 * (lib/libc/src/q10_stubs.c — fake numbers, path ignored), so "size >
 * free ESP" cannot be queried honestly yet.  The guard is a sanity bound
 * (64 KiB..32 MiB — the ESP is 48 MiB) plus a write-time abort: any
 * short write (ENOSPC included) unlinks the partial KERNEL.NEW and
 * leaves KERNEL.ELF untouched.  A real statvfs is a named follow-up.
 */

#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "fcntl.h"
#include "ahttp/http.h"
#include "atls/atls.h"
#include "ota_manifest.h"

/* Guest socket ABI (same declarations libahttp uses; see ahttp.c). */
extern int socket(int, int, int);
extern int connect(int, uint32_t, uint16_t);
extern int send(int, const void *, uint32_t);
extern int recv(int, void *, uint32_t);
extern int closesocket(int);
extern uint32_t dns_resolve(const char *hostname);

#define AF_INET_GUEST 2
#define SOCK_STREAM_GUEST 1

#define CHUNK 4096
#define OTA_SIZE_MIN (64ull * 1024)          /* a kernel is never tiny */
#define OTA_SIZE_MAX (32ull * 1024 * 1024)   /* ESP is 48 MiB total */

static const char *FAT_ELF = "/fat/KERNEL.ELF";
static const char *FAT_OLD = "/fat/KERNEL.OLD";
static const char *FAT_NEW = "/fat/KERNEL.NEW";

/* ---- file hashing (status / sanity prints) ------------------------------- */

static int sha256_file(const char *path, unsigned char digest[32],
                       unsigned long long *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    static char buf[CHUNK];
    atls_sha256_ctx c;
    atls_sha256_init(&c);
    unsigned long long total = 0;
    for (;;) {
        int n = read(fd, buf, sizeof(buf));
        if (n < 0) { close(fd); return -1; }
        if (n == 0) break;
        atls_sha256_update(&c, buf, (size_t)n);
        total += (unsigned long long)n;
    }
    close(fd);
    atls_sha256_final(&c, digest);
    if (out_size) *out_size = total;
    return 0;
}

static void cmd_status(void) {
    unsigned char digest[32];
    char hex[OTA_SHA256_HEX];
    unsigned long long size;

    if (sha256_file(FAT_ELF, digest, &size) == 0) {
        ota_digest_hex(digest, hex);
        printf("[ota] active  %s: %llu bytes, sha256 %s\n",
               FAT_ELF, size, hex);
    } else {
        printf("[ota] active  %s: NOT READABLE\n", FAT_ELF);
    }
    if (sha256_file(FAT_OLD, digest, &size) == 0) {
        ota_digest_hex(digest, hex);
        printf("[ota] fallback %s: %llu bytes, sha256 %s\n",
               FAT_OLD, size, hex);
    } else {
        printf("[ota] fallback %s: none (no rollback slot)\n", FAT_OLD);
    }
    fflush(stdout);
}

/* ---- manifest fetch + validate ------------------------------------------- */

static int fetch_manifest(ahttp_client *cli, const char *url,
                          ota_manifest *m) {
    ahttp_response *r = ahttp_client_get(cli, url);
    if (!r) {
        printf("[ota] manifest fetch failed: no response object\n");
        return -1;
    }
    if (r->error != AHTTP_OK) {
        printf("[ota] manifest fetch failed: %s\n",
               ahttp_strerror(r->error, r->tls_error));
        ahttp_response_free(r);
        return -1;
    }
    if (r->status_code != 200) {
        printf("[ota] manifest fetch failed: HTTP %d\n", r->status_code);
        ahttp_response_free(r);
        return -1;
    }
    int prc = ota_manifest_parse((const char *)r->body, r->body_len, m);
    ahttp_response_free(r);
    if (prc != OTA_MANIFEST_OK) {
        printf("[ota] refused: bad manifest (%s)\n",
               ota_manifest_strerror(prc));
        return -1;
    }
    if (m->size < OTA_SIZE_MIN || m->size > OTA_SIZE_MAX) {
        printf("[ota] refused: size %llu is outside the sane kernel "
               "range (%llu..%llu bytes)\n",
               m->size, OTA_SIZE_MIN, OTA_SIZE_MAX);
        return -1;
    }
    return 0;
}

static void print_plan(const ota_manifest *m) {
    printf("[ota] manifest: version=%s\n", m->version);
    printf("[ota] manifest: url=%s\n", m->url);
    printf("[ota] manifest: size=%llu sha256=%s\n", m->size, m->sha256_hex);
}

static void cmd_check(ahttp_client *cli, const char *url) {
    ota_manifest m;
    if (fetch_manifest(cli, url, &m) != 0) { fflush(stdout); return; }
    print_plan(&m);

    unsigned char digest[32];
    unsigned long long size;
    if (sha256_file(FAT_ELF, digest, &size) == 0) {
        char hex[OTA_SHA256_HEX];
        ota_digest_hex(digest, hex);
        int same = (size == m.size) &&
                   ota_hex_digest_ok(digest, m.sha256_hex);
        printf("[ota] active kernel: %llu bytes, sha256 %s%s\n", size, hex,
               same ? "  (identical to the manifest — already updated)"
                    : "");
    } else {
        printf("[ota] active kernel: NOT READABLE\n");
    }
    fflush(stdout);
}

/* ---- minimal streaming HTTP GET ------------------------------------------ */

typedef struct {
    char host[256];
    int  port;
    char path[384];
} http_url;

static int parse_http_url(const char *url, http_url *u) {
    if (strncmp(url, "http://", 7) != 0) return -1;
    const char *rest = url + 7;
    const char *slash = strchr(rest, '/');
    size_t host_len = slash ? (size_t)(slash - rest) : strlen(rest);
    if (host_len == 0 || host_len >= sizeof(u->host)) return -1;
    memcpy(u->host, rest, host_len);
    u->host[host_len] = '\0';

    /* host[:port] */
    char *colon = strchr(u->host, ':');
    u->port = 80;
    if (colon) {
        *colon = '\0';
        int p = atoi(colon + 1);
        if (p <= 0 || p > 65535) return -1;
        u->port = p;
    }
    const char *p = slash ? slash : "/";
    if (strlen(p) >= sizeof(u->path)) return -1;
    strcpy(u->path, p);
    return 0;
}

/* IP in AuraLite host byte order (a<<24|b<<16|c<<8|d), 0 on failure. */
static uint32_t parse_ip(const char *s) {
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
        a < 256 && b < 256 && c < 256 && d < 256)
        return (a << 24) | (b << 16) | (c << 8) | d;
    return 0;
}

/* Stream `url` into `out_fd`, hashing on the fly.  Returns 0 on a fully
 * received body that matches want_size bytes, -1 on any failure. */
static int stream_to_file(const char *url, int out_fd,
                          unsigned long long want_size,
                          unsigned char digest[32]) {
    http_url u;
    if (parse_http_url(url, &u) != 0) {
        if (strncmp(url, "https://", 8) == 0)
            printf("[ota] refused: https payload needs streaming TLS "
                   "(a named deferral); serve the payload over http\n");
        else
            printf("[ota] refused: payload url is not http://\n");
        return -1;
    }

    uint32_t ip = parse_ip(u.host);
    if (!ip) ip = dns_resolve(u.host);
    if (!ip) {
        printf("[ota] cannot resolve %s\n", u.host);
        return -1;
    }
    printf("[ota] connecting %s:%d\n", u.host, u.port);

    int fd = socket(AF_INET_GUEST, SOCK_STREAM_GUEST, 0);
    if (fd < 0) { printf("[ota] socket failed\n"); return -1; }
    if (connect(fd, ip, (uint16_t)u.port) != 0) {
        printf("[ota] connect failed\n");
        closesocket(fd);
        return -1;
    }

    static char req[512];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.0\r\nHost: %s\r\n"
                      "Connection: close\r\n\r\n", u.path, u.host);
    if (rl <= 0 || send(fd, req, (uint32_t)rl) != rl) {
        printf("[ota] request send failed\n");
        closesocket(fd);
        return -1;
    }

    /* Read the header block (bounded; a server that never sends a
     * blank line is not worth waiting for). */
    static char hdr[4096];
    size_t hlen = 0;
    int header_done = 0;
    int idle = 0;
    while (hlen < sizeof(hdr) - 1) {
        int n = recv(fd, hdr + hlen, (uint32_t)(sizeof(hdr) - 1 - hlen));
        if (n == 0) {
            /* Soft timeout (the kernel's TCP recv returns 0 after ~1 s
             * with no segment): the peer may be backing off its RTO
             * while this machine's disk flush hogs the CPU.  Wait it
             * out — up to ~30 s of silence — before declaring death. */
            if (++idle > 30) break;
            continue;
        }
        if (n < 0) break;
        idle = 0;
        hlen += (size_t)n;
        hdr[hlen] = '\0';
        if (strstr(hdr, "\r\n\r\n")) { header_done = 1; break; }
    }
    if (!header_done) {
        printf("[ota] malformed HTTP response (no header end)\n");
        closesocket(fd);
        return -1;
    }
    if (strncmp(hdr, "HTTP/1.", 7) != 0 || hdr[9] != '2') {
        printf("[ota] payload fetch failed: %.20s\n", hdr);
        closesocket(fd);
        return -1;
    }

    /* Content-Length is required: a bounded tool must know what to
     * expect; chunked encoding is refused (a named simplification —
     * the CI server sends Content-Length). */
    const char *cl = strstr(hdr, "Content-Length:");
    if (!cl) cl = strstr(hdr, "content-length:");
    if (!cl) {
        printf("[ota] refused: no Content-Length in payload response "
               "(chunked is not supported)\n");
        closesocket(fd);
        return -1;
    }
    unsigned long long clen = strtoull(cl + 15, NULL, 10);
    if (clen != want_size) {
        printf("[ota] refused: Content-Length %llu != manifest size %llu\n",
               clen, want_size);
        closesocket(fd);
        return -1;
    }

    /* Any body bytes that arrived with the header go first. */
    size_t body_start = (size_t)(strstr(hdr, "\r\n\r\n") - hdr) + 4;
    atls_sha256_ctx c;
    atls_sha256_init(&c);
    unsigned long long got = 0;
    static char buf[CHUNK];

    if (hlen > body_start) {
        size_t n0 = hlen - body_start;
        if (write(out_fd, hdr + body_start, n0) != (ssize_t)n0) goto wfail;
        atls_sha256_update(&c, hdr + body_start, n0);
        got += n0;
    }
    idle = 0;   /* reuse the header phase's soft-timeout budget */
    while (got < clen) {
        unsigned want = (clen - got) > CHUNK ? CHUNK
                                             : (unsigned)(clen - got);
        int n = recv(fd, buf, want);
        if (n == 0) {
            /* Soft timeout, not a dead connection: measured on the real
             * flow, the guest's buffer-cache writeback stalls the CPU
             * long enough that SLIRP's retransmit backoff exceeds the
             * kernel's ~1 s recv wait — recv() returns 0 while the
             * payload is still coming.  Wait it out (up to ~60 s of
             * consecutive silence) instead of aborting a healthy
             * transfer at half its bytes. */
            if (++idle > 60) {
                printf("[ota] connection silent at %llu of %llu bytes\n",
                       got, clen);
                closesocket(fd);
                return -1;
            }
            continue;
        }
        if (n < 0) {
            printf("[ota] connection cut at %llu of %llu bytes\n",
                   got, clen);
            closesocket(fd);
            return -1;
        }
        idle = 0;
        if (write(out_fd, buf, (size_t)n) != n) goto wfail;
        atls_sha256_update(&c, buf, (size_t)n);
        got += (unsigned long long)n;
    }
    closesocket(fd);
    atls_sha256_final(&c, digest);
    return 0;

wfail:
    closesocket(fd);
    printf("[ota] write to /fat failed (disk full?); aborting — "
           "KERNEL.ELF is untouched\n");
    return -1;
}

/* ---- apply / rollback ----------------------------------------------------- */

static void cmd_apply(ahttp_client *cli, const char *url) {
    ota_manifest m;
    if (fetch_manifest(cli, url, &m) != 0) { fflush(stdout); return; }
    print_plan(&m);

    int fd = open(FAT_NEW, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("[ota] cannot create %s\n", FAT_NEW);
        fflush(stdout);
        return;
    }
    printf("[ota] downloading %llu bytes into %s (4 KiB chunks)\n",
           m.size, FAT_NEW);

    unsigned char digest[32];
    if (stream_to_file(m.url, fd, m.size, digest) != 0) {
        close(fd);
        unlink(FAT_NEW);
        printf("[ota] aborted; KERNEL.ELF is untouched\n");
        fflush(stdout);
        return;
    }
    close(fd);

    char got_hex[OTA_SHA256_HEX];
    ota_digest_hex(digest, got_hex);
    if (!ota_hex_digest_ok(digest, m.sha256_hex)) {
        unlink(FAT_NEW);
        printf("[ota] sha256 MISMATCH (manifest %s, payload %s)\n",
               m.sha256_hex, got_hex);
        printf("[ota] aborted; KERNEL.ELF is untouched\n");
        fflush(stdout);
        return;
    }
    printf("[ota] payload verified (sha256 ok)\n");

    /* A/B swap.  The OLD slot is replaced deliberately: it exists to
     * hold the PREVIOUS kernel, which is exactly what the current
     * KERNEL.ELF becomes. */
    unlink(FAT_OLD);                       /* ignore absence */
    if (rename(FAT_ELF, FAT_OLD) != 0) {
        printf("[ota] rename %s -> %s failed\n", FAT_ELF, FAT_OLD);
        fflush(stdout);
        return;
    }
    if (rename(FAT_NEW, FAT_ELF) != 0) {
        printf("[ota] rename %s -> %s failed; rolling back\n",
               FAT_NEW, FAT_ELF);
        rename(FAT_OLD, FAT_ELF);          /* best effort restore */
        fflush(stdout);
        return;
    }
    printf("[ota] A/B swap done: KERNEL.OLD <- current, KERNEL.ELF <- new "
           "(version %s)\n", m.version);

    sync();
    printf("[ota] synced; reboot to activate\n");
    fflush(stdout);
}

static void cmd_rollback(void) {
    unsigned char digest[32];
    if (sha256_file(FAT_OLD, digest, NULL) != 0) {
        printf("[ota] no KERNEL.OLD to roll back to\n");
        fflush(stdout);
        return;
    }
    /* Park the current kernel, promote OLD, drop the parked copy: at
     * every instant a bootable KERNEL.ELF exists. */
    unlink(FAT_NEW);                       /* stale from an aborted apply */
    if (rename(FAT_ELF, FAT_NEW) != 0) {
        printf("[ota] rename %s -> %s failed\n", FAT_ELF, FAT_NEW);
        fflush(stdout);
        return;
    }
    if (rename(FAT_OLD, FAT_ELF) != 0) {
        printf("[ota] rename %s -> %s failed; restoring\n", FAT_OLD, FAT_ELF);
        rename(FAT_NEW, FAT_ELF);
        fflush(stdout);
        return;
    }
    unlink(FAT_NEW);
    printf("[ota] rollback done: KERNEL.ELF <- KERNEL.OLD\n");
    sync();
    printf("[ota] synced; reboot to activate\n");
    fflush(stdout);
}

static void usage(void) {
    puts("usage: ota check <manifest-url>   — fetch + print the update plan");
    puts("       ota apply <manifest-url>   — download, verify, A/B swap, sync");
    puts("       ota rollback               — promote KERNEL.OLD, sync");
    puts("       ota status                 — size + sha256 of both slots");
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    if (strcmp(argv[1], "status") == 0 && argc == 2) {
        cmd_status();
        return 0;
    }
    if (strcmp(argv[1], "rollback") == 0 && argc == 2) {
        cmd_rollback();
        return 0;
    }

    /* check/apply need the manifest URL and an ahttp client. */
    if ((strcmp(argv[1], "check") == 0 || strcmp(argv[1], "apply") == 0) &&
        argc == 3) {
        ahttp_client *cli = ahttp_client_new();
        if (!cli) { puts("[ota] out of memory"); return 1; }
        if (strcmp(argv[1], "check") == 0)
            cmd_check(cli, argv[2]);
        else
            cmd_apply(cli, argv[2]);
        ahttp_client_free(cli);
        return 0;
    }

    usage();
    return 1;
}
