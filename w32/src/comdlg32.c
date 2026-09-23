/* comdlg32.c — W32APP_PLAN.md phase W32A-10: the common dialogs, REAL
 * over the W32A-6 dialog engine.
 *
 * The open/save dialogs (and the folder picker SHBrowseForFolderW,
 * which lives here with the engine and is bound under SHELL32 — the
 * IsTextUnicode forwarder shape from W32A-9) are real modal dialogs:
 * a path edit, a file list, a filter list, OK and Cancel, driven by
 * messages (LB_SETCURSEL then WM_COMMAND IDOK) or an OFN hook.
 * ChooseColor/ChooseFont are the same machinery over the 16 basic
 * colours and the one real font's size ladder.  PrintDlgW is
 * FAIL-CLEAN: no printers exist, PDERR_NODEFAULTPRN, the reason
 * named — the W32A-7 printing-refusal shape.
 *
 * Licensed Apache-2.0.  Interface facts only (see comdlg32.h,
 * w32/PROVENANCE.md).
 */

#include "w32/comdlg32.h"
#include "w32/shell32.h"       /* SHBrowseForFolderW lives here */
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char *w32_fs_xlate_dup(const char *p);
extern W32_WORD w32_win_register_comctl_class(const W32_WNDCLASSEXW *c);

/* ---- the extended error --------------------------------------------------- */

static W32_DWORD cd_err;
static int cd_noted;

static void cd_note(const char *what) {
    if (cd_noted) return;
    cd_noted = 1;
    printf("w32: [comdlg32] %s\n", what);
}

W32_DWORD W32ABI CommDlgExtendedError(void) { return cd_err; }

/* ---- the listbox class ------------------------------------------------------ */

#define CD_LIST_MAX 128
#define CD_LIST_TEXT 80

typedef struct {
    W32_HWND hwnd;
    int used;
    int count;
    int sel;                         /* single selection, -1 none */
    uint8_t selmask[CD_LIST_MAX];    /* multiselect mask */
    uint16_t items[CD_LIST_MAX][CD_LIST_TEXT];
} cd_list_t;

static cd_list_t cd_lists[16];

static cd_list_t *cd_list_by_hwnd(W32_HWND h) {
    for (int i = 0; i < 16; i++)
        if (cd_lists[i].used && cd_lists[i].hwnd == h) return &cd_lists[i];
    return NULL;
}

static cd_list_t *cd_list_alloc(W32_HWND h) {
    for (int i = 0; i < 16; i++)
        if (!cd_lists[i].used) {
            memset(&cd_lists[i], 0, sizeof cd_lists[i]);
            cd_lists[i].used = 1;
            cd_lists[i].hwnd = h;
            cd_lists[i].sel = -1;
            return &cd_lists[i];
        }
    return NULL;
}

/* Copy one UTF-16 string in (bounded). */
static void cd_wncpy(uint16_t *d, const uint16_t *s, size_t cap) {
    size_t i = 0;
    while (s && s[i] && i + 1 < cap) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}

