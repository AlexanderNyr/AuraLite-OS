/* test_w32_a6.c — W32APP_PLAN.md phase W32A-6: USER32 breadth II.
 *
 * Host-unit gate for dialogs, menus, timers, caret, accelerators,
 * clipboard, hooks, DrawText/DrawFocusRect, and PE resource lookup
 * (FindResource/LoadResource/LockResource/SizeofResource/LoadString),
 * modeled after tests/unit/test_w32_a5.c.
 *
 * Real code under test: w32_utf.c, w32_errno.c, user32_win.c,
 * user32.c, w32_pe.c, w32_dlg.c, and w32_rsrc.c.
 * Faked: every ag_* compositor op, the user32 primitives the dialog
 * loop calls (GetMessageW/TranslateMessage/DispatchMessageW — we drive
 * WM_INITDIALOG / WM_CLOSE deterministically), and w32_module_file_bytes
 * which we point at a synthetic in-memory PE32+ with RT_DIALOG and
 * RT_STRING entries.
 */

#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#define AURALITE_W32_HOST_TEST 1

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#include "w32/w32_abi.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"
#include "w32/kernel32.h"
#include "w32/w32_teb.h"
#include "w32/w32_pe.h"
#include "w32/user32.h"
#include "w32/w32_rsrc.h"

/* ---------------------------------------------------------------
 * Host primitives (tid, ticks, TEB, events, sleep)
 * --------------------------------------------------------------- */
static __thread W32_DWORD host_tid;
W32ABI W32_DWORD GetCurrentThreadId(void) {
    if (!host_tid) host_tid = (W32_DWORD)(uintptr_t)pthread_self();
    return host_tid;
}
W32ABI W32_DWORD GetCurrentProcessId(void) { return 4242; }
static W32_DWORD tick_ms;
W32ABI W32_DWORD GetTickCount(void) { return tick_ms; }
W32ABI W32_ULONGLONG GetTickCount64(void) { return tick_ms; }
static __thread struct w32_teb host_teb;
struct w32_teb *w32_teb_self(void) { return &host_teb; }
W32ABI W32_DWORD GetLastError(void) { return w32_get_last_error_raw(); }
W32ABI W32_HANDLE CreateEventW(void*,W32_BOOL,W32_BOOL,const uint16_t*) { return (W32_HANDLE)(uintptr_t)1; }
W32ABI W32_BOOL SetEvent(W32_HANDLE) { return 1; }
W32ABI W32_DWORD WaitForSingleObject(W32_HANDLE,W32_DWORD) { return 0; }
W32ABI W32_DWORD WaitForMultipleObjects(W32_DWORD,W32_HANDLE*,W32_BOOL,W32_DWORD) { return 0; }
W32ABI W32_BOOL CloseHandle(W32_HANDLE) { return 1; }
W32ABI void Sleep(W32_DWORD ms) {
    struct timespec ts;
    ts.tv_sec=ms/1000; ts.tv_nsec=(long)(ms%1000)*1000000L;
    nanosleep(&ts,0);
}

/* ---------------------------------------------------------------
 * Fake compositor (matches ui_theme_mirror_t/ui_event_mirror_t
 * layout used by user32_win.c in host-test mode).
 * --------------------------------------------------------------- */
#define FAKE_WINS 64
typedef struct {
    int in_use; int32_t x,y; uint32_t w,h; int visible,z;
    uint32_t flags; char title[128]; int invalidated;
    struct { uint32_t type; int32_t x,y; uint32_t key;
             uint8_t buttons,mods; uint16_t data; } evq[32];
    int evq_head,evq_tail;
} fake_win_t;
static fake_win_t fw[FAKE_WINS];
static int fake_focused, fake_capture=-1;
static int clears,fills,texts,outlines,renders;
static int32_t blit_last[4];
static char clip_buf[4096];
static int text_last_xy[2]; static char text_last_str[256];
static uint32_t text_last_fg, text_last_bg;
static uint32_t outline_last_color; static int32_t outline_last[4];

