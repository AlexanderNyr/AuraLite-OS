/* W32A-11 sanitized OLE drag callbacks, hit-test, revocation and medium
 * ownership. The fixture calls genuine W32ABI vtables (no PE/OS needed).
 * SPDX-License-Identifier: Apache-2.0 */
#define AURALITE_W32_HOST_TEST 1
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "w32/ole32.h"
#include "w32/kernel32.h"
#include "w32/gdi32.h"
#include "w32/w32_errno.h"
static int n, f;
#define CHECK(x) do { ++n; if (!(x)) { ++f; fprintf(stderr,"FAIL:%d: %s\n",__LINE__,#x); } } while(0)
static W32_DWORD err;
void w32_set_last_error(W32_DWORD e) { err = e; }
static int ready = 1;
int w32_com_ole_ready(void) { return ready; }
static const W32_HWND left = (W32_HWND)(uintptr_t)0x100;
static const W32_HWND right = (W32_HWND)(uintptr_t)0x200;
static const W32_HWND child = (W32_HWND)(uintptr_t)0x201;
W32ABI W32_BOOL IsWindow(W32_HWND h) { return h == left || h == right || h == child; }
W32ABI W32_HWND WindowFromPoint(W32_POINT p) {
    return p.x < 40 ? left : p.x < 80 ? child : NULL;
}
W32ABI W32_HWND GetParent(W32_HWND h) { return h == child ? right : NULL; }
W32ABI W32_BOOL ClientToScreen(W32_HWND h, W32_POINT *p) { (void)p; return IsWindow(h); }
W32ABI W32_BOOL GetCursorPos(W32_POINT *p) { p->x=15; p->y=15; return 1; }
static W32_MSG pending[24];
static int head, end, quit_posted, dispatches;
static void queue(unsigned msg, int x, int y, W32_DWORD keys) {
    W32_MSG *m=&pending[end++];
    memset(m,0,sizeof *m);
    m->message=msg; m->hwnd=left; m->wParam=keys;
    m->lParam=(W32_LPARAM)(((uint32_t)(uint16_t)y << 16) | (uint16_t)x);
}
W32ABI W32_BOOL GetMessageW(W32_MSG *m, W32_HWND hwnd, W32_UINT lo, W32_UINT hi) {
    (void)hwnd; (void)lo; (void)hi;
    if (head == end) { m->wParam=0; return 0; }
    *m=pending[head++]; return 1;
}
W32ABI void PostQuitMessage(int code) { quit_posted = code + 1; }
W32ABI W32_LRESULT DispatchMessageW(const W32_MSG *m) { (void)m; ++dispatches; return 0; }
static int globals, deleted, files, file_fail, tasks;
W32ABI void *GlobalFree(void *h) { free(h); ++globals; return NULL; }
W32ABI W32_BOOL DeleteObject(void *h) { ++deleted; return h != NULL; }
W32ABI W32_BOOL DeleteFileW(const uint16_t *name) {
    CHECK(name[0]=='/' && name[1]=='t');
    if (file_fail) { w32_set_last_error(W32_ERROR_ACCESS_DENIED); return 0; }
    ++files; return 1;
}
W32ABI void CoTaskMemFree(void *p) { ++tasks; free(p); }
#include "../../w32/src/w32_ole_drag.c"