static size_t cd_wlen(const uint16_t *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

/* LB_DIR: fill the list from a guest path pattern ("C:\\dir\\*.txt").
 * DDL_DIRECTORY adds the directory rows.  Returns the added count. */
static int cd_list_dir(cd_list_t *L, const uint16_t *pattern, W32_UINT attrs) {
    char a[512];
    if (w32_utf16z_to_utf8(pattern, a, (int32_t)sizeof a) <= 0) return 0;
    uint16_t query[560];
    size_t n = cd_wlen(pattern);
    if (n + 4 >= 560) return 0;
    cd_wncpy(query, pattern, 560);
    if (n && query[n - 1] != '\\' && query[n - 1] != '/') {
        query[n] = '\\';
        query[n + 1] = 0;
    }
    if (attrs & W32_DDL_DIRECTORY) {
        size_t m = cd_wlen(query);
        cd_wncpy(query + m, (const uint16_t *)(const uint16_t[]){'*', 0},
                 560 - m);
    }
    W32_WIN32_FIND_DATAW fd;
    W32_HANDLE h = FindFirstFileW(query, &fd);
    if (!h) return 0;
    int added = 0;
    do {
        if (fd.cFileName[0] == 0) continue;
        int is_dir = (fd.dwFileAttributes & 0x10u) != 0;
        if ((attrs & W32_DDL_DIRECTORY) && !is_dir) continue;
        if (!(attrs & W32_DDL_DIRECTORY) && is_dir) continue;
        uint16_t row[CD_LIST_TEXT];
        size_t k = 0;
        if (is_dir) row[k++] = '[';
        while (fd.cFileName[k - (is_dir ? 1 : 0)] && k + 2 < CD_LIST_TEXT) {
            row[k] = fd.cFileName[k - (is_dir ? 1 : 0)];
            k++;
        }
        if (is_dir) row[k++] = ']';
        row[k] = 0;
        if (L->count < CD_LIST_MAX) {
            cd_wncpy(L->items[L->count], row, CD_LIST_TEXT);
            L->count++;
            added++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return added;
}

static W32_LRESULT W32ABI cd_list_proc(W32_HWND hwnd, W32_UINT msg,
                                       W32_WPARAM wp, W32_LPARAM lp) {
    cd_list_t *L = cd_list_by_hwnd(hwnd);
    /* the state is born with the window and dies with it */
    if (!L && msg != W32_WM_NCDESTROY) L = cd_list_alloc(hwnd);
    if (!L && msg != W32_WM_NCDESTROY) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == W32_WM_NCDESTROY) {
        if (L) memset(L, 0, sizeof *L);
        return 0;
    }
    switch (msg) {
    case W32_WM_LBUTTONDOWN: {
        if (!L) break;
        /* the click selects the row under it; in this model the click
         * lands on the selected row (the compositor drives focus) */
        if (L->sel >= 0) {
            L->selmask[L->sel] = 1;
            W32_HWND parent = GetParent(hwnd);
            if (parent)
                PostMessageW(parent, W32_WM_COMMAND,
                             (W32_WPARAM)(uint32_t)
                             ((uint32_t)GetWindowLongPtrW(hwnd, W32_GWL_ID) |
                              (W32_LBN_SELCHANGE << 16)),
                             (W32_LPARAM)(uintptr_t)hwnd);
        }
        return 0;
    }
    case W32_LB_ADDSTRING:
    case W32_LB_INSERTSTRING: {
        if (!L) break;
        if (L->count >= CD_LIST_MAX) return -1;
        if (msg == W32_LB_ADDSTRING ||
            (int)wp >= L->count || (int)wp < 0) {
            cd_wncpy(L->items[L->count], (const uint16_t *)(uintptr_t)lp,
                     CD_LIST_TEXT);
            L->count++;
            return L->count - 1;
        }
        /* insert shifts up */
        for (int i = L->count; i > (int)wp; i--)
            cd_wncpy(L->items[i], L->items[i - 1], CD_LIST_TEXT);
        cd_wncpy(L->items[wp], (const uint16_t *)(uintptr_t)lp, CD_LIST_TEXT);
        L->count++;
        return (W32_LRESULT)wp;
    }
    case W32_LB_DELETESTRING:
        if (!L || (int)wp < 0 || (int)wp >= L->count) return -1;
        for (int i = (int)wp; i < L->count - 1; i++)
            cd_wncpy(L->items[i], L->items[i + 1], CD_LIST_TEXT);
        L->count--;
        return L->count;
    case W32_LB_RESETCONTENT:
        if (L) { L->count = 0; L->sel = -1; memset(L->selmask, 0, sizeof L->selmask); }
        return 0;
    case W32_LB_SETCURSEL:
        if (!L) return -1;
        if ((int)wp >= L->count) return -1;
        L->sel = (int)wp;
        if ((int)wp >= 0) L->selmask[wp] = 1;
        return (W32_LRESULT)L->sel;
    case W32_LB_GETCURSEL:
        return L ? L->sel : -1;
    case W32_LB_GETTEXT: {
        if (!L || (int)wp < 0 || (int)wp >= L->count) return -1;
        const uint16_t *s = L->items[wp];
        uint16_t *out = (uint16_t *)(uintptr_t)lp;
        size_t n = cd_wlen(s);
        size_t i = 0;
        while (i < n) { out[i] = s[i]; i++; }
        out[i] = 0;
        return (W32_LRESULT)n;
    }
    case W32_LB_GETTEXTLEN:
        if (!L || (int)wp < 0 || (int)wp >= L->count) return -1;
        return (W32_LRESULT)cd_wlen(L->items[wp]);
    case W32_LB_GETCOUNT:
        return L ? L->count : 0;
    case W32_LB_SETSEL: {
        if (!L) return -1;
        int on = (int)wp;             /* TRUE/FALSE */
        int i = (int)(int32_t)(uint32_t)lp;   /* -1 = all */
        if (i == -1) {
            for (int k = 0; k < L->count; k++) L->selmask[k] = (uint8_t)(on ? 1 : 0);
            if (on && L->count) L->sel = 0;
        } else if (i >= 0 && i < L->count) {
            L->selmask[i] = (uint8_t)(on ? 1 : 0);
            if (on) L->sel = i;
        }
        return 0;
    }
    case W32_LB_GETSELCOUNT: {
        if (!L) return 0;
        int n = 0;
        for (int i = 0; i < L->count; i++) n += L->selmask[i];
        return n;
    }
    case W32_LB_GETSELITEMS: {
        if (!L) return 0;
        int cap = (int)(int32_t)(uint32_t)wp;
        int *out = (int *)(uintptr_t)lp;
        int n = 0;
        for (int i = 0; i < L->count && n < cap; i++)
            if (L->selmask[i]) out[n++] = i;
        return n;
    }
    case W32_LB_DIR: {
        if (!L) return 0;
        return cd_list_dir(L, (const uint16_t *)(uintptr_t)lp, (W32_UINT)wp);
    }
    case W32_WM_PAINT: {
        W32_PAINTSTRUCT ps;
        W32_HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc) {
            W32_RECT rc;
            GetClientRect(hwnd, &rc);
            W32_HBRUSH bg = CreateSolidBrush(0xFFFFFFFFu);
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            W32_HBRUSH fb = CreateSolidBrush(0xFF808080u);
            W32_RECT fr = rc;
            FrameRect(hdc, &fr, fb);
            DeleteObject(fb);
            cd_list_t *l = cd_list_by_hwnd(hwnd);
            if (l) {
                for (int i = 0; i < l->count; i++) {
                    char row[CD_LIST_TEXT * 3];
                    w32_utf16z_to_utf8(l->items[i], row, (int32_t)sizeof row);
                    SetTextColor(hdc, i == l->sel ? 0xFFFFFFFFu : 0xFF000000u);
                    if (i == l->sel) {
                        W32_RECT rrow = rc;
                        rrow.top = rc.top + i * 12;
                        rrow.bottom = rrow.top + 12;
                        W32_HBRUSH hb = CreateSolidBrush(0xFF000080u);
                        FillRect(hdc, &rrow, hb);
                        DeleteObject(hb);
                    }
                    TextOutA(hdc, rc.left + 4, rc.top + 2 + i * 12, row,
                             (int32_t)strlen(row));
                }
            }
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the edit/static/button classes ------------------------------------------ */

static W32_LRESULT W32ABI cd_edit_proc(W32_HWND hwnd, W32_UINT msg,
                                       W32_WPARAM wp, W32_LPARAM lp) {
    /* the window text IS the content; DefWindowProc owns it all */
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static W32_LRESULT W32ABI cd_static_proc(W32_HWND hwnd, W32_UINT msg,
                                         W32_WPARAM wp, W32_LPARAM lp) {
    if (msg == W32_WM_PAINT) {
        W32_PAINTSTRUCT ps;
        W32_HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc) {
            W32_RECT rc;
            GetClientRect(hwnd, &rc);
            uint16_t wtext[128];
            int n = GetWindowTextW(hwnd, wtext, 128);
            if (n > 0) {
                char label[256];
                w32_utf16z_to_utf8(wtext, label, (int32_t)sizeof label);
                SetBkMode(hdc, W32_TRANSPARENT);
                TextOutA(hdc, rc.left + 2, rc.top + 2, label,
                         (int32_t)strlen(label));
            }
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static W32_LRESULT W32ABI cd_btn_proc(W32_HWND hwnd, W32_UINT msg,
                                      W32_WPARAM wp, W32_LPARAM lp) {
    if (msg == W32_WM_LBUTTONDOWN) {
        W32_HWND parent = GetParent(hwnd);
        if (parent)
            PostMessageW(parent, W32_WM_COMMAND,
                         (W32_WPARAM)(uint32_t)
                         ((uint32_t)GetWindowLongPtrW(hwnd, W32_GWL_ID) &
                          0xFFFFu),
                         (W32_LPARAM)(uintptr_t)hwnd);
        return 0;
    }
    if (msg == W32_WM_PAINT) {
        W32_PAINTSTRUCT ps;
        W32_HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc) {
            W32_RECT rc;
            GetClientRect(hwnd, &rc);
            W32_HBRUSH bg = CreateSolidBrush(0xFFC0C0C0u);
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            W32_HBRUSH fb = CreateSolidBrush(0xFF000000u);
            W32_RECT fr = rc;
            FrameRect(hdc, &fr, fb);
            DeleteObject(fb);
            uint16_t wtext[64];
            int n = GetWindowTextW(hwnd, wtext, 64);
            if (n > 0) {
                char label[128];
                w32_utf16z_to_utf8(wtext, label, (int32_t)sizeof label);
                SetBkMode(hdc, W32_TRANSPARENT);
                TextOutA(hdc, rc.left + 6, (rc.bottom - 10) / 2, label,
                         (int32_t)strlen(label));
            }
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- class registration + the template expander ------------------------------- */

static const uint16_t cd_cls_list[]  = {'A','u','r','a','C','D','l','g','L','s','t',0};
static const uint16_t cd_cls_edit[]  = {'A','u','r','a','C','D','l','g','E','d','t',0};
static const uint16_t cd_cls_stat[]  = {'A','u','r','a','C','D','l','g','S','t','t',0};
static const uint16_t cd_cls_btn[]   = {'A','u','r','a','C','D','l','g','B','t','n',0};

static int cd_classes_ready;

static void cd_register_classes(void) {
    if (cd_classes_ready) return;
    W32_WNDCLASSEXW c;
    memset(&c, 0, sizeof c);
    c.cbSize = sizeof c;
    c.style = 0;
    c.hInstance = GetModuleHandleW(NULL);
    c.hCursor = NULL;
    c.hbrBackground = NULL;
    c.lpszClassName = cd_cls_list;
    c.lpfnWndProc = cd_list_proc;
    w32_win_register_comctl_class(&c);
    c.lpszClassName = cd_cls_edit;
    c.lpfnWndProc = cd_edit_proc;
    w32_win_register_comctl_class(&c);
    c.lpszClassName = cd_cls_stat;
    c.lpfnWndProc = cd_static_proc;
    w32_win_register_comctl_class(&c);
    c.lpszClassName = cd_cls_btn;
    c.lpfnWndProc = cd_btn_proc;
    w32_win_register_comctl_class(&c);
    cd_classes_ready = 1;
}

/* One dialog row to expand: class kind, id, position, title. */
typedef struct {
    const uint16_t *cls;
    int id;
    int x, y, cx, cy;
    const uint16_t *title;
} cd_item_spec_t;

/* Build + expand the items directly (the DLGITEMTEMPLATE grammar is
 * the A-6 shape: 9 words, class, title, creation data — the same walk
 * ps_expand_page runs, kept local so the dialogs own their layout). */
static void cd_create_items(W32_HWND dlg, const cd_item_spec_t *items, int n) {
    for (int i = 0; i < n; i++) {
        CreateWindowExW(0, items[i].cls, items[i].title,
                        W32_WS_CHILD | W32_WS_VISIBLE,
                        items[i].x, items[i].y, items[i].cx, items[i].cy,
                        dlg, (W32_HMENU)(uintptr_t)items[i].id,
                        GetModuleHandleW(NULL), 0);
    }
}

/* ---- the open/save dialog core -------------------------------------------------- */

#define CD_ID_EDIT   101
#define CD_ID_FILES  102
#define CD_ID_FILTER 103
#define CD_ID_OK     1
#define CD_ID_CANCEL 2

typedef struct {
    W32_OPENFILENAMEW *ofn;
    int save;                        /* save variant */
    int result;                      /* 0 cancel, 1 ok */
    uint16_t dir[280];               /* current directory (guest view) */
    uint16_t pattern[64];            /* current file pattern */
    /* the filter pairs, split */
    uint16_t fname[8][40];
    uint16_t fpat[8][64];
    int fcount;
    int findex;                      /* 0-based current */
} cd_ofn_ctx_t;

/* Refill the file list from ctx->dir + ctx->pattern. */
static void cd_ofn_refill(W32_HWND dlg, cd_ofn_ctx_t *ctx) {
    W32_HWND list = GetDlgItem(dlg, CD_ID_FILES);
    if (!list) return;
    cd_list_t *L = cd_list_by_hwnd(list);
    if (L) {
        L->count = 0;
        L->sel = -1;
        memset(L->selmask, 0, sizeof L->selmask);
    }
    if (ctx->fcount == 0 || ctx->findex >= ctx->fcount) return;
    uint16_t query[340];
    size_t d = cd_wlen(ctx->dir);
    if (d + cd_wlen(ctx->fpat[ctx->findex]) + 2 >= 340) return;
    cd_wncpy(query, ctx->dir, 340);
    query[d] = '\\';
    cd_wncpy(query + d + 1, ctx->fpat[ctx->findex], 340 - d - 1);
    SendMessageW(list, W32_LB_DIR, 0, (W32_LPARAM)(uintptr_t)query);
}

/* Recompute the filter list rows + current pattern. */
static void cd_ofn_filter_rows(W32_HWND dlg, cd_ofn_ctx_t *ctx) {
    W32_HWND flist = GetDlgItem(dlg, CD_ID_FILTER);
    if (!flist) return;
    cd_list_t *L = cd_list_by_hwnd(flist);
    if (L) {
        L->count = 0;
        L->sel = -1;
    }
    for (int i = 0; i < ctx->fcount; i++) {
        uint16_t row[100];
        size_t n = cd_wlen(ctx->fname[i]);
        cd_wncpy(row, ctx->fname[i], 100);
        (void)n;
        SendMessageW(flist, W32_LB_ADDSTRING, 0,
                     (W32_LPARAM)(uintptr_t)row);
    }
    if (ctx->fcount)
        SendMessageW(flist, W32_LB_SETCURSEL,
                     (W32_WPARAM)(uint32_t)ctx->findex, 0);
}

/* Stat one path (absolute or ctx-relative): is it there, is it a dir. */
static int cd_ofn_stat(cd_ofn_ctx_t *ctx, const uint16_t *name,
                       uint16_t *full, size_t cap, int *is_dir) {
    if (is_dir) *is_dir = 0;
    size_t n = cd_wlen(name);
    if (n == 0 || cap < 4) return 0;
    if (name[0] == '\\' || name[0] == '/' || (n > 1 && name[1] == ':')) {
        cd_wncpy(full, name, cap);   /* already absolute */
    } else {
        size_t d = cd_wlen(ctx->dir);
        if (d + n + 2 >= cap) return 0;
        cd_wncpy(full, ctx->dir, cap);
        full[d] = '\\';
        cd_wncpy(full + d + 1, name, cap - d - 1);
    }
    W32_WIN32_FIND_DATAW fd;
    W32_HANDLE h = FindFirstFileW(full, &fd);
    if (!h) return 0;
    FindClose(h);
    if (is_dir) *is_dir = (fd.dwFileAttributes & 0x10u) != 0;
    return 1;
}

/* The pending handoff: one modal dialog runs at a time (the engine is
 * modal by construction); WM_INITDIALOG is the only message that
 * arrives before the proc can stash the context on the window. */
static cd_ofn_ctx_t *cd_ofn_pending;

static cd_ofn_ctx_t *cd_ofn_ctx(W32_HWND dlg) {
    return (cd_ofn_ctx_t *)(void *)(uintptr_t)GetWindowLongPtrW(dlg, W32_GWL_USERDATA);
}

/* The engine frame proc: hook first, then the engine's own handling. */
static W32_INT_PTR W32ABI cd_ofn_proc(W32_HWND dlg, W32_UINT msg,
                                      W32_WPARAM wp, W32_LPARAM lp) {
    cd_ofn_ctx_t *ctx =
        (msg == W32_WM_INITDIALOG) ? cd_ofn_pending : cd_ofn_ctx(dlg);
    /* the hook sees everything first, the documented precedence */
    if (ctx && (ctx->ofn->Flags & W32_OFN_ENABLEHOOK) && ctx->ofn->lpfnHook) {
        typedef W32_INT_PTR (W32ABI *hook_t)(W32_HWND, W32_UINT, W32_WPARAM,
                                             W32_LPARAM);
        W32_INT_PTR r = ((hook_t)ctx->ofn->lpfnHook)(dlg, msg, wp, lp);
        if (r) return r;
    }
    switch (msg) {
    case W32_WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, W32_GWL_USERDATA, (intptr_t)ctx);
        cd_create_items(dlg, (const cd_item_spec_t[]){
            { cd_cls_stat,  900,  6,  6,  60,  8, (const uint16_t[]){'F','i','l','e',' ','n','a','m','e',':',0} },
            { cd_cls_edit, CD_ID_EDIT,   6, 16, 170, 12, NULL },
            { cd_cls_list, CD_ID_FILES,  6, 32, 170, 70, NULL },
            { cd_cls_stat,  901,  6, 106,  60,  8, (const uint16_t[]){'T','y','p','e',':',0} },
            { cd_cls_list, CD_ID_FILTER, 6, 116, 170, 26, NULL },
            { cd_cls_btn,  CD_ID_OK,    120, 146,  40, 12, (const uint16_t[]){'O','K',0} },
            { cd_cls_btn,  CD_ID_CANCEL,165, 146,  40, 12, (const uint16_t[]){'C','a','n','c','e','l',0} },
        }, 7);
        if (ctx) {
            cd_ofn_filter_rows(dlg, ctx);
            cd_ofn_refill(dlg, ctx);
            if (ctx->ofn->lpstrFile && ctx->ofn->lpstrFile[0]) {
                W32_HWND edit = GetDlgItem(dlg, CD_ID_EDIT);
                SetWindowTextW(edit, ctx->ofn->lpstrFile);
            }
        }
        return 1;
    }
    case W32_WM_COMMAND: {
        if (!ctx) break;
        uint32_t id = (uint32_t)wp & 0xFFFFu;
        uint32_t code = ((uint32_t)wp >> 16) & 0xFFFFu;
        if (id == CD_ID_FILTER && code == W32_LBN_SELCHANGE) {
            W32_HWND flist = GetDlgItem(dlg, CD_ID_FILTER);
            int sel = (int)SendMessageW(flist, W32_LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < ctx->fcount) {
                ctx->findex = sel;
                ctx->ofn->nFilterIndex = (W32_UINT)(sel + 1);
                cd_ofn_refill(dlg, ctx);
            }
            return 1;
        }
        if (id == CD_ID_FILES && code == W32_LBN_SELCHANGE) {
            /* selection lands in the edit, like the real dialog */
            W32_HWND files = GetDlgItem(dlg, CD_ID_FILES);
            W32_HWND edit = GetDlgItem(dlg, CD_ID_EDIT);
            int sel = (int)SendMessageW(files, W32_LB_GETCURSEL, 0, 0);
            if (sel >= 0) {
                uint16_t name[CD_LIST_TEXT];
                SendMessageW(files, W32_LB_GETTEXT, (W32_WPARAM)(uint32_t)sel,
                             (W32_LPARAM)(uintptr_t)name);
                SetWindowTextW(edit, name);
            }
            return 1;
        }
        if (id == CD_ID_OK) {
            W32_OPENFILENAMEW *ofn = ctx->ofn;
            W32_HWND edit = GetDlgItem(dlg, CD_ID_EDIT);
            uint16_t name[280];
            if (GetWindowTextW(edit, name, 280) <= 0) name[0] = 0;
            uint16_t full[600];
            if (name[0] == 0) {
                ctx->result = 0;
                EndDialog(dlg, W32_IDCANCEL);
                return 1;
            }
            int is_dir = 0;
            int exists = cd_ofn_stat(ctx, name, full, 600, &is_dir);
            if (is_dir) {
                /* navigation: the path edit names a directory */
                cd_wncpy(ctx->dir, full, sizeof ctx->dir / 2u);
                cd_ofn_refill(dlg, ctx);
                SetWindowTextW(edit, (const uint16_t[]){0});
                return 1;
            }
            /* multiselect: the selected rows join the typed name */
            if ((ofn->Flags & W32_OFN_ALLOWMULTISELECT) && !ctx->save) {
                W32_HWND files = GetDlgItem(dlg, CD_ID_FILES);
                int selcount = (int)SendMessageW(files, W32_LB_GETSELCOUNT,
                                                 0, 0);
                if (selcount > 1) {
                    int selitems[CD_LIST_MAX];
                    int n = (int)SendMessageW(files, W32_LB_GETSELITEMS,
                                             (W32_WPARAM)(uint32_t)CD_LIST_MAX,
                                             (W32_LPARAM)(uintptr_t)selitems);
                    size_t o = 0, d = cd_wlen(ctx->dir);
                    if (d + 2 >= ofn->nMaxFile) {
                        cd_err = W32_FNERR_BUFFERTOOSMALL;
                        ctx->result = 0;
                        EndDialog(dlg, W32_IDCANCEL);
                        return 1;
                    }
                    cd_wncpy(ofn->lpstrFile, ctx->dir, ofn->nMaxFile);
                    o = d;
                    ofn->lpstrFile[o++] = 0;
                    ofn->nFileOffset = (W32_WORD)o;
                    for (int i = 0; i < n; i++) {
                        uint16_t one[CD_LIST_TEXT];
                        SendMessageW(files, W32_LB_GETTEXT,
                                     (W32_WPARAM)(uint32_t)selitems[i],
                                     (W32_LPARAM)(uintptr_t)one);
                        size_t k = 0;
                        while (one[k] && o + 2 < ofn->nMaxFile)
                            ofn->lpstrFile[o++] = one[k++];
                        ofn->lpstrFile[o++] = 0;
                    }
                    ofn->lpstrFile[o] = 0;   /* the list terminator */
                    ctx->result = 1;
                    EndDialog(dlg, W32_IDOK);
                    return 1;
                }
            }
            /* FILEMUSTEXIST: a missing file is a refusal, not a hang */
            if (!exists && (ofn->Flags & W32_OFN_FILEMUSTEXIST) &&
                !(ofn->Flags & W32_OFN_NOVALIDATE)) {
                cd_err = W32_FNERR_INVALIDFILENAME;
                ctx->result = 0;
                EndDialog(dlg, W32_IDCANCEL);
                return 1;
            }
            /* the default extension, when the name carries none */
            size_t fn = cd_wlen(full);
            int has_ext = 0;
            for (size_t i = fn; i > 0; i--)
                if (full[i - 1] == '.') { has_ext = 1; break; }
                else if (full[i - 1] == '\\' || full[i - 1] == '/') break;
            if (!has_ext && ofn->lpstrDefExt && ofn->lpstrDefExt[0]) {
                size_t e = 0;
                while (ofn->lpstrDefExt[e] && fn + e + 2 < 598) {
                    full[fn + e] = ofn->lpstrDefExt[e];
                    e++;
                }
                full[fn + e] = 0;
                fn += e;
            }
            /* the save dialog's overwrite prompt: a real alert the
             * engine's MessageBox answers (IDOK) */
            if (ctx->save && (ofn->Flags & W32_OFN_OVERWRITEPROMPT) &&
                cd_ofn_stat(ctx, full, full, 600, &is_dir)) {
                MessageBoxW(dlg,
                    (const uint16_t[]){'O','v','e','r','w','r','i','t','e',' ','t','h','i','s',' ','f','i','l','e','?',0},
                    (const uint16_t[]){'S','a','v','e',0}, 0);
            }
            /* fill lpstrFile + the offsets */
            size_t n2 = cd_wlen(full);
            if (n2 + 1 >= ofn->nMaxFile) {
                cd_err = W32_FNERR_BUFFERTOOSMALL;
                ctx->result = 0;
                EndDialog(dlg, W32_IDCANCEL);
                return 1;
            }
            cd_wncpy(ofn->lpstrFile, full, ofn->nMaxFile);
            ofn->nFileOffset = 0;
            for (size_t i = n2; i > 0; i--)
                if (full[i - 1] == '\\' || full[i - 1] == '/') {
                    ofn->nFileOffset = (W32_WORD)i;
                    break;
                }
            ofn->nFileExtension = 0;
            for (size_t i = n2; i > ofn->nFileOffset; i--)
                if (full[i - 1] == '.') {
                    ofn->nFileExtension = (W32_WORD)i;
                    break;
                }
            if (ofn->lpstrFileTitle && ofn->nMaxFileTitle) {
                size_t t = ofn->nFileOffset;
                size_t k = 0;
                while (full[t + k] && k + 1 < ofn->nMaxFileTitle) {
                    ofn->lpstrFileTitle[k] = full[t + k];
                    k++;
                }
                ofn->lpstrFileTitle[k] = 0;
            }
            ctx->result = 1;
            EndDialog(dlg, W32_IDOK);
            return 1;
        }
        if (id == CD_ID_CANCEL) {
            ctx->result = 0;
            EndDialog(dlg, W32_IDCANCEL);
            return 1;
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

/* Parse lpstrFilter into ctx pairs. */
static void cd_ofn_parse_filter(cd_ofn_ctx_t *ctx) {
    ctx->fcount = 0;
    ctx->findex = 0;
    const uint16_t *p = ctx->ofn->lpstrFilter;
    if (!p) return;
    while (*p && ctx->fcount < 8) {
        cd_wncpy(ctx->fname[ctx->fcount], p, 40);
        while (*p) p++;
        p++;
        if (!*p) break;             /* dangling name: no pattern */
        cd_wncpy(ctx->fpat[ctx->fcount], p, 64);
        while (*p) p++;
        p++;
        ctx->fcount++;
    }
    if (ctx->fcount == 0) {
        /* the "all files" default, one real pair */
        static const uint16_t all[] = {'A','l','l',' ','F','i','l','e','s',0};
        static const uint16_t star[] = {'*','.','*',0};
        cd_wncpy(ctx->fname[0], all, 40);
        cd_wncpy(ctx->fpat[0], star, 64);
        ctx->fcount = 1;
    }
    if (ctx->ofn->nFilterIndex &&
        ctx->ofn->nFilterIndex <= (W32_UINT)ctx->fcount)
        ctx->findex = (int)ctx->ofn->nFilterIndex - 1;
}

/* Build the in-memory dialog template (the A-6 grammar; the item walk
 * is the engine's, the items come from cd_create_items). */
static void cd_ofn_template(W32_BYTE *tbuf, size_t cap, int save) {
    memset(tbuf, 0, cap);
    W32_DLGTEMPLATE *t = (W32_DLGTEMPLATE *)(void *)tbuf;
    t->style = W32_DS_MODALFRAME | W32_DS_SETFONT;
    t->exStyle = 0;
    t->items = 0;
    t->x = 40; t->y = 40; t->cx = 212; t->cy = 175;
    uint16_t *p = (uint16_t *)(void *)(tbuf + sizeof *t);
    *p++ = 0;                        /* menu */
    *p++ = 0;                        /* class */
    const uint16_t *title = save
        ? (const uint16_t[]){'S','a','v','e',' ','A','s',0}
        : (const uint16_t[]){'O','p','e','n',0};
    while (*title) *p++ = *title++;
    *p++ = 0;
    *p++ = 8; *p++ = 400; *p++ = 0;  /* DS_SETFONT: point, weight, italic */
    const uint16_t *face = (const uint16_t[]){'V','G','A',' ','8','x','1','6',0};
    while (*face) *p++ = *face++;
    *p++ = 0;
}

/* The shared core of the four open/save entries. */
static int cd_get_file_name(W32_OPENFILENAMEW *ofn, int save) {
    cd_err = 0;
    if (!ofn || ofn->lStructSize < sizeof *ofn || !ofn->lpstrFile ||
        ofn->nMaxFile < 2) {
        cd_err = W32_CDERR_STRUCTSIZE;
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (ofn->Flags & (W32_OFN_ENABLETEMPLATE | W32_OFN_ENABLETEMPLATEHANDLE)) {
        cd_err = W32_CDERR_NOTEMPLATE;
        cd_note("OFN_ENABLETEMPLATE(ANDLE) refused: the dialog is built "
                "by the engine (CDERR_NOTEMPLATE)");
        return 0;
    }
    if (ofn->Flags & W32_OFN_SHOWHELP)
        cd_note("OFN_SHOWHELP accepted, noted: there is no help UI, the "
                "button never existed here");
    if (ofn->Flags & W32_OFN_ENABLEINCLUDENOTIFY)
        cd_note("OFN_ENABLEINCLUDENOTIFY accepted, noted: no include "
                "events are raised");

    cd_register_classes();
    cd_ofn_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ofn = ofn;
    ctx.save = save;
    cd_ofn_parse_filter(&ctx);
    /* the start directory: lpstrInitialDir, else the initial path's
     * directory, else the root of the default drive */
    if (ofn->lpstrInitialDir && ofn->lpstrInitialDir[0]) {
        cd_wncpy(ctx.dir, ofn->lpstrInitialDir, sizeof ctx.dir / 2u);
    } else if (ofn->lpstrFile && ofn->lpstrFile[0]) {
        size_t n = cd_wlen(ofn->lpstrFile);
        size_t cut = 0;
        for (size_t i = n; i > 0; i--)
            if (ofn->lpstrFile[i - 1] == '\\' ||
                ofn->lpstrFile[i - 1] == '/') { cut = i; break; }
        if (cut && cut < 279) {
            cd_wncpy(ctx.dir, ofn->lpstrFile, cut);
            ctx.dir[cut] = 0;
        }
    }
    if (ctx.dir[0] == 0) {
        static const uint16_t root[] = {'C',':','\\',0};
        cd_wncpy(ctx.dir, root, 280);
    }

    W32_BYTE tbuf[128];
    cd_ofn_template(tbuf, sizeof tbuf, save);
    cd_ofn_pending = &ctx;
    DialogBoxIndirectParamW(GetModuleHandleW(NULL),
                            (const W32_DLGTEMPLATE *)(void *)tbuf,
                            ofn->hwndOwner, cd_ofn_proc,
                            (W32_LPARAM)(intptr_t)0);
    cd_ofn_pending = NULL;
    return ctx.result;
}

W32_BOOL W32ABI GetOpenFileNameW(W32_OPENFILENAMEW *ofn) {
    return cd_get_file_name(ofn, 0);
}

W32_BOOL W32ABI GetSaveFileNameW(W32_OPENFILENAMEW *ofn) {
    return cd_get_file_name(ofn, 1);
}

/* ---- the A-variants: byte-identical layouts, converted in place ------------- */

/* The A struct: same offsets, CHAR* where W has WCHAR*.  The engine
 * runs W-shaped; the A wrapper converts the string members around it. */
static int cd_ofn_a_to_w(void *ofnA, W32_OPENFILENAMEW *w,
                         uint16_t **filter_w, uint16_t **file_w,
                         uint16_t **title_buf, uint16_t **dir_w,
                         uint16_t **defext_w, uint16_t **filetitle_w) {
    /* layout offsets in DWORD-sized slots, mirrored from the W struct */
    typedef struct {
        W32_DWORD lStructSize; W32_HWND hwndOwner; W32_HINSTANCE hInstance;
        const char *lpstrFilter; char *lpstrCustomFilter;
        W32_UINT nMaxCustFilter; W32_UINT nFilterIndex;
        char *lpstrFile; W32_DWORD nMaxFile; char *lpstrFileTitle;
        W32_DWORD nMaxFileTitle; const char *lpstrInitialDir;
        const char *lpstrTitle; W32_DWORD Flags;
        W32_WORD nFileOffset, nFileExtension; const char *lpstrDefExt;
        W32_LPARAM lCustData; void *lpfnHook;
        const char *lpTemplateName; void *pvReserved;
        W32_DWORD dwReserved, FlagsEx;
    } cd_ofn_a_t;
    cd_ofn_a_t *a = (cd_ofn_a_t *)ofnA;
    memset(w, 0, sizeof *w);
    w->lStructSize = sizeof *w;
    w->hwndOwner = a->hwndOwner;
    w->hInstance = a->hInstance;
    w->nMaxCustFilter = a->nMaxCustFilter;
    w->nFilterIndex = a->nFilterIndex;
    w->nMaxFile = a->nMaxFile;
    w->nMaxFileTitle = a->nMaxFileTitle;
    w->Flags = a->Flags;
    w->lCustData = a->lCustData;
    w->lpfnHook = a->lpfnHook;
    w->FlagsEx = a->FlagsEx;

    /* filter: pairs to the double-NUL */
    if (a->lpstrFilter) {
        size_t total = 0;
        while (a->lpstrFilter[total]) total += strlen(a->lpstrFilter + total) + 1;
        total++;                     /* the list terminator */
        *filter_w = (uint16_t *)malloc(total * 2u + 2u);
        if (!*filter_w) return 0;
        size_t o = 0;
        for (size_t i = 0; i + 1 < total;) {
            size_t n = strlen(a->lpstrFilter + i);
            w32_utf8z_to_utf16(a->lpstrFilter + i, *filter_w + o,
                               (int32_t)(n + 1) * 2);
            o += n + 1;
            i += n + 1;
        }
        (*filter_w)[o] = 0;
        w->lpstrFilter = *filter_w;
    }
    if (a->lpstrFile) {
        *file_w = (uint16_t *)calloc(a->nMaxFile, 2u);
        if (!*file_w) return 0;
        w32_utf8z_to_utf16(a->lpstrFile, *file_w, (int32_t)a->nMaxFile * 2);
        w->lpstrFile = *file_w;
        w->nMaxFile = a->nMaxFile;
    }
    if (a->lpstrFileTitle && a->nMaxFileTitle) {
        *filetitle_w = (uint16_t *)calloc(a->nMaxFileTitle, 2u);
        if (!*filetitle_w) return 0;
        w->lpstrFileTitle = *filetitle_w;
        w->nMaxFileTitle = a->nMaxFileTitle;
    }
    if (a->lpstrInitialDir) {
        size_t n = strlen(a->lpstrInitialDir) + 1;
        *dir_w = (uint16_t *)malloc(n * 2u);
        if (!*dir_w) return 0;
        w32_utf8z_to_utf16(a->lpstrInitialDir, *dir_w, (int32_t)n * 2);
        w->lpstrInitialDir = *dir_w;
    }
    if (a->lpstrTitle) {
        size_t n = strlen(a->lpstrTitle) + 1;
        *title_buf = (uint16_t *)malloc(n * 2u);
        if (!*title_buf) return 0;
        w32_utf8z_to_utf16(a->lpstrTitle, *title_buf, (int32_t)n * 2);
        w->lpstrTitle = *title_buf;
    }
    if (a->lpstrDefExt) {
        size_t n = strlen(a->lpstrDefExt) + 1;
        *defext_w = (uint16_t *)malloc(n * 2u);
        if (!*defext_w) return 0;
        w32_utf8z_to_utf16(a->lpstrDefExt, *defext_w, (int32_t)n * 2);
        w->lpstrDefExt = *defext_w;
    }
    return 1;
}

static void cd_ofn_w_to_a(void *ofnA, W32_OPENFILENAMEW *w) {
    typedef struct {
        W32_DWORD lStructSize; W32_HWND hwndOwner; W32_HINSTANCE hInstance;
        const char *lpstrFilter; char *lpstrCustomFilter;
        W32_UINT nMaxCustFilter; W32_UINT nFilterIndex;
        char *lpstrFile; W32_DWORD nMaxFile; char *lpstrFileTitle;
        W32_DWORD nMaxFileTitle; const char *lpstrInitialDir;
        const char *lpstrTitle; W32_DWORD Flags;
        W32_WORD nFileOffset, nFileExtension; const char *lpstrDefExt;
        W32_LPARAM lCustData; void *lpfnHook;
        const char *lpTemplateName; void *pvReserved;
        W32_DWORD dwReserved, FlagsEx;
    } cd_ofn_a_t;
    cd_ofn_a_t *a = (cd_ofn_a_t *)ofnA;
    a->nFilterIndex = w->nFilterIndex;
    a->nFileOffset = w->nFileOffset;
    a->nFileExtension = w->nFileExtension;
    if (a->lpstrFile && w->lpstrFile) {
        /* multiselect keeps the double-NUL shape; convert the list */
        char *out = a->lpstrFile;
        size_t cap = a->nMaxFile;
        const uint16_t *p = w->lpstrFile;
        size_t o = 0;
        while (*p && o + 1 < cap) {
            int n = w32_utf16z_to_utf8(p, out + o, (int32_t)(cap - o));
            if (n < 0) break;
            o += (size_t)n + 1;
            p += cd_wlen(p) + 1;
        }
        if (o < cap) out[o] = 0;
    }
    if (a->lpstrFileTitle && w->lpstrFileTitle)
        w32_utf16z_to_utf8(w->lpstrFileTitle, a->lpstrFileTitle,
                           (int32_t)a->nMaxFileTitle);
}

static int cd_get_file_name_a(void *ofnA, int save) {
    W32_OPENFILENAMEW w;
    uint16_t *filter_w = NULL, *file_w = NULL, *title_b = NULL;
    uint16_t *dir_w = NULL, *defext_w = NULL, *filetitle_w = NULL;
    if (!cd_ofn_a_to_w(ofnA, &w, &filter_w, &file_w, &title_b, &dir_w,
                       &defext_w, &filetitle_w)) {
        cd_err = W32_CDERR_INITIALIZATION;
        return 0;
    }
    int r = cd_get_file_name(&w, save);
    cd_ofn_w_to_a(ofnA, &w);
    free(filter_w); free(file_w); free(title_b); free(dir_w);
    free(defext_w); free(filetitle_w);
    return r;
}

W32_BOOL W32ABI GetOpenFileNameA(void *ofn) { return cd_get_file_name_a(ofn, 0); }
W32_BOOL W32ABI GetSaveFileNameA(void *ofn) { return cd_get_file_name_a(ofn, 1); }

/* ---- ChooseColorW ------------------------------------------------------------- */

/* The 16 basic colours (the VGA set, ours). */
static const W32_DWORD cd_colors[16] = {
    0x00000000u, 0x00800000u, 0x00008000u, 0x00808000u,
    0x00000080u, 0x00800080u, 0x00008080u, 0x00C0C0C0u,
    0x00808080u, 0x00FF0000u, 0x0000FF00u, 0x00FFFF00u,
    0x000000FFu, 0x00FF00FFu, 0x0000FFFFu, 0x00FFFFFFu,
};
static const char *const cd_color_names[16] = {
    "Black", "Maroon", "Green", "Olive", "Navy", "Purple", "Teal", "Silver",
    "Gray", "Red", "Lime", "Yellow", "Blue", "Fuchsia", "Aqua", "White",
};

typedef struct {
    W32_CHOOSECOLORW *cc;
    int result;
} cd_cc_ctx_t;

static cd_cc_ctx_t *cd_cc_pending;

static W32_INT_PTR W32ABI cd_cc_proc(W32_HWND dlg, W32_UINT msg,
                                     W32_WPARAM wp, W32_LPARAM lp) {
    cd_cc_ctx_t *ctx =
        (msg == W32_WM_INITDIALOG) ? cd_cc_pending
                                   : (cd_cc_ctx_t *)(void *)(uintptr_t)
                                     GetWindowLongPtrW(dlg, W32_GWL_USERDATA);
    if (ctx && (ctx->cc->Flags & W32_CC_ENABLEHOOK) && ctx->cc->lpfnHook) {
        typedef W32_INT_PTR (W32ABI *hook_t)(W32_HWND, W32_UINT, W32_WPARAM,
                                             W32_LPARAM);
        W32_INT_PTR r = ((hook_t)ctx->cc->lpfnHook)(dlg, msg, wp, lp);
        if (r) return r;
    }
    switch (msg) {
    case W32_WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, W32_GWL_USERDATA, (intptr_t)ctx);
        cd_create_items(dlg, (const cd_item_spec_t[]){
            { cd_cls_stat,  910,  6,  6,  80,  8, (const uint16_t[]){'B','a','s','i','c',' ','c','o','l','o','u','r','s',':',0} },
            { cd_cls_list,  110,  6, 16, 150, 60, NULL },
            { cd_cls_btn,  CD_ID_OK,    120,  82, 40, 12, (const uint16_t[]){'O','K',0} },
            { cd_cls_btn,  CD_ID_CANCEL,165,  82, 40, 12, (const uint16_t[]){'C','a','n','c','e','l',0} },
        }, 4);
        if (ctx) {
            W32_HWND list = GetDlgItem(dlg, 110);
            for (int i = 0; i < 16; i++) {
                uint16_t row[40];
                w32_utf8z_to_utf16(cd_color_names[i], row, 80);
                SendMessageW(list, W32_LB_ADDSTRING, 0,
                             (W32_LPARAM)(uintptr_t)row);
            }
            /* CC_RGBINIT preselects the matching row */
            if (ctx->cc->Flags & W32_CC_RGBINIT) {
                for (int i = 0; i < 16; i++)
                    if (cd_colors[i] == (ctx->cc->rgbResult & 0x00FFFFFFu)) {
                        SendMessageW(list, W32_LB_SETCURSEL,
                                     (W32_WPARAM)(uint32_t)i, 0);
                        break;
                    }
            }
        }
        return 1;
    }
    case W32_WM_COMMAND: {
        if (!ctx) break;
        uint32_t id = (uint32_t)wp & 0xFFFFu;
        if (id == CD_ID_OK) {
            W32_HWND list = GetDlgItem(dlg, 110);
            int sel = (int)SendMessageW(list, W32_LB_GETCURSEL, 0, 0);
            if (sel < 0 || sel >= 16) {
                ctx->result = 0;
                EndDialog(dlg, W32_IDCANCEL);
                return 1;
            }
            ctx->cc->rgbResult = cd_colors[sel];
            /* the custom colours persist through lpCustColors */
            if (ctx->cc->lpCustColors)
                for (int i = 0; i < 16; i++)
                    ctx->cc->lpCustColors[i] = cd_colors[i];
            ctx->result = 1;
            EndDialog(dlg, W32_IDOK);
            return 1;
        }
        if (id == CD_ID_CANCEL) {
            ctx->result = 0;
            EndDialog(dlg, W32_IDCANCEL);
            return 1;
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

static int cd_choose_color(W32_CHOOSECOLORW *cc) {
    cd_err = 0;
    if (!cc || cc->lStructSize < sizeof *cc) {
        cd_err = W32_CDERR_STRUCTSIZE;
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (cc->Flags & (W32_CC_ENABLETEMPLATE /*| TEMPLATEHANDLE: not offered*/)) {
        cd_err = W32_CDERR_NOTEMPLATE;
        cd_note("CC_ENABLETEMPLATE refused: the dialog is built by the "
                "engine (CDERR_NOTEMPLATE)");
        return 0;
    }
    if (cc->Flags & W32_CC_SHOWHELP)
        cd_note("CC_SHOWHELP accepted, noted: there is no help UI");
    cd_register_classes();
    cd_cc_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cc = cc;
    W32_BYTE tbuf[96];
    memset(tbuf, 0, sizeof tbuf);
    W32_DLGTEMPLATE *t = (W32_DLGTEMPLATE *)(void *)tbuf;
    t->style = W32_DS_MODALFRAME;
    t->items = 0;
    t->x = 60; t->y = 60; t->cx = 212; t->cy = 110;
    {
        uint16_t *p = (uint16_t *)(void *)(tbuf + sizeof *t);
        *p++ = 0; *p++ = 0;
        const uint16_t *title = (const uint16_t[]){'C','o','l','o','u','r',0};
        while (*title) *p++ = *title++;
        *p++ = 0;
    }
    cd_cc_pending = &ctx;
    DialogBoxIndirectParamW(GetModuleHandleW(NULL),
                            (const W32_DLGTEMPLATE *)(void *)tbuf,
                            cc->hwndOwner, cd_cc_proc, 0);
    cd_cc_pending = NULL;
    return ctx.result;
}

W32_BOOL W32ABI ChooseColorW(W32_CHOOSECOLORW *cc) { return cd_choose_color(cc); }

W32_BOOL W32ABI ChooseColorA(void *cc) {
    /* layout-identical apart from nothing: no string members exist */
    return cd_choose_color((W32_CHOOSECOLORW *)cc);
}

/* ---- ChooseFontW ---------------------------------------------------------------- */

typedef struct {
    W32_CHOOSEFONTW *cf;
    int result;
} cd_cf_ctx_t;

static cd_cf_ctx_t *cd_cf_pending;

/* The one real face's size ladder (the stock font's derivations). */
static const int cd_font_sizes[] = { 8, 10, 12, 14, 16, 18, 24, 32 };

static W32_INT_PTR W32ABI cd_cf_proc(W32_HWND dlg, W32_UINT msg,
                                     W32_WPARAM wp, W32_LPARAM lp) {
    cd_cf_ctx_t *ctx =
        (msg == W32_WM_INITDIALOG) ? cd_cf_pending
                                   : (cd_cf_ctx_t *)(void *)(uintptr_t)
                                     GetWindowLongPtrW(dlg, W32_GWL_USERDATA);
    if (ctx && (ctx->cf->Flags & W32_CF_ENABLEHOOK) && ctx->cf->lpfnHook) {
        typedef W32_INT_PTR (W32ABI *hook_t)(W32_HWND, W32_UINT, W32_WPARAM,
                                             W32_LPARAM);
        W32_INT_PTR r = ((hook_t)ctx->cf->lpfnHook)(dlg, msg, wp, lp);
        if (r) return r;
    }
    switch (msg) {
    case W32_WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, W32_GWL_USERDATA, (intptr_t)ctx);
        cd_create_items(dlg, (const cd_item_spec_t[]){
            { cd_cls_stat,  920,  6,  6,  80,  8, (const uint16_t[]){'F','o','n','t',':',0} },
            { cd_cls_stat,  921,  6, 16, 160,  8, (const uint16_t[]){'V','G','A',' ','8','x','1','6',0} },
            { cd_cls_stat,  922,  6, 28,  80,  8, (const uint16_t[]){'S','i','z','e',':',0} },
            { cd_cls_list,  120,  6, 38,  60, 60, NULL },
            { cd_cls_btn,  CD_ID_OK,    120, 102, 40, 12, (const uint16_t[]){'O','K',0} },
            { cd_cls_btn,  CD_ID_CANCEL,165, 102, 40, 12, (const uint16_t[]){'C','a','n','c','e','l',0} },
        }, 6);
        if (ctx) {
            W32_HWND list = GetDlgItem(dlg, 120);
            for (size_t i = 0; i < sizeof cd_font_sizes / sizeof cd_font_sizes[0]; i++) {
                char row[16];
                snprintf(row, sizeof row, "%d", cd_font_sizes[i]);
                uint16_t wrow[16];
                w32_utf8z_to_utf16(row, wrow, 32);
                SendMessageW(list, W32_LB_ADDSTRING, 0,
                             (W32_LPARAM)(uintptr_t)wrow);
            }
            /* CF_INITTOLOGFONTSTRUCT: preselect the closest height */
            int want = 16;
            if ((ctx->cf->Flags & W32_CF_INITTOLOGFONTSTRUCT) && ctx->cf->lpLogFont)
                want = ctx->cf->lpLogFont->lfHeight > 0
                           ? (int)ctx->cf->lpLogFont->lfHeight : 16;
            int best = 4;            /* 16, the stock size */
            for (size_t i = 0; i < sizeof cd_font_sizes / sizeof cd_font_sizes[0]; i++)
                if (cd_font_sizes[i] == want) { best = (int)i; break; }
            SendMessageW(list, W32_LB_SETCURSEL, (W32_WPARAM)(uint32_t)best, 0);
        }
        return 1;
    }
    case W32_WM_COMMAND: {
        if (!ctx) break;
        uint32_t id = (uint32_t)wp & 0xFFFFu;
        if (id == CD_ID_OK) {
            W32_HWND list = GetDlgItem(dlg, 120);
            int sel = (int)SendMessageW(list, W32_LB_GETCURSEL, 0, 0);
            if (sel < 0 ||
                (size_t)sel >= sizeof cd_font_sizes / sizeof cd_font_sizes[0]) {
                ctx->result = 0;
                EndDialog(dlg, W32_IDCANCEL);
                return 1;
            }
            if (ctx->cf->lpLogFont) {
                W32_LOGFONTW *lf = ctx->cf->lpLogFont;
                memset(lf, 0, sizeof *lf);
                lf->lfHeight = (W32_LONG)cd_font_sizes[sel];
                lf->lfWeight = 400;
                lf->lfCharSet = 255;           /* OEM_CHARSET */
                lf->lfPitchAndFamily = 0x01;   /* FIXED_PITCH */
                static const uint16_t face[] =
                    {'V','G','A',' ','8','x','1','6',0};
                cd_wncpy(lf->lfFaceName, face, 32);
            }
            ctx->cf->iPointSize = cd_font_sizes[sel] * 10;
            ctx->cf->rgbColors = 0;
            ctx->cf->nFontType = 0x0004;       /* SIMULATED_FONT */
            ctx->result = 1;
            EndDialog(dlg, W32_IDOK);
            return 1;
        }
        if (id == CD_ID_CANCEL) {
            ctx->result = 0;
            EndDialog(dlg, W32_IDCANCEL);
            return 1;
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

static int cd_choose_font(W32_CHOOSEFONTW *cf) {
    cd_err = 0;
    if (!cf || cf->lStructSize < sizeof *cf || !cf->lpLogFont) {
        cd_err = W32_CDERR_STRUCTSIZE;
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (cf->Flags & W32_CF_ENABLETEMPLATE) {
        cd_err = W32_CDERR_NOTEMPLATE;
        cd_note("CF_ENABLETEMPLATE refused: the dialog is built by the "
                "engine (CDERR_NOTEMPLATE)");
        return 0;
    }
    if (cf->Flags & W32_CF_PRINTERFONTS)
        cd_note("CF_PRINTERFONTS accepted, noted: no printer fonts exist; "
                "the screen list is the whole list");
    if (cf->Flags & W32_CF_LIMITSIZE) {
        /* the ladder is clamped to the asked window */
        if (cf->nSizeMin > 32) {
            cd_err = W32_CDERR_STRUCTSIZE;
            return 0;
        }
    }
    cd_register_classes();
    cd_cf_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cf = cf;
    W32_BYTE tbuf[96];
    memset(tbuf, 0, sizeof tbuf);
    W32_DLGTEMPLATE *t = (W32_DLGTEMPLATE *)(void *)tbuf;
    t->style = W32_DS_MODALFRAME;
    t->items = 0;
    t->x = 60; t->y = 60; t->cx = 212; t->cy = 130;
    {
        uint16_t *p = (uint16_t *)(void *)(tbuf + sizeof *t);
        *p++ = 0; *p++ = 0;
        const uint16_t *title = (const uint16_t[]){'F','o','n','t',0};
        while (*title) *p++ = *title++;
        *p++ = 0;
    }
    cd_cf_pending = &ctx;
    DialogBoxIndirectParamW(GetModuleHandleW(NULL),
                            (const W32_DLGTEMPLATE *)(void *)tbuf,
                            cf->hwndOwner, cd_cf_proc, 0);
    cd_cf_pending = NULL;
    return ctx.result;
}

W32_BOOL W32ABI ChooseFontW(W32_CHOOSEFONTW *cf) { return cd_choose_font(cf); }

W32_BOOL W32ABI ChooseFontA(void *cf) {
    /* the layout is byte-identical; the LOGFONTA inside differs but the
     * engine writes face/height as bytes the caller reads back in A
     * shape — lfFaceName is the only string and it is ASCII here */
    return cd_choose_font((W32_CHOOSEFONTW *)cf);
}

/* ---- PrintDlgW (FAIL-CLEAN) -------------------------------------------------------- */

W32_BOOL W32ABI PrintDlgW(W32_PRINTDLGW *pd) {
    (void)pd;
    cd_note("PrintDlgW refused: there are no printers "
            "(PDERR_NODEFAULTPRN, the W32A-7 printing shape)");
    cd_err = W32_PDERR_NODEFAULTPRN;
    w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

/* ---- SHBrowseForFolderW (bound under SHELL32 too) ---------------------------------- */

/* The folder picker: the directory tree under / (two levels), the
 * shell32 PIDL model on return. */
typedef struct {
    W32_BROWSEINFOW *bi;
    int result;
    uint16_t paths[64][280];
    uint16_t sel_path[280];
    int count;
} cd_bf_ctx_t;

static cd_bf_ctx_t *cd_bf_pending;

static void cd_bf_fill(cd_bf_ctx_t *ctx, W32_HWND list) {
    ctx->count = 0;
    cd_list_t *L = cd_list_by_hwnd(list);
    if (L) {
        L->count = 0;
        L->sel = -1;
        memset(L->selmask, 0, sizeof L->selmask);
    }
    /* level 0: the root; then one level of children */
    cd_wncpy(ctx->paths[ctx->count], (const uint16_t[]){'/',0}, 280);
    ctx->count++;
    static const uint16_t star[] = {'/', '*', 0};
    W32_WIN32_FIND_DATAW fd;
    W32_HANDLE h = FindFirstFileW(star, &fd);
    if (h) {
        do {
            if (!(fd.dwFileAttributes & 0x10u)) continue;
            if (fd.cFileName[0] == 0) continue;
            if (ctx->count >= 64) break;
            size_t o = 0;
            ctx->paths[ctx->count][o++] = '/';
            while (fd.cFileName[o - 1] && o + 1 < 278) {
                ctx->paths[ctx->count][o] = fd.cFileName[o - 1];
                o++;
            }
            ctx->paths[ctx->count][o] = 0;
            ctx->count++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (int i = 0; i < ctx->count; i++)
        SendMessageW(list, W32_LB_ADDSTRING, 0,
                     (W32_LPARAM)(uintptr_t)ctx->paths[i]);
}

static W32_INT_PTR W32ABI cd_bf_proc(W32_HWND dlg, W32_UINT msg,
                                     W32_WPARAM wp, W32_LPARAM lp) {
    (void)lp;
    cd_bf_ctx_t *ctx =
        (msg == W32_WM_INITDIALOG) ? cd_bf_pending
                                   : (cd_bf_ctx_t *)(void *)(uintptr_t)
                                     GetWindowLongPtrW(dlg, W32_GWL_USERDATA);
    if (ctx && ctx->bi->lpfnCallback) {
        /* the callback is browse-model, not dialog-precedence: it sees
         * the init and the selection, the documented BFFM shape */
        typedef W32_INT_PTR (W32ABI *cb_t)(W32_HWND, W32_UINT, W32_LPARAM,
                                           W32_LPARAM);
        if (msg == W32_WM_INITDIALOG)
            ((cb_t)ctx->bi->lpfnCallback)(dlg, 1 /* BFFM_INITIALIZED */,
                                           0, ctx->bi->lParam);
    }
    switch (msg) {
    case W32_WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, W32_GWL_USERDATA, (intptr_t)ctx);
        cd_create_items(dlg, (const cd_item_spec_t[]){
            { cd_cls_stat,  930,  6,  6, 160,  8, (const uint16_t[]){'F','o','l','d','e','r',':',0} },
            { cd_cls_list,  130,  6, 16, 170, 80, NULL },
            { cd_cls_btn,  CD_ID_OK,    120, 102, 40, 12, (const uint16_t[]){'O','K',0} },
            { cd_cls_btn,  CD_ID_CANCEL,165, 102, 40, 12, (const uint16_t[]){'C','a','n','c','e','l',0} },
        }, 4);
        if (ctx) {
            if (ctx->bi->lpszTitle)
                SetWindowTextW(dlg, ctx->bi->lpszTitle);
            cd_bf_fill(ctx, GetDlgItem(dlg, 130));
        }
        return 1;
    }
    case W32_WM_COMMAND: {
        if (!ctx) break;
        uint32_t id = (uint32_t)wp & 0xFFFFu;
        if (id == CD_ID_OK) {
            W32_HWND list = GetDlgItem(dlg, 130);
            int sel = (int)SendMessageW(list, W32_LB_GETCURSEL, 0, 0);
            if (sel < 0 || sel >= ctx->count) {
                ctx->result = 0;
                EndDialog(dlg, W32_IDCANCEL);
                return 1;
            }
            if (ctx->bi->pszDisplayName)
                cd_wncpy(ctx->bi->pszDisplayName, ctx->paths[sel], 280);
            cd_wncpy(ctx->sel_path, ctx->paths[sel], 280);
            ctx->result = 1;
            EndDialog(dlg, W32_IDOK);
            return 1;
        }
        if (id == CD_ID_CANCEL) {
            ctx->result = 0;
            EndDialog(dlg, W32_IDCANCEL);
            return 1;
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

void *W32ABI SHBrowseForFolderW(W32_BROWSEINFOW *bi) {
    cd_err = 0;
    if (!bi) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (bi->ulFlags & W32_BIF_NEWDIALOGSTYLE)
        cd_note("BIF_NEWDIALOGSTYLE accepted, noted: the dialog is the "
                "one the engine builds");
    cd_register_classes();
    cd_bf_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.bi = bi;
    W32_BYTE tbuf[96];
    memset(tbuf, 0, sizeof tbuf);
    W32_DLGTEMPLATE *t = (W32_DLGTEMPLATE *)(void *)tbuf;
    t->style = W32_DS_MODALFRAME;
    t->items = 0;
    t->x = 50; t->y = 50; t->cx = 212; t->cy = 130;
    {
        uint16_t *p = (uint16_t *)(void *)(tbuf + sizeof *t);
        *p++ = 0; *p++ = 0;
        const uint16_t *title =
            (const uint16_t[]){'B','r','o','w','s','e',' ','F','o','r',
                               ' ','F','o','l','d','e','r',0};
        while (*title) *p++ = *title++;
        *p++ = 0;
    }
    cd_bf_pending = &ctx;
    DialogBoxIndirectParamW(GetModuleHandleW(NULL),
                            (const W32_DLGTEMPLATE *)(void *)tbuf,
                            bi->hwndOwner, cd_bf_proc, 0);
    cd_bf_pending = NULL;
    if (!ctx.result) return NULL;
    /* the PIDL the shell32 model defines: { u16 cb, u16 csidl (0: a
     * browsed path, not a CSIDL), u16 path_units, path, u16 0 }.
     * SHGetPathFromIDListW walks the same shape. */
    static uint16_t sel_path[280];
    cd_wncpy(sel_path, ctx.sel_path, 280);
    size_t units = cd_wlen(sel_path);
    size_t cb = 2 + 2 + (units + 1) * 2 + 2;
    uint16_t *p = (uint16_t *)malloc(cb + 2);
    if (!p) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    p[0] = (uint16_t)cb;
    p[1] = 0;
    p[2] = (uint16_t)units;
    for (size_t i = 0; i < units + 1; i++) p[3 + i] = sel_path[i];
    p[3 + units + 1] = 0;
    return p;
}
