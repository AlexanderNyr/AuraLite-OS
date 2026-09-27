/* W32A-11: bounded in-process OLE drag targets and STGMEDIUM ownership.
 * IDropTarget sees the caller's genuine IDataObject, not a fabricated one.
 * Sources outside a w32run process and COM cross-process marshalling do not
 * exist. Published OLE2/oleidl/objidl vtable and HRESULT facts only.
 * SPDX-License-Identifier: Apache-2.0 */
#include "w32/ole32.h"
#include "w32/kernel32.h"
#include "w32/gdi32.h"
#include "w32/w32_errno.h"
#include <stddef.h>
#include <stdint.h>

_Static_assert(sizeof(W32_STGMEDIUM) == 24 &&
               offsetof(W32_STGMEDIUM, medium) == 8 &&
               offsetof(W32_STGMEDIUM, pUnkForRelease) == 16,
               "Win64 STGMEDIUM layout");

#define W32_DROP_TARGETS 32
static struct drop_entry {
    W32_HWND hwnd;
    W32_IDropTarget *target;
    unsigned active;
    int registered;
} drop_entries[W32_DROP_TARGETS];
static volatile int drop_lock;
static void lock_drop(void) {
    while (__sync_lock_test_and_set(&drop_lock, 1))
        while (drop_lock) __asm__ volatile("pause" ::: "memory");
}
static void unlock_drop(void) { __sync_lock_release(&drop_lock); }

/* Active drags pin the registration: a callback may RevokeDragDrop or destroy
 * its own HWND without freeing the target before that callback returns.
 * No COM callback or USER32 call is made while holding drop_lock. */
static struct drop_entry *hold_entry(W32_HWND hwnd) {
    struct drop_entry *found = NULL;
    lock_drop();
    for (int i = 0; i < W32_DROP_TARGETS; ++i)
        if (drop_entries[i].registered && drop_entries[i].hwnd == hwnd) {
            found = &drop_entries[i]; ++found->active; break;
        }
    unlock_drop();
    return found;
}
static void release_entry(struct drop_entry *entry) {
    if (!entry) return;
    W32_IDropTarget *target = NULL;
    lock_drop();
    if (entry->active && !--entry->active && !entry->registered) {
        target = entry->target;
        entry->target = NULL;
    }
    unlock_drop();
    if (target) target->vt->Release(target);
}
W32ABI W32_DWORD RegisterDragDrop(W32_HWND hwnd, W32_IDropTarget *target) {
    if (!target || !target->vt || !target->vt->AddRef || !target->vt->Release ||
        !target->vt->DragEnter || !target->vt->DragOver ||
        !target->vt->DragLeave || !target->vt->Drop) return W32_COM_E_INVALIDARG;
    if (!IsWindow(hwnd)) return W32_DRAGDROP_E_INVALIDHWND;
    if (!w32_com_ole_ready()) return W32_COM_E_NOTINITIALIZED;
    target->vt->AddRef(target);
    W32_DWORD result = W32_COM_S_OK;
    lock_drop();
    int free_slot = -1;
    for (int i = 0; i < W32_DROP_TARGETS; ++i) {
        if (drop_entries[i].registered && drop_entries[i].hwnd == hwnd) {
            result = W32_DRAGDROP_E_ALREADYREGISTERED; break;
        }
        if (!drop_entries[i].registered && !drop_entries[i].active &&
            !drop_entries[i].target && free_slot < 0) free_slot = i;
    }
    if (result == W32_COM_S_OK) {
        if (free_slot < 0) result = W32_COM_E_OUTOFMEMORY;
        else {
            drop_entries[free_slot].target = target;
            drop_entries[free_slot].hwnd = hwnd;
            drop_entries[free_slot].registered = 1;
        }
    }
    unlock_drop();
    if (result != W32_COM_S_OK) target->vt->Release(target);
    return result;
}
W32ABI W32_DWORD RevokeDragDrop(W32_HWND hwnd) {
    W32_IDropTarget *target = NULL;
    int found = 0;
    lock_drop();
    for (int i = 0; i < W32_DROP_TARGETS; ++i) {
        struct drop_entry *entry = &drop_entries[i];
        if (!entry->registered || entry->hwnd != hwnd) continue;
        found = 1;
        entry->registered = 0;
        entry->hwnd = NULL;
        if (!entry->active) { target = entry->target; entry->target = NULL; }
        break;
    }
    unlock_drop();
    if (target) target->vt->Release(target);
    return found ? W32_COM_S_OK : W32_DRAGDROP_E_NOTREGISTERED;
}
void w32_ole_drag_window_destroyed(W32_HWND hwnd) {
    (void)RevokeDragDrop(hwnd);
}