typedef struct {
    W32_IDropTarget obj;
    int refs, added, released, enters, overs, leaves, drops, revoke_enter;
    W32_DWORD propose;
    W32_HWND hwnd;
    void *got_data;
} drop_target;
static W32_DWORD W32ABI target_query(W32_IDropTarget *s, const W32_GUID *id, void **out) {
    (void)s; (void)id; (void)out; return W32_COM_E_INVALIDARG;
}
static W32_DWORD W32ABI target_add(W32_IDropTarget *s) {
    drop_target *t=(drop_target *)s; ++t->added; return (W32_DWORD)++t->refs;
}
static W32_DWORD W32ABI target_release(W32_IDropTarget *s) {
    drop_target *t=(drop_target *)s; ++t->released; return (W32_DWORD)--t->refs;
}
static W32_DWORD W32ABI enter(W32_IDropTarget *s, void *data, W32_DWORD keys,
                              W32_POINTL p, W32_DWORD *effect) {
    drop_target *t=(drop_target *)s; ++t->enters; t->got_data=data;
    CHECK(p.x >= 0 && keys <= 1); /* button-up can enter a new HWND */
    *effect=t->propose;
    if (t->revoke_enter) CHECK(RevokeDragDrop(t->hwnd)==W32_COM_S_OK);
    return W32_COM_S_OK;
}
static W32_DWORD W32ABI over(W32_IDropTarget *s, W32_DWORD keys,
                             W32_POINTL p, W32_DWORD *effect) {
    drop_target *t=(drop_target *)s; ++t->overs;
    CHECK(p.x >= 0 && keys == 1);
    *effect=t->propose; return W32_COM_S_OK;
}
static W32_DWORD W32ABI leave(W32_IDropTarget *s) {
    drop_target *t=(drop_target *)s; ++t->leaves; return W32_COM_S_OK;
}
static W32_DWORD W32ABI drop(W32_IDropTarget *s, void *data, W32_DWORD keys,
                             W32_POINTL p, W32_DWORD *effect) {
    drop_target *t=(drop_target *)s; ++t->drops;
    CHECK(data==t->got_data && keys == 0 && p.y == 15);
    *effect=t->propose; return W32_COM_S_OK;
}
static const W32_IDropTargetVtbl tv={target_query,target_add,target_release,enter,over,leave,drop};
typedef struct { W32_IDropSource obj; int refs, added, released, queries, feedback; W32_DWORD last; } drag_source;
static W32_DWORD W32ABI src_query(W32_IDropSource *s,const W32_GUID *id,void **out) {
    (void)s;(void)id;(void)out;return W32_COM_E_INVALIDARG;
}
static W32_DWORD W32ABI src_add(W32_IDropSource *s) {
    drag_source *a=(drag_source *)s; ++a->added; return (W32_DWORD)++a->refs;
}
static W32_DWORD W32ABI src_rel(W32_IDropSource *s) {
    drag_source *a=(drag_source *)s; ++a->released; return (W32_DWORD)--a->refs;
}
static W32_DWORD W32ABI cont(W32_IDropSource *s,W32_BOOL esc,W32_DWORD keys) {
    drag_source *a=(drag_source *)s; ++a->queries;
    return esc ? W32_DRAGDROP_S_CANCEL : keys&1 ? W32_COM_S_OK : W32_DRAGDROP_S_DROP;
}
static W32_DWORD W32ABI feedback(W32_IDropSource *s,W32_DWORD effect) {
    drag_source *a=(drag_source *)s; ++a->feedback; a->last=effect;
    return W32_DRAGDROP_S_USEDEFAULTCURSORS;
}
static const W32_IDropSourceVtbl sv={src_query,src_add,src_rel,cont,feedback};
static void reset_queue(void) { head=0; end=0; quit_posted=0; dispatches=0; }
int main(void) {
    drop_target a={.obj={&tv},.refs=1,.propose=W32_DROPEFFECT_COPY,.hwnd=left};
    drop_target b={.obj={&tv},.refs=1,.propose=W32_DROPEFFECT_COPY,.hwnd=right};
    drag_source src={.obj={&sv},.refs=1};
    W32_DWORD effect=99;
    CHECK(RegisterDragDrop(NULL,&a.obj)==W32_DRAGDROP_E_INVALIDHWND);
    CHECK(RegisterDragDrop(left,NULL)==W32_COM_E_INVALIDARG);
    ready=0;
    CHECK(RegisterDragDrop(left,&a.obj)==W32_COM_E_NOTINITIALIZED);
    CHECK(DoDragDrop(&a,&src.obj,1,&effect)==W32_COM_E_NOTINITIALIZED && effect==0);
    ready=1;
    CHECK(RegisterDragDrop(left,&a.obj)==W32_COM_S_OK && a.refs==2);
    CHECK(RegisterDragDrop(left,&a.obj)==W32_DRAGDROP_E_ALREADYREGISTERED && a.refs==2);
    CHECK(RegisterDragDrop(right,&b.obj)==W32_COM_S_OK && b.refs==2);
    CHECK(RevokeDragDrop(child)==W32_DRAGDROP_E_NOTREGISTERED);
    reset_queue();
    queue(W32_WM_MOUSEMOVE,15,15,1);
    queue(W32_WM_MOUSEMOVE,16,15,1);
    queue(W32_WM_MOUSEMOVE,60,15,1); /* child hit -> registered parent */
    queue(W32_WM_MOUSEMOVE,61,15,1);
    queue(W32_WM_LBUTTONUP,61,15,0);
    CHECK(DoDragDrop(&a,&src.obj,W32_DROPEFFECT_COPY,&effect)==W32_DRAGDROP_S_DROP);
    CHECK(effect==W32_DROPEFFECT_COPY);
    CHECK(a.enters==1 && a.overs==1 && a.leaves==1 && a.drops==0);
    CHECK(b.enters==1 && b.overs==1 && b.drops==1 && b.leaves==0);
    CHECK(a.got_data==&a && b.got_data==&a);
    CHECK(src.queries==5 && src.feedback==4 && src.refs==1);
    CHECK(RevokeDragDrop(left)==W32_COM_S_OK && a.refs==1);
    CHECK(RevokeDragDrop(right)==W32_COM_S_OK && b.refs==1);

    /* Button-up directly over a NEW window, without a mouse-move at that
     * coordinate: leave the old target, enter and drop on the actual HWND. */
    CHECK(RegisterDragDrop(left,&a.obj)==W32_COM_S_OK);
    CHECK(RegisterDragDrop(right,&b.obj)==W32_COM_S_OK);
    reset_queue(); queue(W32_WM_MOUSEMOVE,15,15,1);
    queue(W32_WM_LBUTTONUP,60,15,0);
    CHECK(DoDragDrop(&a,&src.obj,1,&effect)==W32_DRAGDROP_S_DROP);
    CHECK(effect==1 && a.leaves==2 && b.enters==2 && b.drops==2);
    CHECK(RevokeDragDrop(left)==W32_COM_S_OK && a.refs==1);
    CHECK(RevokeDragDrop(right)==W32_COM_S_OK && b.refs==1);
    /* A release over empty space must NOT drop on the previous HWND. */
    CHECK(RegisterDragDrop(left,&a.obj)==W32_COM_S_OK);
    reset_queue(); queue(W32_WM_MOUSEMOVE,15,15,1);
    queue(W32_WM_LBUTTONUP,95,15,0);
    CHECK(DoDragDrop(&a,&src.obj,1,&effect)==W32_DRAGDROP_S_CANCEL);
    CHECK(effect==0 && a.drops==0 && a.leaves==3);
    CHECK(RevokeDragDrop(left)==W32_COM_S_OK && a.refs==1);

    /* Target revokes itself from DragEnter. Its ref survives until DragLeave.
     * Drop after revocation must not invoke Drop on a destroyed target. */
    a.revoke_enter=1;
    CHECK(RegisterDragDrop(left,&a.obj)==W32_COM_S_OK);
    reset_queue(); queue(W32_WM_MOUSEMOVE,12,15,1); queue(W32_WM_LBUTTONUP,12,15,0);
    effect=8;
    CHECK(DoDragDrop(&a,&src.obj,1,&effect)==W32_DRAGDROP_S_CANCEL);
    CHECK(effect==0 && a.drops==0 && a.refs==1);
    CHECK(RevokeDragDrop(left)==W32_DRAGDROP_E_NOTREGISTERED);
    a.revoke_enter=0;
    CHECK(RegisterDragDrop(left,&a.obj)==W32_COM_S_OK);
    reset_queue(); queue(W32_WM_MOUSEMOVE,10,15,1);
    queue(W32_WM_KEYDOWN,10,15,0);
    pending[end-1].wParam=W32_VK_ESCAPE;
    CHECK(DoDragDrop(&a,&src.obj,1,&effect)==W32_DRAGDROP_S_CANCEL);
    CHECK(effect==0 && a.leaves>=2);
    CHECK(RevokeDragDrop(left)==W32_COM_S_OK);

    CHECK(RegisterDragDrop(left,&a.obj)==W32_COM_S_OK);
    w32_ole_drag_window_destroyed(left);
    CHECK(a.refs==1 && RevokeDragDrop(left)==W32_DRAGDROP_E_NOTREGISTERED);
    reset_queue(); /* QUIT is reposted; never consumed silently */
    queue(W32_WM_MOUSEMOVE,18,15,1);
    CHECK(DoDragDrop(&a,&src.obj,1,&effect)==W32_DRAGDROP_S_CANCEL);
    CHECK(quit_posted==1 && effect==0);
    CHECK(DoDragDrop(&a,&src.obj,1,NULL)==W32_COM_E_INVALIDARG);
    CHECK(DoDragDrop(&a,&src.obj,0,&effect)==W32_COM_E_INVALIDARG);

    W32_STGMEDIUM m={.tymed=W32_TYMED_HGLOBAL,.medium=malloc(12)};
    ReleaseStgMedium(&m);
    CHECK(globals==1 && m.tymed==W32_TYMED_NULL && !m.medium);
    m=(W32_STGMEDIUM){.tymed=W32_TYMED_GDI,.medium=(void *)1};
    ReleaseStgMedium(&m);
    CHECK(deleted==1 && m.tymed==W32_TYMED_NULL);
    m=(W32_STGMEDIUM){.tymed=W32_TYMED_ISTREAM,.medium=&a.obj};
    ReleaseStgMedium(&m);
    CHECK(a.refs==0 && a.released>=1 && m.tymed==W32_TYMED_NULL);
    a.refs=1;
    m=(W32_STGMEDIUM){.tymed=W32_TYMED_HGLOBAL,.medium=(void *)0x1,.pUnkForRelease=&a.obj};
    ReleaseStgMedium(&m);
    CHECK(a.refs==0 && globals==1 && m.tymed==W32_TYMED_NULL);
    static const uint16_t path[]={'/','t','m','p','/','f',0};
    uint16_t *owned=malloc(sizeof path); memcpy(owned,path,sizeof path);
    m=(W32_STGMEDIUM){.tymed=W32_TYMED_FILE,.medium=owned};
    file_fail=1; err=0;
    ReleaseStgMedium(&m);
    CHECK(err==W32_ERROR_ACCESS_DENIED && m.medium==owned && !tasks);
    file_fail=0;
    ReleaseStgMedium(&m);
    CHECK(files==1 && tasks==1 && m.tymed==W32_TYMED_NULL);
    m=(W32_STGMEDIUM){.tymed=W32_TYMED_MFPICT,.medium=(void *)0x1};
    err=0; ReleaseStgMedium(&m);
    CHECK(err==W32_ERROR_NOT_SUPPORTED && m.medium==(void *)0x1);
    ReleaseStgMedium(NULL);
    fprintf(stderr,"w32a11-drag: %d checks, %d failures\n",n,f);
    return f!=0;
}
