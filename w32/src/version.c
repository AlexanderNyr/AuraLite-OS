/* version.c — W32APP_PLAN.md phase W32A-10: the VERSION module.
 *
 * A real reader for the PE VS_VERSION_INFO resource: open the file,
 * walk the W32A-1 resource directory to RT_VERSION/1 (the documented
 * default name), hand back the blob, and parse it for VerQueryValueW.
 *
 * The tree grammar (published VS_VERSION_INFO documentation): a node
 * is { u16 value_len, u16 type, UTF-16 key (NUL), pad to 32-bit,
 * value (value_len bytes), children }.  The root key is
 * "VS_VERSION_INFO" and its value is VS_FIXEDFILEINFO (52 bytes,
 * signature 0xFEEF04BD); the children are VarFileInfo and
 * StringFileInfo; a StringTable's key is the 8-digit language hex
 * ("040904B0"); a String's value is UTF-16, value_len in 16-bit
 * units, NUL included.
 *
 * Licensed Apache-2.0.  Interface facts only (see version.h,
 * w32/PROVENANCE.md).
 */

#include "w32/version.h"
#include "w32/w32_pe.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

extern char *w32_fs_xlate_dup(const char *p);

static int ver_noted;

static void ver_note(const char *what) {
    if (ver_noted) return;
    ver_noted = 1;
    printf("w32: [version] %s\n", what);
}

/* ---- file -> blob -------------------------------------------------------------- */

static uint8_t *ver_read_file(const char *host, size_t *out_len) {
    int fd = open(host, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
        (size_t)st.st_size > 64u * 1024u * 1024u) {
        close(fd);
        return NULL;
    }
    size_t n = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf) {
        close(fd);
        return NULL;
    }
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, buf + off, n - off);
        if (r <= 0) {
            free(buf);
            close(fd);
            return NULL;
        }
        off += (size_t)r;
    }
    close(fd);
    *out_len = n;
    return buf;
}

/* Find RT_VERSION (16), name 1 (VS_VERSION_INFO), language 040904B0
 * or the first found.  Returns a malloc'd copy or NULL. */