W32ABI void ReleaseStgMedium(W32_STGMEDIUM *m) {
    if (!m || (m->tymed == W32_TYMED_NULL && !m->pUnkForRelease)) return;
    if (m->pUnkForRelease) {
        void *owner = m->pUnkForRelease;
        void **vt = *(void ***)owner;
        if (!vt || !vt[2]) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return; }
        ((W32_DWORD (W32ABI *)(void *))vt[2])(owner);
    } else if (m->tymed == W32_TYMED_HGLOBAL) {
        if (m->medium && GlobalFree(m->medium)) {
            w32_set_last_error(W32_ERROR_INVALID_HANDLE); return;
        }
    } else if (m->tymed == W32_TYMED_GDI) {
        if (m->medium && !DeleteObject(m->medium)) {
            w32_set_last_error(W32_ERROR_INVALID_HANDLE); return;
        }
    } else if (m->tymed == W32_TYMED_ISTREAM ||
               m->tymed == W32_TYMED_ISTORAGE) {
        if (m->medium) {
            void **vt = *(void ***)m->medium;
            if (!vt || !vt[2]) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return; }
            ((W32_DWORD (W32ABI *)(void *))vt[2])(m->medium);
        }
    } else if (m->tymed == W32_TYMED_FILE) {
        /* The real path layer supplies the precise last error on failure. */
        if (!m->medium) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return;
        }
        if (!DeleteFileW((const uint16_t *)m->medium)) return;
        CoTaskMemFree(m->medium);
    } else {
        w32_set_last_error(W32_ERROR_NOT_SUPPORTED);
        return;
    }
    m->tymed = W32_TYMED_NULL;
    m->medium = NULL;
    m->pUnkForRelease = NULL;
}

