/* w32_manifest.c — see w32_manifest.h for the contract.
 * SPDX-License-Identifier: Apache-2.0 -- written for AuraLite OS.
 *
 * What "well-formed enough" means here, precisely: an optional UTF-8 BOM,
 * an optional <?xml ...?> prolog, then <assembly and, later, </assembly>.
 * Element order, namespaces, comments and single-vs-double quotes are NOT
 * modelled -- the scan looks for double-quoted attribute spellings exactly
 * as the ladder manifests (and llvm-rc fixtures) write them.  A manifest
 * that quotes attributes with apostrophes parses as "stanza absent" and
 * takes defaults; that is documented here rather than fixed, because none
 * of the measured manifests do it.
 */

#include "w32/w32_manifest.h"
#include "w32/w32_pe.h"

#ifndef AURALITE_W32_HOST_TEST
#include <stdio.h>
#else
#include <stdio.h>
#endif

static const uint8_t *find_sub(const uint8_t *hay, size_t hlen,
                               const char *needle, size_t *pos_out) {
    size_t nlen = 0;
    while (needle[nlen]) nlen++;
    if (nlen == 0 || nlen > hlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t k = 0;
        while (k < nlen && hay[i + k] == (uint8_t)needle[k]) k++;
        if (k == nlen) {
            if (pos_out) *pos_out = i;
            return hay + i;
        }
    }
    return 0;
}

static int is_space(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int w32_manifest_parse(const uint8_t *xml, size_t len, w32_manifest_t *out) {
    size_t i = 0, j;
    if (!xml || !out) return -1;
    out->has_manifest  = 1;
    out->comctl_major  = 5;
    out->exec_level    = W32_MANIFEST_AS_INVOKER;
    out->dpi_aware     = 0;
    out->supported_os  = 0;

    /* UTF-8 BOM, then whitespace. */
    if (len >= 3 && xml[0] == 0xEFu && xml[1] == 0xBBu && xml[2] == 0xBFu)
        i = 3;
    while (i < len && is_space(xml[i])) i++;
    /* An <?xml ...?> prolog, skipped wholesale to its ?>. */
    if (i + 5 < len && xml[i] == '<' && xml[i + 1] == '?' &&
        xml[i + 2] == 'x' && xml[i + 3] == 'm' && xml[i + 4] == 'l') {
        j = i + 5;
        while (j + 1 < len && !(xml[j] == '?' && xml[j + 1] == '>')) j++;
        if (j + 1 >= len) return -1;
        i = j + 2;
        while (i < len && is_space(xml[i])) i++;
    }
    /* The envelope. */
    if (!find_sub(xml + i, len - i, "<assembly", 0)) return -1;
    if (!find_sub(xml, len, "</assembly>", 0)) return -1;

    /* Common-Controls v6: the dependency name plus a 6.x version.  Any
     * other spelling (v5, unversioned, absent) is the v5 branch. */
    if (find_sub(xml, len, "Microsoft.Windows.Common-Controls", &j)) {
        const uint8_t *rest = xml + j;
        size_t rlen = len > j ? len - j : 0;
        if (find_sub(rest, rlen < 512 ? rlen : 512, "version=\"6.", 0))
            out->comctl_major = 6;
    }

    /* Execution level.  highestAvailable degrades to asInvoker with a note:
     * there is no elevation to hold, so "highest" is "invoker". */
    if (find_sub(xml, len, "requestedExecutionLevel", &j)) {
        const uint8_t *rest = xml + j;
        size_t rlen = len > j ? len - j : 0;
        size_t win = rlen < 256 ? rlen : 256;
        if (find_sub(rest, win, "level=\"requireAdministrator\"", 0))
            out->exec_level = W32_MANIFEST_ADMIN;
        else if (find_sub(rest, win, "level=\"highestAvailable\"", 0))
            out->exec_level = W32_MANIFEST_HIGHEST;
    }

    /* DPI record for W32A-7.  Either spelling counts; the value rides along
     * in the fixture logs, not in this struct. */
    if (find_sub(xml, len, "dpiAwareness", 0) ||
        find_sub(xml, len, "dpiAware", 0))
        out->dpi_aware = 1;

    /* supportedOS: counted here, logged by the caller with the file name. */
    {
        size_t off = 0, p;
        while (off < len &&
               find_sub(xml + off, len - off, "<supportedOS", &p)) {
            out->supported_os++;
            off += p + 1;
            if (off == 0) break; /* overflow paranoia, unreachable */
        }
    }
    return 0;
}

int w32_manifest_check(const uint8_t *file, size_t file_size,
                       w32_manifest_t *out) {
    pe_image_t img;
    uint32_t rva = 0, mlen = 0, off = 0;
    int rc;
    if (!file || !out) return -1;
    out->has_manifest  = 0;
    out->comctl_major  = 5;
    out->exec_level    = W32_MANIFEST_AS_INVOKER;
    out->dpi_aware     = 0;
    out->supported_os  = 0;

    if (pe_parse(file, file_size, &img) != PE_OK) return -1;
    rc = pe_find_resource(&img, PE_RSRC_MANIFEST, &rva, &mlen);
    if (rc != PE_OK) return -1;
    if (rva == 0 || mlen == 0) return 0;   /* absent is legal */
    if (pe_rva_to_offset(&img, rva, mlen, &off) != PE_OK) return -1;
    if (w32_manifest_parse(file + off, mlen, out) != 0) return -1;
    for (int k = 0; k < out->supported_os; k++)
        printf("w32: manifest: supportedOS stanza ignored (not actioned)\n");
    return 0;
}