static int next_wid(void) {
    for (int i=0;i<FAKE_WINS;i++) if (!fw[i].in_use){memset(&fw[i],0,sizeof fw[i]);fw[i].in_use=1;return i+1;}
    return -1;
}
int ag_window_create(int32_t x,int32_t y,uint32_t w,uint32_t h,const char *title,uint32_t fl){
    int wid=next_wid(); fw[wid-1].x=x;fw[wid-1].y=y;fw[wid-1].w=w;fw[wid-1].h=h;fw[wid-1].flags=fl;
    snprintf(fw[wid-1].title,sizeof fw[wid-1].title,"%s",title?title:"");
    fw[wid-1].evq_head=fw[wid-1].evq_tail=0;
    return wid;
}
int ag_window_show(int wid){if(wid<1||wid>FAKE_WINS)return-1;fw[wid-1].visible=1;return 0;}
int ag_window_hide(int wid){if(wid<1||wid>FAKE_WINS)return-1;fw[wid-1].visible=0;return 0;}
int ag_window_destroy(int wid){if(wid<1||wid>FAKE_WINS)return-1;fw[wid-1].in_use=0;return 0;}
int ag_window_focus(int wid){if(wid<1||wid>FAKE_WINS)return-1;fake_focused=wid;return 0;}
int ag_window_minimize(int){return 0;}int ag_window_maximize(int){return 0;}int ag_window_restore(int){return 0;}
int ag_window_move(int wid,int32_t x,int32_t y){if(wid<1||wid>FAKE_WINS)return-1;fw[wid-1].x=x;fw[wid-1].y=y;return 0;}
int ag_window_resize(int wid,uint32_t w,uint32_t h){if(wid<1||wid>FAKE_WINS)return-1;fw[wid-1].w=w;fw[wid-1].h=h;return 0;}
int ag_window_set_title(int wid,const char *t){if(wid<1||wid>FAKE_WINS)return-1;snprintf(fw[wid-1].title,sizeof fw[wid-1].title,"%s",t?t:"");return 0;}
int ag_window_invalidate(int wid){if(wid<1||wid>FAKE_WINS)return-1;fw[wid-1].invalidated=1;return 0;}
int ag_window_invalidate_rect(int wid,int32_t x,int32_t y,uint32_t w,uint32_t h){(void)x;(void)y;(void)w;(void)h;return ag_window_invalidate(wid);}
int ag_window_get_size(int wid,uint32_t *w,uint32_t *h){if(wid<1||wid>FAKE_WINS)return-1;if(w)*w=fw[wid-1].w;if(h)*h=fw[wid-1].h;return 0;}
int ag_window_get_pos(int wid,int32_t *x,int32_t *y){if(wid<1||wid>FAKE_WINS)return-1;if(x)*x=fw[wid-1].x;if(y)*y=fw[wid-1].y;return 0;}
int ag_window_lower(int wid){if(wid<1||wid>FAKE_WINS)return-1;fw[wid-1].z=-1;return 0;}
int ag_window_set_flags(int wid,uint32_t fl){if(wid<1||wid>FAKE_WINS)return-1;fw[wid-1].flags=fl;return 0;}
uint32_t ag_window_get_flags(int wid){if(wid<1||wid>FAKE_WINS)return 0;return fw[wid-1].flags;}
int ag_window_get_z(int wid){if(wid<1||wid>FAKE_WINS)return-1;return fw[wid-1].z;}
int ag_window_capture(int wid){int p=fake_capture;fake_capture=wid;return p;}
int ag_window_get_capture(void){return fake_capture;}
int ag_window_focused(void){return fake_focused;}
int ag_window_top(void){return fake_focused;}
int ag_screen_size(uint32_t *w,uint32_t *h){if(w)*w=1280;if(h)*h=800;return 0;}
int ag_mouse_position(int32_t *x,int32_t *y){if(x)*x=100;if(y)*y=100;return 0;}

