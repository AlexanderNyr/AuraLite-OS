/* w32_manifest.h — application-manifest probing for the W32A-1 loader.
 * SPDX-License-Identifier: Apache-2.0 -- written for AuraLite OS.
 *
 * W32APP_PLAN.md phase W32A-1: parse the .rsrc type-24 manifest and record
 * what the loader must honour now versus later:
 *
 *   now:    requestedExecutionLevel (requireAdministrator refuses -- there
 *           is no elevation, and pretending would be a D9 lie);
 *   now:    the Common-Controls assemblyIdentity selects the comctl32
 *           major (6 when the manifest pins v6, else the v5/unversioned
 *           branch -- all three ladder manifests pin v6, the v5 branch is
 *           fixture-proved);
 *   later:  dpiAware/dpiAwareness is recorded for W32A-7;
 *   loudly: supportedOS GUIDs are parsed and logged, never actioned.
 *
 * The parser is a documented substring scan, not an XML parser: manifests
 * are author-controlled, double-quoted, case-sensitive XML, and the four
 * facts above do not need a tree.  Anything that is not manifest-shaped at
 * all (no <assembly> envelope) is malformed, and malformed refuses -- the
 * Windows side-by-side error, not a guess.
 */

#ifndef AURALITE_W32_MANIFEST_H
#define AURALITE_W32_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#define W32_MANIFEST_AS_INVOKER 0
#define W32_MANIFEST_HIGHEST    1
#define W32_MANIFEST_ADMIN      2

typedef struct {
    int has_manifest;      /* a type-24 resource was present at all */
    int comctl_major;      /* 6 when the SxS dep pins v6, else 5 */
    int exec_level;        /* W32_MANIFEST_* */
    int dpi_aware;         /* any dpiAware/dpiAwareness stanza seen */
    int supported_os;      /* count of supportedOS stanzas (ignored loudly) */
} w32_manifest_t;

/* Parse manifest bytes.  Returns 0 with *out filled, or -1 when the bytes
 * are not manifest-shaped.  Never fails on a well-formed manifest that
 * merely lacks a stanza -- absent stanzas take documented defaults. */
int w32_manifest_parse(const uint8_t *xml, size_t len, w32_manifest_t *out);

/* Find the type-24 resource in a raw PE file and parse it.  Returns 0 with
 * *out filled (has_manifest = 0 and defaults when there is no manifest),
 * -1 on malformed bytes or an unreadable image.  supportedOS stanzas are
 * logged here, where "loudly" has a file to point at. */
int w32_manifest_check(const uint8_t *file, size_t file_size,
                       w32_manifest_t *out);

#endif /* AURALITE_W32_MANIFEST_H */
