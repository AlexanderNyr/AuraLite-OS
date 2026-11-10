/* kernel/fs/documents.h — OF0: one writable place for the user's documents.
 *
 * docs/plans/OFFICE_PLAN.md OF0 asks for "/documents: a writable directory
 * that survives a reboot (/disk/documents when a disk is present,
 * /tmp/documents otherwise), created at boot if missing".  The office suite's
 * file dialog is written against this one path (OF2), so the backing store can
 * change later without touching the applications.
 */

#ifndef AURALITE_FS_DOCUMENTS_H
#define AURALITE_FS_DOCUMENTS_H

/* The stable path applications use. */
#define DOCUMENTS_PATH "/documents"

/* Report the backing store, create a durable directory when the machine has a
 * writable disk filesystem, and prove DOCUMENTS_PATH can be written through.
 * Returns 0 on success, a negative errno otherwise.  Safe to call twice. */
int documents_init(void);

/* The backing directory chosen by the last documents_init(), e.g.
 * "/tmp/documents", or NULL before it runs.  Used by the boot receipt and by
 * tests; never NULL after a successful init. */
const char *documents_backing(void);

/* Non-zero when DOCUMENTS_PATH is expected to survive a reboot (a real
 * directory filesystem on a disk), zero when it is tmpfs and therefore
 * volatile -- which is what it is today.  Apps quote this instead of
 * guessing, and the boot receipt prints it. */
int documents_is_durable(void);

#endif /* AURALITE_FS_DOCUMENTS_H */