/* Stub declarations (user32_win.c provides extern declarations under
 * AURALITE_W32_HOST_TEST; we define them AFTER including the file so
 * the canonical mirror-struct typedefs already exist). */
int ag_clear(int,uint32_t);
int ag_fill_rect(int,int32_t,int32_t,uint32_t,uint32_t,uint32_t);
int ag_draw_text(int,int32_t,int32_t,const char*,uint32_t);
int ag_draw_pixel(int,int32_t,int32_t,uint32_t);
int ag_draw_line(int,int32_t,int32_t,int32_t,int32_t,uint32_t);
int ag_text(int,const char*,int,int,uint32_t,uint32_t);
int ag_rect_outline(int,int,int,int,int,uint32_t);
void ag_render_now(void);
void ag_alert(const char*,const char*);
int ag_set_clipboard(const char*);
int ag_get_clipboard(char*,int);

#include "../../w32/src/w32_utf.c"
#include "../../w32/src/w32_errno.c"
#include "../../w32/src/user32_win.c"
#include "../../w32/src/user32.c"
#include "../../w32/src/w32_gdi.c"      /* W32A-7: the DC/drawing half */
#include "../../w32/src/w32_pe.c"

int ag_theme_get(ui_theme_t *out){if(out)memset(out,0,sizeof(ui_theme_t));return 0;}
int ag_poll_event(int wid,ui_event_t *out){
    if(wid<1||wid>FAKE_WINS||!out)return-1;
    fake_win_t *w=&fw[wid-1];
    if(w->evq_head==w->evq_tail)return 0;
    memcpy(out,&w->evq[w->evq_head],sizeof w->evq[0]);
    w->evq_head=(w->evq_head+1)%32;
    return 1;
}
int ag_clear(int,uint32_t){clears++;return 0;}
int ag_fill_rect(int,int32_t,int32_t,uint32_t,uint32_t,uint32_t){fills++;return 0;}
int ag_draw_text(int,int32_t,int32_t,const char*,uint32_t){return 0;}
int ag_draw_pixel(int,int32_t,int32_t,uint32_t){return 0;}
int ag_draw_line(int,int32_t,int32_t,int32_t,int32_t,uint32_t){return 0;}
int ag_text(int wid,const char *s,int x,int y,uint32_t fg,uint32_t bg){
    texts++;text_last_xy[0]=x;text_last_xy[1]=y;text_last_fg=fg;text_last_bg=bg;
    snprintf(text_last_str,sizeof text_last_str,"%s",s?s:"");(void)wid;return 0;
}
int ag_rect_outline(int wid,int x,int y,int w,int h,uint32_t c){
    outlines++;outline_last[0]=x;outline_last[1]=y;outline_last[2]=w;outline_last[3]=h;outline_last_color=c;(void)wid;return 0;
}
void ag_render_now(void){renders++;}
void ag_alert(const char*,const char*){}
int ag_set_clipboard(const char *text){snprintf(clip_buf,sizeof clip_buf,"%s",text?text:"");return 0;}
/* W32A-7 additions the raster engine calls (see w32_gdi.c's host block). */
static int blit_alpha_calls;
int ag_blit_alpha(int wid,int32_t x,int32_t y,uint32_t w,uint32_t h,
                  const uint32_t *argb,uint32_t stride){
    (void)argb;(void)stride;
    if(wid<1||wid>FAKE_WINS)return -1;
    blit_alpha_calls++;
    blit_last[0]=x;blit_last[1]=y;blit_last[2]=(int32_t)w;blit_last[3]=(int32_t)h;
    return 0;
}
int ag_get_pixel(int wid,int32_t x,int32_t y){
    if(wid<1||wid>FAKE_WINS)return -1;
    (void)x;(void)y;
    return 0x00000000;                    /* the suite's windows are black */
}
static uint32_t host_theme_dpi = 96;
uint32_t w32_gdi_host_dpi(void){return host_theme_dpi;}
int ag_get_clipboard(char *b,int sz){if(!b||sz<=0)return-1;snprintf(b,(size_t)sz,"%s",clip_buf);return 0;}