static uint16_t *ver_blob_for(const char *host, size_t *out_units) {
    size_t flen = 0;
    uint8_t *file = ver_read_file(host, &flen);
    if (!file) {
        w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
        return NULL;
    }
    pe_image_t img;
    if (pe_parse(file, flen, &img) != PE_OK) {
        free(file);
        w32_set_last_error(W32_ERROR_FILE_INVALID);
        return NULL;
    }
    uint32_t rva = 0, size = 0;
    if (pe_find_resource_ex(&img, 16u, 1u, 0, &rva, &size) != PE_OK ||
        rva == 0 || size == 0) {
        if (pe_find_resource(&img, 16u, &rva, &size) != PE_OK || rva == 0) {
            free(file);
            ver_note("no VS_VERSION_INFO resource in this image; "
                     "GetFileVersionInfo* answers 0/FALSE");
            w32_set_last_error(W32_ERROR_RESOURCE_TYPE_NOT_FOUND);
            return NULL;
        }
    }
    uint32_t off = 0;
    if (pe_rva_to_offset(&img, rva, size, &off) != PE_OK) {
        free(file);
        w32_set_last_error(W32_ERROR_FILE_INVALID);
        return NULL;
    }
    /* Engine blob shape (documented in version.h): the caller's buffer
     * holds [u16 unit_count][resource bytes][u16 zero pad].  The Win32
     * API carries no length to VerQueryValueW, so the prefix is what
     * bounds the walk -- without it a small blob would be walked to a
     * 64 KiB end the malloc never covered.  The buffer is documented
     * as opaque; nothing but VerQueryValueW ever reads it back. */
    uint16_t *blob = (uint16_t *)malloc(size + 4);
    if (!blob) {
        free(file);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    blob[0] = (uint16_t)(size / 2u);   /* the length prefix */
    memcpy(blob + 1, img.data + off, size);
    blob[1 + size / 2u] = 0;           /* the zero pad */
    free(file);
    *out_units = size / 2u;            /* resource units, prefix aside */
    return blob;
}

/* ---- the public three ----------------------------------------------------------- */

W32_DWORD W32ABI GetFileVersionInfoSizeW(W32_LPCWSTR path, W32_DWORD *handle) {
    if (handle) *handle = 0;
    if (!path || !path[0]) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char a[512];
    if (w32_utf16z_to_utf8(path, a, (int32_t)sizeof a) <= 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char *host = w32_fs_xlate_dup(a);
    if (!host) return 0;
    size_t units = 0;
    uint16_t *blob = ver_blob_for(host, &units);
    free(host);
    if (!blob) return 0;
    free(blob);
    return (W32_DWORD)(units * 2u + 4u);   /* prefix + pad */
}

W32_BOOL W32ABI GetFileVersionInfoW(W32_LPCWSTR path, W32_DWORD handle,
                                    W32_DWORD len, void *data) {
    (void)handle;                   /* documented: ignored */
    if (!path || !data || len == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char a[512];
    if (w32_utf16z_to_utf8(path, a, (int32_t)sizeof a) <= 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char *host = w32_fs_xlate_dup(a);
    if (!host) return 0;
    size_t units = 0;
    uint16_t *blob = ver_blob_for(host, &units);
    free(host);
    if (!blob) return 0;
    size_t bytes = units * 2u + 4u; /* prefix + resource + pad */
    if (bytes > len) {              /* documented: fail, no truncation */
        free(blob);
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(data, blob, bytes);
    free(blob);
    return 1;
}

/* ---- the tree walk ---------------------------------------------------------------- */

typedef struct {
    const uint16_t *p;              /* node start */
    const uint16_t *end;            /* region end (units) */
} vwalk_t;

/* Read one node of the documented grammar:
 *   { u16 wLength (whole node, bytes), u16 wValueLength, u16 wType,
 *     UTF-16 key NUL-terminated, pad to 32-bit, value, pad, children }
 * wValueLength counts BYTES for the root's fixed block and CHARACTERS
 * (NUL included) for String values -- the documented asymmetry, and the
 * one every real compiler output exhibits.  `val_in_bytes` selects it.
 * Children run from the post-value pad to the node end (wLength), so a
 * caller can skip whole subtrees with node_end.  Returns 0 on
 * malformed input; the region end bounds every read. */
static int vwalk_node(vwalk_t *w, const uint16_t **key, size_t *key_len,
                      const uint16_t **val, size_t *val_units,
                      int val_in_bytes, const uint16_t **children,
                      const uint16_t **node_end) {
    if (w->p + 3 > w->end) return 0;
    size_t total = w->p[0];         /* wLength: bytes, whole node */
    size_t vlen  = w->p[1];         /* wValueLength: bytes or chars */
    if (total < 4 || w->p + (total + 1) / 2 > w->end) return 0;
    const uint16_t *k = w->p + 3;
    const uint16_t *e = w->end;
    const uint16_t *q = k;
    while (q < e && *q) q++;
    if (q >= e) return 0;
    if (key) *key = k;
    if (key_len) *key_len = (size_t)(q - k);
    q++;                            /* past the key NUL */
    while ((size_t)(q - w->p) & 1u) q++;    /* pad to 32-bit */
    size_t vunits = val_in_bytes ? (vlen + 1) / 2 : vlen;
    if (q + vunits > e) return 0;
    if (val) *val = q;
    if (val_units) *val_units = vunits;
    q += vunits;
    while ((size_t)(q - w->p) & 1u) q++;    /* pad to 32-bit */
    const uint16_t *ne = w->p + (total + 1) / 2;
    if (ne < q || ne > e) return 0;
    if (children) *children = q;
    if (node_end) *node_end = ne;
    return 1;
}

static int w16_eq_ascii(const uint16_t *k, size_t klen, const char *ascii) {
    for (size_t i = 0; i < klen; i++) {
        if (!ascii[i]) return 0;
        if (k[i] != (uint16_t)(unsigned char)ascii[i]) return 0;
    }
    return ascii[klen] == 0;
}

static int w16_eq(const uint16_t *a, const uint16_t *b, size_t units) {
    for (size_t i = 0; i < units; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* ---- VerQueryValueW -------------------------------------------------------------- */

W32_BOOL W32ABI VerQueryValueW(const void *block, W32_LPCWSTR sub,
                               void **out, W32_UINT *outLen) {
    if (!block || !sub || !out || !outLen) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    *out = NULL;
    *outLen = 0;

    /* Split the sub-block: "\\", or "\\StringFileInfo\\<lang>\\<key>". */
    const uint16_t *comp[3] = {0, 0, 0};
    size_t clen[3] = {0, 0, 0};
    int ncomp = 0;
    if (sub[0] != 0x5Cu) {          /* must start with a backslash */
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (sub[1] != 0) {
        const uint16_t *p = sub + 1;
        comp[0] = p;
        while (*p) {
            if (*p == 0x5Cu) {
                if (ncomp >= 2) {
                    w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
                    return 0;
                }
                clen[ncomp] = (size_t)(p - comp[ncomp]);
                ncomp++;
                comp[ncomp] = p + 1;
            }
            p++;
        }
        clen[ncomp] = (size_t)(p - comp[ncomp]);
        ncomp++;
        if (ncomp != 3 || !w16_eq_ascii(comp[0], clen[0], "StringFileInfo")) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return 0;
        }
    }

    /* The engine's blob shape: [u16 unit_count][resource][u16 zero].
     * The prefix bounds the walk -- the Win32 API passes no length. */
    const uint16_t *blob = (const uint16_t *)block;
    size_t blob_units = blob[0];
    if (blob_units == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const uint16_t *blob_end = blob + 1 + blob_units + 1;

    vwalk_t w = { blob + 1, blob_end };
    const uint16_t *key, *val, *kids, *root_end;
    size_t klen, vunits;
    if (!vwalk_node(&w, &key, &klen, &val, &vunits, 1, &kids, &root_end) ||
        !w16_eq_ascii(key, klen, "VS_VERSION_INFO"))
        goto bad;

    if (ncomp == 0) {
        /* "\\" -> the fixed block, signature verified. */
        if (vunits < 26u)
            goto bad;
        uint32_t sig = (uint32_t)val[0] | ((uint32_t)val[1] << 16);
        if (sig != W32_VS_FFI_SIGNATURE)
            goto bad;
        *out = (void *)val;
        *outLen = 52u;
        return 1;
    }

    /* Level 1: find StringFileInfo under the root. */
    {
        vwalk_t cw = { kids, root_end };
        const uint16_t *ck, *cv, *cc, *ce;
        size_t ckl, cvu;
        int _lvl1=0;
        while (vwalk_node(&cw, &ck, &ckl, &cv, &cvu, 0, &cc, &ce)) {
            _lvl1++;
            if (w16_eq_ascii(ck, ckl, "StringFileInfo")) {
                /* Level 2: the language tables. */
                vwalk_t sw = { cc, ce };
                const uint16_t *sk, *sv, *sc, *se;
                size_t skl, svu;
                int _lvl2=0;
                while (vwalk_node(&sw, &sk, &skl, &sv, &svu, 0, &sc, &se)) {
                    _lvl2++;
                    if (skl == clen[1] && w16_eq(sk, comp[1], skl)) {
                        /* Level 3: the strings. */
                        vwalk_t tw = { sc, se };
                        const uint16_t *tk, *tv, *tc, *te;
                        size_t tkl, tvu;
                        int _lvl3=0;
                        while (vwalk_node(&tw, &tk, &tkl, &tv, &tvu, 0,
                                          &tc, &te)) {
                            _lvl3++;
                            if (tkl == clen[2] && w16_eq(tk, comp[2], tkl) &&
                                tvu > 0) {
                                *out = (void *)tv;
                                *outLen = (W32_UINT)tvu;
                                return 1;
                            }
                            tw.p = te;
                        }
                    }
                    sw.p = se;
                }
            }
            cw.p = ce;
        }
    }

bad:
    w32_set_last_error(W32_ERROR_RESOURCE_TYPE_NOT_FOUND);
    return 0;
}