static W32_HWND find_target(W32_POINT pt) {
    W32_HWND hwnd = WindowFromPoint(pt);
    for (int i = 0; hwnd && i < 32; ++i) {
        lock_drop();
        int found = 0;
        for (int j = 0; j < W32_DROP_TARGETS; ++j)
            if (drop_entries[j].registered && drop_entries[j].hwnd == hwnd) {
                found = 1; break;
            }
        unlock_drop();
        if (found) return hwnd;
        W32_HWND parent = GetParent(hwnd);
        if (parent == hwnd) break;
        hwnd = parent;
    }
    return NULL;
}
static void leave_target(struct drop_entry **active) {
    if (!*active) return;
    (*active)->target->vt->DragLeave((*active)->target);
    release_entry(*active);
    *active = NULL;
}
static W32_POINT cursor_from_msg(const W32_MSG *m) {
    W32_POINT pt = {0, 0};
    if (m->hwnd) {
        pt.x = (int16_t)(m->lParam & 0xffff);
        pt.y = (int16_t)((m->lParam >> 16) & 0xffff);
        if (ClientToScreen(m->hwnd, &pt)) return pt;
    }
    (void)GetCursorPos(&pt);
    return pt;
}
W32ABI W32_DWORD DoDragDrop(void *data, W32_IDropSource *source,
                             W32_DWORD allowed, W32_DWORD *effect) {
    if (!effect) return W32_COM_E_INVALIDARG;
    *effect = W32_DROPEFFECT_NONE;
    if (!data || !source || !source->vt || !source->vt->QueryContinueDrag ||
        !source->vt->GiveFeedback || !source->vt->AddRef ||
        !source->vt->Release || !(allowed & 7u)) return W32_COM_E_INVALIDARG;
    if (!w32_com_ole_ready()) return W32_COM_E_NOTINITIALIZED;
    source->vt->AddRef(source);
    struct drop_entry *active = NULL;
    W32_DWORD result = W32_DRAGDROP_S_CANCEL, chosen = W32_DROPEFFECT_NONE;
    for (;;) {
        W32_MSG m = {0};
        int got = GetMessageW(&m, NULL, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage((int32_t)m.wParam);
            leave_target(&active);
            break;
        }
        if (m.message != W32_WM_MOUSEMOVE && m.message != W32_WM_LBUTTONUP &&
            !(m.message == W32_WM_KEYDOWN && m.wParam == W32_VK_ESCAPE)) {
            DispatchMessageW(&m);
            continue;
        }
        W32_DWORD keys = (W32_DWORD)m.wParam & 0xffffu;
        W32_BOOL escape = m.message == W32_WM_KEYDOWN;
        if (m.message == W32_WM_LBUTTONUP) keys &= ~1u;
        W32_DWORD action = source->vt->QueryContinueDrag(source, escape, keys);
        if (action == W32_DRAGDROP_S_CANCEL || escape) {
            leave_target(&active);
            break;
        }
        if (action == W32_DRAGDROP_S_DROP) {
            /* A button-up can land on another HWND without an intervening
             * WM_MOUSEMOVE. Never drop on the previous HWND by accident. */
            W32_POINT pt = cursor_from_msg(&m);
            W32_POINTL pt_long = {pt.x, pt.y};
            W32_HWND under = find_target(pt);
            if (active && (!active->registered || active->hwnd != under))
                leave_target(&active);
            if (!active && under) {
                active = hold_entry(under);
                chosen = allowed & 7u;
                if (active && active->target->vt->DragEnter(active->target,
                        data, keys, pt_long, &chosen) != W32_COM_S_OK)
                    chosen = W32_DROPEFFECT_NONE;
            }
            if (active && active->registered && (chosen & allowed & 7u)) {
                W32_DWORD accepted = chosen & allowed & 7u;
                if (active->target->vt->Drop(active->target, data, keys,
                                              pt_long, &accepted) == W32_COM_S_OK &&
                    (accepted & allowed & 7u)) {
                    chosen = accepted & allowed & 7u;
                    result = W32_DRAGDROP_S_DROP;
                }
            }
            if (result != W32_DRAGDROP_S_DROP) leave_target(&active);
            else { release_entry(active); active = NULL; }
            break;
        }
        if (action != W32_COM_S_OK) {
            leave_target(&active);
            break;
        }
        W32_POINT pt = cursor_from_msg(&m);
        W32_POINTL pt_long = {pt.x, pt.y};
        W32_HWND hwnd = find_target(pt);
        if (active && (!active->registered || active->hwnd != hwnd))
            leave_target(&active);
        if (!active && hwnd) {
            active = hold_entry(hwnd);
            if (active) {
                chosen = allowed & 7u;
                if (active->target->vt->DragEnter(active->target, data, keys,
                                                   pt_long, &chosen) != W32_COM_S_OK)
                    chosen = W32_DROPEFFECT_NONE;
            }
        } else if (active) {
            chosen = allowed & 7u;
            if (active->target->vt->DragOver(active->target, keys,
                                              pt_long, &chosen) != W32_COM_S_OK)
                chosen = W32_DROPEFFECT_NONE;
        }
        chosen &= allowed & 7u;
        source->vt->GiveFeedback(source, chosen);
    }
    *effect = result == W32_DRAGDROP_S_DROP ? chosen : W32_DROPEFFECT_NONE;
    source->vt->Release(source);
    return result;
}