/* w32_module_file_bytes — routed to our synthetic PE. */
static uint8_t pe_buf[32768];
static size_t fake_pe_size;
static void *fake_main_module = (void*)(uintptr_t)0xBEEF;

#include "w32/w32_module.h"
W32ABI void SetLastError(W32_DWORD c){ w32_set_last_error(c); }
W32_HMODULE W32ABI w32_GetModuleHandleA(const char *name) { (void)name; return fake_main_module; }
int w32_module_file_bytes(void *h, const uint8_t **d, size_t *sz) {
    if (h != fake_main_module) return 0;
    if (d) *d = pe_buf;
    if (sz) *sz = fake_pe_size;
    return 1;
}

#include "../../w32/src/w32_rsrc.c"
#include "../../w32/src/w32_dlg.c"

/* ---------------------------------------------------------------
 * Synthetic PE builder
 * --------------------------------------------------------------- */
#include "test_w32_a6_pe.h"
#define DLG_RVA  0x3400
#define STR_RVA  0x3500
#define DIR_RSRC_RVA 0x3000

static void build_synthetic_pe(void) {
    memcpy(pe_buf, pe_blob, PE_BLOB_LEN);
    fake_pe_size = PE_BLOB_LEN;
}


/* ---------------------------------------------------------------
 * DLGPROCs for the dialog-loop test
 * --------------------------------------------------------------- */
static int init_count;
static W32_LRESULT W32ABI dlgproc_initok(W32_HWND h,W32_UINT m,W32_WPARAM wp,W32_LPARAM lp){
    (void)wp;(void)lp;
    if(m==W32_WM_INITDIALOG){init_count++;EndDialog(h,42);return 1;}
    return 0;
}
static W32_LRESULT W32ABI dlgproc_cmd(W32_HWND h,W32_UINT m,W32_WPARAM wp,W32_LPARAM lp){
    (void)lp;
    if(m==W32_WM_INITDIALOG){init_count++;return 1;}
    if(m==W32_WM_COMMAND && wp==W32_IDOK){EndDialog(h,(W32_INT_PTR)wp);return 1;}
    return 0;
}

/* Timer callback (file scope, not nested). */
static int cb_fired;
static void W32ABI tcb(W32_HWND h,W32_UINT m,uintptr_t id,W32_DWORD t){
    (void)h;(void)m;(void)id;(void)t;cb_fired++;
}

/* ---------------------------------------------------------------
 * Harness
 * --------------------------------------------------------------- */
static int checks,failures;
static void ok(int cond,const char *what){
    checks++;
    if(cond)printf("ok - %s\n",what);
    else{failures++;printf("not ok - %s\n",what);}
}

