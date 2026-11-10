/* userspace/office/libaodoc/aoundo.h — undo and redo with a byte budget
 * (OFFICE_PLAN.md OF2, decision D6).
 *
 * The design decision, stated once so it does not have to be rediscovered: a
 * history entry is a SERIALISED COPY OF THE DOCUMENT, not an inverse operation.
 *
 * Inverse operations are cheaper and much harder to get right.  Every edit here
 * can split a run, merge two paragraphs, retarget a run array and re-normalise,
 * and the inverse of "merge two paragraphs" is not "split them at the same
 * offset" -- it is a small structural transplant.  A copy of the document is
 * correct by construction, and it reuses the very serialiser that the save path
 * uses, so undo cannot drift out of step with what the file format can express.
 *
 * The cost is what the byte budget is for: entries are evicted oldest-first when
 * the history buffer fills, and typing is COALESCED so that a run of keystrokes
 * is one entry (a text editor that offers one undo per character is not
 * offering undo, it is offering a rewind button).
 *
 * The undo and redo stacks get SEPARATE buffers, which is a design decision
 * forced by arithmetic: one shared buffer with the stacks growing towards each
 * other has no room for the redo entry at the moment it is most needed, because
 * the entry being consumed sits exactly where the new one has to go.  Two
 * buffers make undo() a pop from one stack and a push onto the other with no
 * overlap to reason about.
 */

#ifndef AURALITE_OFFICE_AOUNDO_H
#define AURALITE_OFFICE_AOUNDO_H

#include <stddef.h>
#include <stdint.h>

#include "aomodel.h"

#define AO_UNDO_DEFAULT_BYTES (256u * 1024u)

/* Entry kinds.  Consecutive entries of the same coalescing kind collapse into
 * one; AO_UNDO_BARRIER never coalesces, and is what a caret move, a save or a
 * style change should push. */
#define AO_UNDO_EDIT     0u    /* a discrete edit: always its own entry */
#define AO_UNDO_TYPING   1u    /* coalesces with the previous typing run */
#define AO_UNDO_DELETE   2u    /* coalesces with the previous delete run */
#define AO_UNDO_BARRIER  3u    /* never coalesces, always pushed */

typedef struct {
    uint8_t *undo_base;
    size_t   undo_cap, undo_used;
    uint8_t *redo_base;
    size_t   redo_cap, redo_used;
    uint32_t undo_count, redo_count;
    uint32_t pushes, coalesced, evicted, refusals;
    uint8_t  last_kind;        /* for coalescing */
    uint8_t  coalesce_blocked; /* set until the next push after a break */
} ao_undo_t;

/* Bind to caller-owned memory.  A zero capacity is legal: that stack then
 * records nothing and reports AO_E_FULL, which is an honest "no history". */
int  ao_undo_init(ao_undo_t *u, void *undo_mem, size_t undo_cap,
                  void *redo_mem, size_t redo_cap);
void ao_undo_reset(ao_undo_t *u);

/* Remember the document AS IT IS NOW, before applying an edit of `kind`.
 * Returns AO_OK, AO_E_FULL (the entry does not fit even in an empty history --
 * the caller should keep editing, just without that step) or AO_E_ARG. */
int  ao_undo_push(ao_undo_t *u, const ao_model_t *m, uint8_t kind);

/* Restore the previous state into `m`, moving the current state onto the redo
 * stack.  Returns AO_OK, AO_E_FULL (nothing to undo) or AO_E_FORMAT (the stored
 * entry no longer parses -- refused, never half-applied). */
int  ao_undo_undo(ao_undo_t *u, ao_model_t *m);
int  ao_undo_redo(ao_undo_t *u, ao_model_t *m);

/* End the current coalescing run: the next push always makes a new entry. */
void ao_undo_break(ao_undo_t *u);

uint32_t ao_undo_depth(const ao_undo_t *u);
uint32_t ao_undo_redo_depth(const ao_undo_t *u);
size_t   ao_undo_bytes(const ao_undo_t *u);

#endif /* AURALITE_OFFICE_AOUNDO_H */