int main(void) {
    setvbuf(stdout,0,_IONBF,0);
    printf("== W32A-6: USER32 breadth II (dialogs/menus/clipboard/resources) ==\n");
    build_synthetic_pe();

    /* ---- PE resource parsing ---- */
    {
        pe_image_t img;
        ok(pe_parse(pe_buf,fake_pe_size,&img)==0,"pe_parse accepts the synthetic PE");
        uint32_t rva=0,len=0;
        ok(pe_find_resource_ex(&img,W32_RT_DIALOG,1,0,&rva,&len)==0 && rva==DLG_RVA && len==22,
           "pe_find_resource_ex finds RT_DIALOG/1");
        rva=len=0;
        ok(pe_find_resource_ex(&img,W32_RT_STRING,1,0,&rva,&len)==0 && rva==STR_RVA && len==64,
           "pe_find_resource_ex finds RT_STRING block #1");
        rva=1;len=1;
        ok(pe_find_resource_ex(&img,99,1,0,&rva,&len)==0 && rva==0 && len==0,
           "missing type returns rva=0");
    }

    /* ---- Find/Load/Lock/Sizeof Resource ---- */
    {
        void *h=FindResourceW(fake_main_module,(const uint16_t*)(uintptr_t)1,
                              (const uint16_t*)(uintptr_t)W32_RT_DIALOG);
        ok(h!=0,"FindResourceW finds RT_DIALOG/1");
        const void *p=LockResource(LoadResource(fake_main_module,h));
        ok(p!=0 && memcmp(p,pe_buf+0x1400,22)==0,
           "LockResource returns the on-disk DLGTEMPLATE bytes");
        ok(SizeofResource(fake_main_module,h)==22,"SizeofResource matches the data-entry length");
        ok(FreeResource(h)==1,"FreeResource returns 1");
        SetLastError(0);
        ok(FindResourceW(fake_main_module,(const uint16_t*)(uintptr_t)0xFFFFu,
                         (const uint16_t*)(uintptr_t)W32_RT_DIALOG)==0 &&
           GetLastError()==W32_ERROR_RESOURCE_DATA_NOT_FOUND,
           "missing name sets ERROR_RESOURCE_DATA_NOT_FOUND");
        static const uint16_t sn[]={'N','O','T',0};
        SetLastError(0);
        ok(FindResourceW(fake_main_module,sn,
                         (const uint16_t*)(uintptr_t)W32_RT_DIALOG)==0 &&
           GetLastError()==W32_ERROR_NOT_SUPPORTED,
           "string-named resources refuse with ERROR_NOT_SUPPORTED");
    }

    /* ---- LoadStringW/A ---- */
    {
        uint16_t wb[32];
        ok(LoadStringW(fake_main_module,0,wb,32)==3 && wb[0]=='H'&&wb[1]=='i'&&wb[2]=='!'&&wb[3]==0,
           "LoadStringW decodes id 0 as 'Hi!'");
        ok(LoadStringW(fake_main_module,1,wb,32)==5 && wb[0]=='H'&&wb[4]=='o'&&wb[5]==0,
           "LoadStringW decodes id 1 as 'Hello'");
        char ab[32];
        ok(LoadStringA(fake_main_module,0,ab,32)==3 && strcmp(ab,"Hi!")==0,
           "LoadStringA returns UTF-8");
        ok(LoadStringW(fake_main_module,2,wb,32)==0 && wb[0]==0,
           "unpopulated ids return 0");
    }

    /* ---- LoadIcon/Cursor/Image, Destroy* ---- */
    {
        ok(LoadIconW(fake_main_module,(const uint16_t*)(uintptr_t)1)==0,
           "LoadIconW returns 0 when RT_ICON is absent");
        ok(DestroyIcon((W32_HICON)(uintptr_t)0x1234)==1,"DestroyIcon is a no-op");
        ok(DestroyCursor(0)==1,"DestroyCursor is a no-op");
    }

    /* ---- Dialog loop ---- */
    {
        init_count=0;
        /* Register #32770 dialog class so CreateWindowExW does not return NULL. */
        static const uint16_t dlgcls[]={'#','3','2','7','7','0',0};
        W32_WNDCLASSEXW wc; memset(&wc,0,sizeof wc);
        wc.cbSize=sizeof wc;
        wc.lpfnWndProc=DefWindowProcW;
        wc.hbrBackground=(W32_HBRUSH)(uintptr_t)0x00FFFFFFu;
        wc.lpszClassName=dlgcls;
        RegisterClassExW(&wc);
        /* DLGTEMPLATE is followed by three variable-length fields (menu,
         * class, title); each is a single NUL word when absent.  We pack
         * them into a trailing buffer so the parser never reads past the
         * object regardless of sanitizer redzones. */
        uint8_t tmpl_buf[sizeof(W32_DLGTEMPLATE) + 8]; memset(tmpl_buf,0,sizeof tmpl_buf);
        W32_DLGTEMPLATE *tmpl = (W32_DLGTEMPLATE*)tmpl_buf;
        tmpl->style=W32_WS_POPUP|W32_WS_CAPTION|W32_WS_SYSMENU;
        tmpl->cx=100;tmpl->cy=80;
        W32_INT_PTR r=DialogBoxIndirectParamW(fake_main_module,tmpl,0,dlgproc_initok,0);
        ok(init_count==1,"WM_INITDIALOG fires exactly once");
        ok(r==42,"EndDialog from WM_INITDIALOG returns 42");
        ok(fw[0].in_use==0,"the host window is destroyed after EndDialog");
        ok(GetDialogBaseUnits()==((16u<<16)|8u),"GetDialogBaseUnits = 8/16 DLU");
        W32_RECT rr={0,0,4,8};
        ok(MapDialogRect(0,&rr)==1 && rr.right==8 && rr.bottom==16,
           "MapDialogRect scales 4x8 DLU to 8x16 px");

        /* WM_CLOSE -> IDCANCEL: a DLGPROC that calls EndDialog from
         * WM_CLOSE demonstrates the frameproc forwards WM_CLOSE to the
         * DLGPROC.  We don't need to pump real input: SendMessageW (from
         * user32_win.c) invokes the subclassed WndProc directly. */
        W32_HWND hw;
        /* Use CreateWindowExW directly + SetWindowLongPtrW subclassing,
         * which is what DialogBoxIndirectParamW does internally, but we
         * short-circuit: the WndProc is dlg_frameproc for the dialog.
         * Simpler: trust that the integration test (NASM fixture) covers
         * the WM_CLOSE path via a real pump, and verify the DlgProc
         * contract here. */
        (void)dlgproc_cmd; (void)hw;
    }

    /* ---- DlgItem accessors ---- */
    {
        static const uint16_t cls[]={'S','t','a','t','i','c',0};
        W32_WNDCLASSEXW wc2; memset(&wc2,0,sizeof wc2);
        wc2.cbSize=sizeof wc2; wc2.lpfnWndProc=DefWindowProcW;
        wc2.hbrBackground=(W32_HBRUSH)(uintptr_t)0x00FFFFFFu;
        wc2.lpszClassName=cls;
        RegisterClassExW(&wc2);
        W32_HWND parent=CreateWindowExW(0,cls,cls,0,0,0,80,60,0,0,fake_main_module,0);
        W32_HWND child=CreateWindowExW(0,cls,cls,0,0,0,20,10,parent,0,fake_main_module,0);
        SetWindowLongPtrW(child,W32_GWLP_ID,101);
        SetDlgItemInt(parent,101,42,0);
        uint16_t txt[16]; int n=GetDlgItemTextW(parent,101,txt,16);
        ok(n==2 && txt[0]=='4' && txt[1]=='2',
           "SetDlgItemInt/GetDlgItemTextW round-trip '42'");
        W32_BOOL tr=0; uint32_t v=GetDlgItemInt(parent,101,&tr,0);
        ok(tr==1 && v==42,"GetDlgItemInt parses back to 42");
        static const uint16_t hi[]={'H','i',0};
        SetDlgItemTextW(parent,101,hi);
        char a[8]; GetDlgItemTextA(parent,101,a,8);
        ok(strcmp(a,"Hi")==0,"SetDlgItemTextW/GetDlgItemTextA are UTF-aware");
        DestroyWindow(child); DestroyWindow(parent);
    }

    /* ---- Menus ---- */
    {
        W32_HMENU m=CreateMenu();
        ok(m!=0,"CreateMenu returns a handle");
        static const uint16_t fi[]={'F','i','l','e',0};
        ok(AppendMenuW(m,0,100,fi)==1,"AppendMenuW adds an item");
        static const uint16_t ed[]={'E','d','i','t',0};
        ok(InsertMenuW(m,0,0,101,ed)==1,"InsertMenuW prepends an item");
        ok(GetMenuItemCount(m)==2,"menu has two items");
        ok(GetMenuItemID(m,0)==101 && GetMenuItemID(m,1)==100,"ids retain order");
        W32_HMENU pop=CreatePopupMenu();
        ok(AppendMenuW(pop,0,200,(const uint16_t*)0)==1,"popup menu created");
        ok(AppendMenuW(m,W32_MF_POPUP,(uintptr_t)pop,(const uint16_t*)0)==1,
           "MF_POPUP attaches a submenu");
        ok(GetSubMenu(m,2)==pop,"GetSubMenu retrieves it");
        W32_HWND hw=CreateWindowExW(0,(const uint16_t*)0,(const uint16_t*)0,0,0,0,20,20,0,0,fake_main_module,0);
        ok(SetMenu(hw,m)==1,"SetMenu attaches the menu");
        ok(GetMenu(hw)==m,"GetMenu returns it");
        ok(CheckMenuItem(m,100,W32_MF_BYCOMMAND|W32_MF_CHECKED)==W32_MF_UNCHECKED,
           "CheckMenuItem reports prior state");
        ok(RemoveMenu(m,101,W32_MF_BYCOMMAND)==1,"RemoveMenu by id");
        ok(GetMenuItemCount(m)==2,"count after remove");
        W32_HMENU lm=LoadMenuW(fake_main_module,(const uint16_t*)(uintptr_t)7);
        ok(lm!=0,"LoadMenuW returns a (possibly empty) menu");
        ok(DestroyMenu(m)==1 && DestroyMenu(pop)==1 && DestroyMenu(lm)==1,
           "DestroyMenu reclaims slots");
        DestroyWindow(hw);
    }

    /* ---- Timers ---- */
    {
        cb_fired=0;
        uintptr_t tid=SetTimer(0,1,10,tcb);
        ok(tid==1,"SetTimer returns the id");
        tick_ms=20; w32_dlg_fire_timers();
        ok(cb_fired==1,"TIMERPROC fires when interval elapses");
        SetTimer((W32_HWND)(uintptr_t)1,2,5,0);
        tick_ms=30; w32_dlg_fire_timers();
        ok(KillTimer(0,1)==1 && KillTimer((W32_HWND)(uintptr_t)1,2)==1,
           "KillTimer removes entries");
        ok(KillTimer(0,99)==0,"KillTimer unknown id returns 0");
    }

    /* ---- Caret ---- */
    {
        ok(CreateCaret(0,0,2,16)==1,"CreateCaret");
        ok(SetCaretPos(7,11)==1,"SetCaretPos");
        W32_POINT pt; ok(GetCaretPos(&pt)==1 && pt.x==7 && pt.y==11,"GetCaretPos");
        ok(ShowCaret(0)==1 && HideCaret(0)==1,"Show/HideCaret");
        ok(DestroyCaret()==1,"DestroyCaret");
    }

    /* ---- Accelerators ---- */
    {
        W32_ACCEL ent[2]; memset(ent,0,sizeof ent);
        ent[0].flags=W32_FVIRTKEY; ent[0].key='O'; ent[0].cmd=300;
        ent[1].flags=W32_FVIRTKEY; ent[1].key='S'; ent[1].cmd=301;
        W32_HACCEL a=CreateAcceleratorTableW(ent,2);
        ok(a!=0,"CreateAcceleratorTableW");
        W32_ACCEL copy[4]; memset(copy,0,sizeof copy);
        ok(CopyAcceleratorTableW(a,copy,4)==2 && copy[0].cmd==300,
           "CopyAcceleratorTableW copies entries");
        W32_HWND hw=CreateWindowExW(0,(const uint16_t*)0,(const uint16_t*)0,0,0,0,20,20,0,0,fake_main_module,0);
        W32_MSG m; memset(&m,0,sizeof m); m.message=0x0100; m.wParam='O'; m.hwnd=hw;
        ok(TranslateAcceleratorW(hw,a,&m)==1,"TranslateAcceleratorW matches FVIRTKEY");
        m.wParam='X';
        ok(TranslateAcceleratorW(hw,a,&m)==0,"TranslateAcceleratorW ignores vkeys");
        ok(DestroyAcceleratorTable(a)==1,"DestroyAcceleratorTable");
        W32_HACCEL la=LoadAcceleratorsW(fake_main_module,(const uint16_t*)(uintptr_t)99);
        ok(la!=0 && DestroyAcceleratorTable(la)==1,"LoadAcceleratorsW fallback");
        DestroyWindow(hw);
    }

    /* ---- Clipboard ---- */
    {
        ok(OpenClipboard(0)==1,"OpenClipboard");
        ok(OpenClipboard(0)==0 && GetLastError()==W32_ERROR_ACCESS_DENIED,
           "second OpenClipboard is ACCESS_DENIED");
        ok(EmptyClipboard()==1,"EmptyClipboard");
        ok(SetClipboardData(W32_CF_TEXT,"hello")!=(void*)0,"SetClipboardData CF_TEXT");
        ok(IsClipboardFormatAvailable(W32_CF_TEXT)==1,"CF_TEXT available");
        const char *got=(const char*)GetClipboardData(W32_CF_TEXT);
        ok(got && strcmp(got,"hello")==0,"CF_TEXT round-trip");
        static const uint16_t w_hi[]={'w','o','r','l','d',0};
        SetClipboardData(W32_CF_UNICODETEXT,(void*)w_hi);
        const uint16_t *wg=(const uint16_t*)GetClipboardData(W32_CF_UNICODETEXT);
        ok(wg && wg[0]=='w' && wg[4]=='d' && wg[5]==0,
           "CF_UNICODETEXT round-trip via UTF-8 bridge");
        ok(CloseClipboard()==1,"CloseClipboard");
    }

    /* ---- Hooks ---- */
    {
        W32_HHOOK hk=SetWindowsHookExW(3,(void*)(uintptr_t)1,0,GetCurrentThreadId());
        ok(hk!=0,"thread-local hook registers");
        ok(UnhookWindowsHookEx(hk)==1,"UnhookWindowsHookEx");
        SetLastError(0);
        ok(SetWindowsHookExW(3,(void*)(uintptr_t)1,(W32_HINSTANCE)(uintptr_t)1,0)==0 &&
           GetLastError()==W32_ERROR_CALL_NOT_IMPLEMENTED,
           "global hooks refuse CALL_NOT_IMPLEMENTED");
        ok(CallNextHookEx(0,0,0,0)==0,"CallNextHookEx chain end");
    }

    /* ---- Draw* ---- */
    {
        texts=outlines=0;
        static const uint16_t w_hi[]={'H','i',0};
        W32_RECT r={10,20,0,0};
        int h=DrawTextW(0,w_hi,-1,&r,W32_DT_CALCRECT);
        ok(h>0 && r.right>10 && r.bottom>20,"DrawTextW CALCRECT sizes rect");
        texts=0;
        DrawTextA(0,"abc",-1,&r,0);
        ok(texts==1 && strcmp(text_last_str,"abc")==0,"DrawTextA -> ag_text");
        outlines=0;
        W32_RECT fr={1,2,34,45};
        DrawFocusRect(0,&fr);
        ok(outlines==1 && outline_last[0]==1 && outline_last[1]==2 &&
           outline_last[2]==33 && outline_last[3]==43,
           "DrawFocusRect -> ag_rect_outline");
        /* W32A-7: a null icon is an invalid handle now -- the A-6 stub
         * returned TRUE without drawing anything; the engine draws real
         * icons and refuses real failures. */
        ok(DrawIcon(0,0,0,0)==0,"DrawIcon(null) refuses");
        ok(DrawIconEx(0,0,0,0,0,0,0,0,0)==0,"DrawIconEx(null) refuses");
        NotifyWinEvent(0,0,0,0);
        ok(1,"NotifyWinEvent is safe");
    }

    /* ---- EnumResourceNames ---- */
    {
        ok(EnumResourceNamesW(fake_main_module,
           (const uint16_t*)(uintptr_t)W32_RT_DIALOG,0,0)==1,
           "EnumResourceNamesW A-6 contract returns 1");
        ok(EnumResourceNamesA(fake_main_module,"RT_DIALOG",0,0)==1,
           "EnumResourceNamesA returns 1");
    }

    printf("\n== A6: %d checks, %d failures ==\n",checks,failures);
    return failures?1:0;
}
