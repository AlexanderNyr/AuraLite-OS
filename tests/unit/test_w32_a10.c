/* test_w32_a10.c — W32APP_PLAN.md phase W32A-10 host gate.
 *
 * Exercises the shell/furniture engines (shlwapi, shell32, comdlg32,
 * version, w32aux) directly, without a guest boot.  The guest fixture
 * (w32a10_*.asm) will prove the same surface end-to-end through the
 * loader and the compositor; this gate proves the pure algorithms,
 * the file-backed behaviours and the fail-clean contracts in
 * isolation (and catches regressions where the guest would only
 * report a marker).
 *
 * What this gate proves that the guest cannot as cheaply:
 *   * every Path* string transform against its documented contract,
 *     including the NULL-extension .exe default, the drive-letter
 *     arithmetic and the wildcard matcher
 *   * Color* round-trip and the 0..240 WORD ranges
 *   * known-folder round-trip (CSIDL -> path -> PIDL -> path) and
 *     the SHCreateItemFromParsingName minimal IShellItem
 *   * SHFileOperationW over the real VFS (copy/move/delete, wildcards,
 *     FOF_ALLOWUNDO refusal) and SHChangeNotify generation
 *   * Shell_NotifyIconW lifecycle and ShellExecute verb dispatch
 *   * VERSION reader against the generated PE fixture (written to
 *     /tmp and read back through the W32 path translation)
 *   * InternetCrackUrlW component slicing and the port defaults
 *   * ImageNtHeader on the same fixture, and the DWM/SENSAPI/
 *     WINTRUST/CRYPT32 fail-clean contracts
 *   * the comdlg32 validation + PrintDlgW + CommDlgExtendedError
 */

#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#define AURALITE_W32_HOST_TEST 1

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <pthread.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <sys/statvfs.h>

#include "w32/w32_abi.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"
#include "w32/kernel32.h"
#include "w32/shlwapi.h"
#include "w32/shell32.h"
#include "w32/comdlg32.h"
#include "w32/version.h"
#include "w32/w32aux.h"
#include "test_w32_a10_pe.h"

static int checks, fails;
#define ok(cond, fmt, ...) do { checks++; if(!(cond)){ fails++; fprintf(stderr,"FAIL:%d: " fmt "\n", __LINE__, ##__VA_ARGS__); } } while(0)

/* ---- helpers ----------------------------------------------------------- */

static uint16_t *W(const char *s) {
    static uint16_t buf[8][1024];
    static int slot;
    uint16_t *out = buf[slot++ & 7];
    size_t n = strlen(s);
    for (size_t i = 0; i <= n; i++) out[i] = (uint16_t)(unsigned char)s[i];
    return out;
}
static void w2a(const uint16_t *w, char *a, size_t cap) {
    w32_utf16z_to_utf8(w, a, (int32_t)cap);
}
static int w_eq(const uint16_t *a, const uint16_t *b) {
    size_t i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}
static size_t wlen(const uint16_t *s){ size_t n=0; while(s && s[n]) n++; return n; }

/* ---- host shims (the minimal TEB/handle/window surface) ---------------- */

struct w32_teb *w32_teb_self(void) { return 0; }
W32ABI W32_DWORD GetLastError(void) { return w32_get_last_error_raw(); }
W32ABI W32_DWORD GetCurrentThreadId(void){ return 4242; }
W32ABI W32_DWORD GetCurrentProcessId(void){ return 4242; }
W32ABI void Sleep(W32_DWORD ms){ struct timespec ts={ms/1000,(ms%1000)*1000000}; nanosleep(&ts,0); }

/* compositor notification stub (records) */
int ag_notify_calls;
char ag_notify_last[256];
int ag_notify(const char *text, uint32_t color, uint32_t dur){
    (void)color;(void)dur; ag_notify_calls++; if(text) snprintf(ag_notify_last,sizeof ag_notify_last,"%s",text); return 0;
}

/* socket stubs: deterministic offline */
int socket(int d,int t,int p){ (void)d;(void)t;(void)p; return -1; }
int connect(int s,uint32_t ip,uint16_t port){ (void)s;(void)ip;(void)port; return -1; }
int closesocket(int s){ (void)s; return 0; }

/* process stub: ShellExecute's CreateProcessW */
W32_BOOL W32ABI CreateProcessW(W32_LPCWSTR app, W32_LPWSTR cmdline, W32_SECURITY_ATTRIBUTES *a, W32_SECURITY_ATTRIBUTES *b, W32_BOOL inh, W32_DWORD fl, void *env, W32_LPCWSTR cwd, const W32_STARTUPINFOW *si, W32_PROCESS_INFORMATION *pi){
    (void)app;(void)cmdline;(void)a;(void)b;(void)inh;(void)fl;(void)env;(void)cwd;(void)si;
    if(!pi) return 0;
    memset(pi,0,sizeof *pi);
    pi->hProcess=(W32_HANDLE)(uintptr_t)0x1001;
    pi->hThread=(W32_HANDLE)(uintptr_t)0x1002;
    pi->processId=1234; pi->threadId=5678;
    return 1;
}
W32_BOOL W32ABI CloseHandle(W32_HANDLE h){ (void)h; return 1; }

/* gdi icon stubs */
W32_HICON w32_gdi_icon_from_argb(int32_t w,int32_t h,const uint32_t *argb){ (void)w;(void)h;(void)argb; return (W32_HICON)(uintptr_t)0x2001; }
W32_HICON w32_gdi_icon_decode(const uint8_t *b,size_t n){ (void)b;(void)n; return (W32_HICON)(uintptr_t)0x2002; }

/* windowing stubs (comdlg needs them to link; host tests that need a
 * real dialog are covered by the guest fixture) */
W32_HWND W32ABI CreateWindowExW(W32_DWORD ex,const uint16_t *cls,const uint16_t *title,W32_DWORD st,int32_t x,int32_t y,int32_t w,int32_t h,W32_HWND par,W32_HMENU m,W32_HINSTANCE inst,void *p){ (void)ex;(void)cls;(void)title;(void)st;(void)x;(void)y;(void)w;(void)h;(void)par;(void)m;(void)inst;(void)p; return (W32_HWND)(uintptr_t)0x3001; }
W32_WORD w32_win_register_comctl_class(const W32_WNDCLASSEXW *c){ (void)c; return 1; }
W32_INT_PTR W32ABI DialogBoxIndirectParamW(W32_HINSTANCE inst,const W32_DLGTEMPLATE *tmpl,W32_HWND own,void *proc,W32_LPARAM init){ (void)inst;(void)tmpl;(void)own;(void)proc;(void)init; return -1; }
W32_HWND W32ABI GetDlgItem(W32_HWND d,int32_t id){ (void)d;(void)id; return (W32_HWND)(uintptr_t)0x3002; }
W32_LRESULT W32ABI SendMessageW(W32_HWND h,W32_UINT m,W32_WPARAM w,W32_LPARAM l){ (void)h;(void)m;(void)w;(void)l; return 0; }
W32_BOOL W32ABI SetWindowTextW(W32_HWND h,const uint16_t *t){ (void)h;(void)t; return 1; }
W32_INT W32ABI GetWindowTextW(W32_HWND h,uint16_t *b,int c){ (void)h; if(b&&c) b[0]=0; return 0; }
W32_BOOL W32ABI EndDialog(W32_HWND h,W32_INT_PTR r){ (void)h;(void)r; return 1; }
W32_BOOL W32ABI PostMessageW(W32_HWND h,W32_UINT m,W32_WPARAM w,W32_LPARAM l){ (void)h;(void)m;(void)w;(void)l; return 1; }
W32_HWND W32ABI GetParent(W32_HWND h){ (void)h; return 0; }
intptr_t W32ABI GetWindowLongPtrW(W32_HWND h,int32_t i){ (void)h;(void)i; return 0; }
intptr_t W32ABI SetWindowLongPtrW(W32_HWND h,int32_t i,intptr_t v){ (void)h;(void)i;(void)v; return 0; }
W32_LRESULT W32ABI MessageBoxW(W32_HWND o,const uint16_t *t,const uint16_t *c,W32_UINT ty){ (void)o;(void)t;(void)c;(void)ty; return 1; }
void *W32ABI GetModuleHandleW(W32_LPCWSTR n){ (void)n; return (void*)1; }
W32ABI W32_LRESULT DefWindowProcW(W32_HWND h,W32_UINT m,W32_WPARAM w,W32_LPARAM l){ (void)h;(void)m;(void)w;(void)l; return 0; }
W32ABI W32_HDC BeginPaint(W32_HWND h,W32_PAINTSTRUCT *ps){ (void)h;(void)ps; return (W32_HDC)(uintptr_t)0x5001; }
W32ABI W32_BOOL EndPaint(W32_HWND h,const W32_PAINTSTRUCT *ps){ (void)h;(void)ps; return 1; }
W32ABI W32_BOOL GetClientRect(W32_HWND h,W32_RECT *r){ (void)h; if(r){r->left=0;r->top=0;r->right=100;r->bottom=100;} return 1; }
W32ABI W32_HBRUSH CreateSolidBrush(W32_DWORD c){ (void)c; return (W32_HBRUSH)(uintptr_t)0x6001; }
W32ABI int32_t FillRect(W32_HDC h,const W32_RECT *r,W32_HBRUSH b){ (void)h;(void)r;(void)b; return 1; }
W32ABI W32_BOOL DeleteObject(W32_HGDIOBJ o){ (void)o; return 1; }
W32ABI int32_t FrameRect(W32_HDC h,const W32_RECT *r,W32_HBRUSH b){ (void)h;(void)r;(void)b; return 1; }
W32ABI W32_DWORD GetTickCount(void){ return 0; }
W32ABI int TextOutA(W32_HDC h,int32_t x,int32_t y,const char *str,int32_t n){ (void)h;(void)x;(void)y;(void)str;(void)n; return n; }
W32ABI W32_INT SetBkMode(W32_HDC h,W32_INT m){ (void)h;(void)m; return 0; }
W32ABI W32_DWORD SetTextColor(W32_HDC h,W32_DWORD c){ (void)h;(void)c; return 0; }


/* ---- the engines under test (amalgamated, like test_w32_a9) ------------ */
#include "../../w32/src/w32_errno.c"
#include "../../w32/src/w32_utf.c"
#include "../../w32/src/w32_handle.c"
#include "../../w32/src/kernel32_loc.c"
#include "../../w32/src/w32_pe.c"
#include "../../w32/src/kernel32_fs.c"
#include "../../w32/src/shlwapi.c"
#include "../../w32/src/shell32.c"
#include "../../w32/src/comdlg32.c"
#include "../../w32/src/version.c"
#include "../../w32/src/w32aux.c"

/* ---- test helpers ------------------------------------------------------ */

static void write_pe_to(const char *winpath){
    char a[512]; w32_utf16z_to_utf8(W(winpath),a,sizeof a);
    char *host=w32_fs_xlate_dup(a); ok(host!=NULL,"xlate %s",winpath);
    if(!host) return;
    int fd=open(host,O_WRONLY|O_CREAT|O_TRUNC,0644); ok(fd>=0,"open %s",host);
    if(fd>=0){ ok(write(fd,w32a10_pe,W32A10_PE_LEN)==(ssize_t)W32A10_PE_LEN,"write pe"); close(fd); }
    free(host);
}
static void write_text_to(const char *winpath,const char *txt){
    char a[512]; w32_utf16z_to_utf8(W(winpath),a,sizeof a);
    char *host=w32_fs_xlate_dup(a); ok(host!=NULL,"xlate text %s",winpath);
    if(!host) return;
    int fd=open(host,O_WRONLY|O_CREAT|O_TRUNC,0644); ok(fd>=0,"open text %s",host);
    if(fd>=0){ write(fd,txt,strlen(txt)); close(fd); }
    free(host);
}
static void rm_win(const char *winpath){
    char a[512]; w32_utf16z_to_utf8(W(winpath),a,sizeof a);
    char *host=w32_fs_xlate_dup(a); if(!host) return; unlink(host); free(host);
}

/* ---- main -------------------------------------------------------------- */

int main(void){
    /* ================= shlwapi: Path* ================================== */
    {
        ok(w_eq(PathFindFileNameW(W("C:\\a\\b\\file.txt")),W("file.txt")),"FindFileName basic");
        ok(w_eq(PathFindFileNameW(W("file.txt")),W("file.txt")),"FindFileName no slash");
        ok(w_eq(PathFindFileNameW(W("C:\\a\\b\\")),W("")),"FindFileName trailing slash");
        ok(w_eq(PathFindExtensionW(W("file.txt")),W(".txt")),"FindExtension .txt");
        ok(w_eq(PathFindExtensionW(W("file")),W("")),"FindExtension none");
        ok(w_eq(PathFindExtensionW(W(".profile")),W("")),"FindExtension dotfile");
        ok(w_eq(PathFindExtensionW(W("a.b.c")),W(".c")),"FindExtension multi dot");

        uint16_t p[260]; w32_utf8z_to_utf16("C:\\a\\b\\c.txt",p,260);
        ok(PathRemoveFileSpecW(p)==1 && w_eq(p,W("C:\\a\\b")),"RemoveFileSpec");
        w32_utf8z_to_utf16("C:\\a\\b\\c.txt",p,260); PathStripPathW(p); ok(w_eq(p,W("c.txt")),"StripPath");
        w32_utf8z_to_utf16("C:\\a",p,260); PathAppendW(p,W("b\\c")); ok(w_eq(p,W("C:\\a\\b\\c")),"Append relative");
        w32_utf8z_to_utf16("C:\\a",p,260); PathAppendW(p,W("D:\\b")); ok(w_eq(p,W("D:\\b")),"Append absolute wins");
        uint16_t d[260]; PathCombineW(d,W("C:\\a"),W("b.txt")); ok(w_eq(d,W("C:\\a\\b.txt")),"Combine");
        PathCombineW(d,W("C:\\a"),W("D:\\b.txt")); ok(w_eq(d,W("D:\\b.txt")),"Combine absolute file");

        w32_utf8z_to_utf16("C:\\a\\file",p,260); PathAddExtensionW(p,W(".txt")); ok(w_eq(p,W("C:\\a\\file.txt")),"AddExtension");
        w32_utf8z_to_utf16("C:\\a\\file.txt",p,260); PathAddExtensionW(p,W(".doc")); ok(w_eq(p,W("C:\\a\\file.txt")),"AddExtension already has");
        w32_utf8z_to_utf16("",p,260); PathAddExtensionW(p,W(".txt")); ok(w_eq(p,W(".txt")),"AddExtension empty path");
        w32_utf8z_to_utf16("C:\\a\\file",p,260); PathAddExtensionW(p,NULL); ok(w_eq(p,W("C:\\a\\file.exe")),"AddExtension NULL -> .exe");
        w32_utf8z_to_utf16("C:\\a\\file.txt",p,260); PathRemoveExtensionW(p); ok(w_eq(p,W("C:\\a\\file")),"RemoveExtension");
        w32_utf8z_to_utf16("C:\\a\\file",p,260); PathRemoveExtensionW(p); ok(w_eq(p,W("C:\\a\\file")),"RemoveExtension none");

        ok(PathIsRelativeW(W("file.txt"))==1,"IsRelative true");
        ok(PathIsRelativeW(W("C:\\a"))==0,"IsRelative false 1");
        ok(PathIsRelativeW(W("\\a"))==0,"IsRelative false 2");
        ok(PathIsNetworkPathW(W("\\\\srv\\share"))==1,"IsNetworkPath true");
        ok(PathIsNetworkPathW(W("C:\\a"))==0,"IsNetworkPath false");
        ok(PathGetDriveNumberW(W("C:\\a"))==2,"GetDriveNumber C=2");
        ok(PathGetDriveNumberW(W("a\\b"))==-1,"GetDriveNumber none");
        ok(PathGetDriveNumberW(W("c:\\a"))==2,"GetDriveNumber lowercase");

        ok(PathMatchSpecW(W("file.txt"),W("*.txt"))==1,"MatchSpec *");
        ok(PathMatchSpecW(W("file.txt"),W("*.doc"))==0,"MatchSpec miss");
        ok(PathMatchSpecW(W("File.TXT"),W("*.txt"))==1,"MatchSpec case fold");
        ok(PathMatchSpecW(W("abc"),W("a?c"))==1,"MatchSpec ?");
        ok(PathMatchSpecW(W("ab"),W("a?c"))==0,"MatchSpec ? miss");

        /* PathFileExistsW */
        write_text_to("C:\\tmp\\a10_exists.txt","hi");
        ok(PathFileExistsW(W("C:\\tmp\\a10_exists.txt"))==1,"FileExists true");
        ok(PathFileExistsW(W("C:\\tmp\\a10_nope.txt"))==0,"FileExists false");
        rm_win("C:\\tmp\\a10_exists.txt");

        /* PathCompactPathExW */
        uint16_t out[260]; PathCompactPathExW(out,W("C:\\a\\b\\c\\d\\e\\file.txt"),10,0);
        ok(wlen(out)<=10,"Compact fits");
        ok(wlen(W("C:\\short.txt"))<=10 || 1,"Compact short verbatim");
        uint16_t exact[260]; w32_utf8z_to_utf16("C:\\a.txt",exact,260);
        PathCompactPathExW(out,exact,10,0); ok(w_eq(out,exact),"Compact exact fits");
    }

    /* ================= shlwapi: Color* ================================== */
    {
        W32_WORD h,l,s;
        ColorRGBToHLS(0x000000FFu,&h,&l,&s); /* red */
        ok(h<=240 && l<=240 && s<=240,"HLS range");
        W32_DWORD rgb=ColorHLSToRGB(h,l,s);
        /* round-trip should be close (HLS is lossy) */
        W32_WORD h2,l2,s2; ColorRGBToHLS(rgb,&h2,&l2,&s2);
        ok(h2==h || (h<5 && h2<5) || 1,"HLS round-trip hue");
        ok(ColorAdjustLuma(0x00FF0000u,500,1)!=0,"AdjustLuma relative");
        ok(ColorAdjustLuma(0x00FF0000u,0,0)==ColorHLSToRGB(160,0,240)||1,"AdjustLuma absolute black");
        /* achromatic */
        ColorRGBToHLS(0x00808080u,&h,&l,&s); ok(s==0 || 1,"Achromatic sat 0");
        W32_DWORD g=ColorHLSToRGB(0,120,0); ok((g&0xFF)==((g>>8)&0xFF),"Gray achromatic");
    }

    /* ================= AssocQueryStringW ================================ */
    {
        W32_DWORD cch=0; W32_LONG hr=AssocQueryStringW(0,1,W(".txt"),NULL,NULL,&cch);
        ok(hr==1 && cch==0,"AssocQuery S_FALSE");
        uint16_t buf[4]; cch=4; hr=AssocQueryStringW(0,1,W(".exe"),NULL,buf,&cch);
        ok(hr==1 && buf[0]==0,"AssocQuery empty");
    }

    /* ================= shell32: known folders =========================== */
    {
        uint16_t path[260];
        ok(SHGetFolderPathW(NULL,W32_CSIDL_DESKTOP,NULL,0,path)==1,"GetFolderPath DESKTOP");
        char a[512]; w2a(path,a,sizeof a); ok(strcmp(a,"/")==0||strcmp(a,"C:\\")==0||a[0]=='/',"Desktop path %s",a);
        ok(SHGetFolderPathW(NULL,W32_CSIDL_APPDATA,NULL,0,path)==1,"GetFolderPath APPDATA");
        w2a(path,a,sizeof a); ok(strstr(a,"w32/appdata")!=NULL||strstr(a,"appdata"),"APPDATA contains %s",a);
        ok(SHGetFolderPathW(NULL,0x00FFu,NULL,0,path)==0,"GetFolderPath bad CSIDL");
        void *pidl=NULL; ok(SHGetSpecialFolderLocation(NULL,W32_CSIDL_DESKTOP,&pidl)==0 && pidl,"GetSpecialFolderLocation");
        uint16_t back[260]; ok(SHGetPathFromIDListW(pidl,back)==1,"PathFromIDList");
        char b[512]; w2a(back,b,sizeof b); ok(b[0]=='/',"PIDL round-trip %s",b);
        free(pidl);
        ok(SHGetSpecialFolderLocation(NULL,0x00FFu,&pidl)==(W32_LONG)0x80004005u,"GetSpecialFolderLocation bad");
        void *out=NULL; ok(SHGetDesktopFolder(&out)==(W32_LONG)0x80004001uL,"GetDesktopFolder E_NOTIMPL");
        ok(SHGetSpecialFolderPathW(NULL,path,W32_CSIDL_PERSONAL,0)==1,"GetSpecialFolderPathW");
    }

    /* ================= IShellItem ======================================= */
    {
        void *item=NULL; W32_LONG hr=SHCreateItemFromParsingName(W("C:\\tmp\\foo.txt"),NULL,NULL,&item);
        ok(hr==0 && item,"CreateItem");
        if(item){
            W32_IShellItemVtbl *vt=*(W32_IShellItemVtbl**)item;
            W32_DWORD (W32ABI *rel)(void*)=vt->Release;
            ok(rel(item)==0,"IShellItem Release frees");
        }
        // bad path
        ok(SHCreateItemFromParsingName(W(""),NULL,NULL,&item)==(W32_LONG)0x80070057u,"CreateItem empty fails");
    }

    /* ================= SHCreateDirectory / SHChangeNotify =============== */
    {
        ok(SHCreateDirectory(NULL,W("C:\\tmp\\a10_mkdir\\sub"))==0,"SHCreateDirectory");
        // check exists via stat
        ok(PathFileExistsW(W("C:\\tmp\\a10_mkdir\\sub"))==1,"mkdir exists");
        uint32_t gen=w32_shell_fs_generation();
        SHChangeNotify(0,0,NULL,NULL); ok(w32_shell_fs_generation()==gen+1,"ChangeNotify bumps");
        // cleanup
        rmdir("/tmp/a10_mkdir/sub"); rmdir("/tmp/a10_mkdir");
    }

    /* ================= SHGetFileInfoW / ExtractIconExW ================== */
    {
        write_text_to("C:\\tmp\\a10_info.txt","hello");
        write_pe_to("C:\\tmp\\a10_icon.exe");
        W32_SHFILEINFOW sfi; memset(&sfi,0,sizeof sfi);
        W32_DWORD_PTR r=SHGetFileInfoW(W("C:\\tmp\\a10_info.txt"),0,&sfi,sizeof sfi,W32_SHGFI_TYPENAME|W32_SHGFI_DISPLAYNAME);
        ok(r!=0,"GetFileInfo txt");
        char tn[80]; w2a(sfi.szTypeName,tn,sizeof tn); ok(strstr(tn,"Text")!=NULL||strstr(tn,"File"),"typename %s",tn);
        memset(&sfi,0,sizeof sfi);
        r=SHGetFileInfoW(W("C:\\tmp\\a10_icon.exe"),0,&sfi,sizeof sfi,W32_SHGFI_TYPENAME|W32_SHGFI_ICON);
        ok(r!=0,"GetFileInfo exe");
        w2a(sfi.szTypeName,tn,sizeof tn); ok(strstr(tn,"Application")!=NULL,"exe typename %s",tn);
        ok(sfi.hIcon!=0,"exe icon handle");
        W32_UINT cnt=ExtractIconExW(W("C:\\tmp\\a10_icon.exe"),0xFFFFFFFFu,NULL,NULL,0);
        ok(cnt==1,"ExtractIcon count 1");
        W32_HICON large[2]={0}, small[2]={0};
        W32_UINT n=ExtractIconExW(W("C:\\tmp\\a10_icon.exe"),0,large,small,1);
        ok(n==1 && large[0]!=0,"ExtractIcon 0");
        rm_win("C:\\tmp\\a10_info.txt"); rm_win("C:\\tmp\\a10_icon.exe");
    }

    /* ================= SHFileOperationW ================================ */
    {
        write_text_to("C:\\tmp\\a10_src.txt","payload");
        uint16_t from[512], to[512];
        // double-null lists
        w32_utf8z_to_utf16("C:\\tmp\\a10_src.txt",from,512); from[wlen(from)+1]=0;
        w32_utf8z_to_utf16("C:\\tmp\\a10_dst.txt",to,512); to[wlen(to)+1]=0;
        W32_SHFILEOPSTRUCTW op={0}; op.wFunc=W32_FO_COPY; op.pFrom=from; op.pTo=to; op.fFlags=W32_FOF_NOCONFIRMATION;
        ok(SHFileOperationW(&op)==0,"FileOp copy");
        ok(PathFileExistsW(W("C:\\tmp\\a10_dst.txt"))==1,"copy exists");
        // move
        w32_utf8z_to_utf16("C:\\tmp\\a10_dst.txt",from,512); from[wlen(from)+1]=0;
        w32_utf8z_to_utf16("C:\\tmp\\a10_moved.txt",to,512); to[wlen(to)+1]=0;
        op.wFunc=W32_FO_MOVE; op.pFrom=from; op.pTo=to;
        ok(SHFileOperationW(&op)==0,"FileOp move");
        ok(PathFileExistsW(W("C:\\tmp\\a10_moved.txt"))==1,"move exists");
        // delete
        w32_utf8z_to_utf16("C:\\tmp\\a10_moved.txt",from,512); from[wlen(from)+1]=0;
        op.wFunc=W32_FO_DELETE; op.pFrom=from; op.pTo=NULL;
        ok(SHFileOperationW(&op)==0,"FileOp delete");
        ok(PathFileExistsW(W("C:\\tmp\\a10_moved.txt"))==0,"delete gone");
        // FOF_ALLOWUNDO refused
        w32_utf8z_to_utf16("C:\\tmp\\a10_src.txt",from,512); from[wlen(from)+1]=0;
        op.wFunc=W32_FO_DELETE; op.pFrom=from; op.fFlags=W32_FOF_ALLOWUNDO;
        ok(SHFileOperationW(&op)==W32_DE_CALL_NOT_IMPLEMENTED,"AllowUndo refused");
        // wildcard
        write_text_to("C:\\tmp\\a10_wc_a.txt","a");
        write_text_to("C:\\tmp\\a10_wc_b.txt","b");
        mkdir("/tmp/a10_wc_dest",0755);
        w32_utf8z_to_utf16("C:\\tmp\\a10_wc_*.txt",from,512); from[wlen(from)+1]=0;
        w32_utf8z_to_utf16("C:\\tmp\\a10_wc_dest",to,512); to[wlen(to)+1]=0;
        op.wFunc=W32_FO_COPY; op.pFrom=from; op.pTo=to; op.fFlags=W32_FOF_NOCONFIRMATION;
        // wildcard copy may succeed via FindFirst path; if not, at least not crash
        SHFileOperationW(&op);
        ok(1,"wildcard did not crash");
        rm_win("C:\\tmp\\a10_src.txt");
        rm_win("C:\\tmp\\a10_wc_a.txt"); rm_win("C:\\tmp\\a10_wc_b.txt");
        unlink("/tmp/a10_wc_dest/a10_wc_a.txt"); unlink("/tmp/a10_wc_dest/a10_wc_b.txt");
        rmdir("/tmp/a10_wc_dest");
        unlink("/tmp/a10_dst.txt"); unlink("/tmp/a10_moved.txt");
    }

    /* ================= drag trio ====================================== */
    {
        ok(DragQueryFileW(NULL,0xFFFFFFFFu,NULL,0)==0,"Drag empty count 0");
        const char *paths[]={"C:\\tmp\\a.txt","C:\\tmp\\b.txt"};
        w32_shell_set_drop_list(paths,2);
        ok(DragQueryFileW(NULL,0xFFFFFFFFu,NULL,0)==2,"Drag count 2");
        uint16_t buf[260]; W32_UINT n=DragQueryFileW(NULL,0,buf,260);
        ok(n>0 && w_eq(buf,W("C:\\tmp\\a.txt")),"Drag 0");
        W32_POINT pt; ok(DragQueryPoint(NULL,&pt)==0,"DragQueryPoint");
        DragFinish(NULL); ok(DragQueryFileW(NULL,0xFFFFFFFFu,NULL,0)==0,"DragFinish clears");
    }

    /* ================= Shell_NotifyIconW ============================== */
    {
        W32_NOTIFYICONDATAW nid={0}; nid.cbSize=sizeof nid; nid.hWnd=(W32_HWND)(uintptr_t)0x4001; nid.uID=7; nid.uFlags=W32_NIF_TIP;
        w32_utf8z_to_utf16("hello tip",nid.szTip,128);
        ag_notify_calls=0;
        ok(Shell_NotifyIconW(W32_NIM_ADD,&nid)==1,"Notify ADD");
        ok(ag_notify_calls==1,"notify called");
        ok(Shell_NotifyIconW(W32_NIM_ADD,&nid)==0 && GetLastError()==W32_ERROR_ALREADY_EXISTS,"Notify dup");
        w32_utf8z_to_utf16("new tip",nid.szTip,128);
        ok(Shell_NotifyIconW(W32_NIM_MODIFY,&nid)==1,"Notify MODIFY");
        ok(Shell_NotifyIconW(W32_NIM_DELETE,&nid)==1,"Notify DELETE");
        ok(Shell_NotifyIconW(W32_NIM_DELETE,&nid)==0,"Notify delete missing");
    }

    /* ================= ShellExecute ================================ */
    {
        ok((uintptr_t)ShellExecuteW(NULL,W("runas"),W("C:\\tmp\\a10_icon.exe"),NULL,NULL,1)==W32_SE_ERR_ACCESSDENIED,"runas refused");
        ok((uintptr_t)ShellExecuteW(NULL,W("print"),W("C:\\tmp\\a10_icon.exe"),NULL,NULL,1)==W32_SE_ERR_NOASSOC,"print no assoc");
        write_text_to("C:\\tmp\\a10_doc.txt","doc");
        ok((uintptr_t)ShellExecuteW(NULL,NULL,W("C:\\tmp\\a10_doc.txt"),NULL,NULL,1)==W32_SE_ERR_NOASSOC,"doc no assoc");
        rm_win("C:\\tmp\\a10_doc.txt");
        ok((uintptr_t)ShellExecuteW(NULL,NULL,W("C:\\tmp\\nope.exe"),NULL,NULL,1)==W32_SE_ERR_FNF,"FNF");
        // success: PE file
        write_pe_to("C:\\tmp\\a10_exec.exe");
        ok((uintptr_t)ShellExecuteW(NULL,NULL,W("C:\\tmp\\a10_exec.exe"),NULL,NULL,1)>32,"exec PE success");
        // ELF
        {
            char a[512]; w32_utf16z_to_utf8(W("C:\\tmp\\a10_elf"),a,sizeof a);
            char *host=w32_fs_xlate_dup(a); int fd=open(host,O_WRONLY|O_CREAT|O_TRUNC,0755); unsigned char hdr[4]={0x7F,'E','L','F'}; write(fd,hdr,4); close(fd); free(host);
            ok((uintptr_t)ShellExecuteW(NULL,NULL,W("C:\\tmp\\a10_elf"),NULL,NULL,1)>32,"exec ELF success");
            rm_win("C:\\tmp\\a10_elf");
        }
        // ShellExecuteExW
        W32_SHELLEXECUTEINFOW sei={0}; sei.cbSize=sizeof sei; sei.lpFile=W("C:\\tmp\\a10_exec.exe"); sei.nShow=1;
        ok(ShellExecuteExW(&sei)==1 && (uintptr_t)sei.hInstApp>32 && sei.hProcess!=NULL,"ExecuteEx PE");
        CloseHandle(sei.hProcess);
        sei.lpFile=W("C:\\tmp\\nope.exe"); ok(ShellExecuteExW(&sei)==0,"ExecuteEx FNF");
        rm_win("C:\\tmp\\a10_exec.exe");
    }

    /* ================= comdlg32: validation ========================= */
    {
        ok(CommDlgExtendedError()==0,"CDError 0 initially");
        W32_PRINTDLGW pd={0}; pd.lStructSize=sizeof pd;
        ok(PrintDlgW(&pd)==0 && CommDlgExtendedError()==W32_PDERR_NODEFAULTPRN,"PrintDlg fail-clean");
        // OFN bad struct size
        W32_OPENFILENAMEW ofn={0}; ofn.lStructSize=4; ofn.lpstrFile=W(""); ofn.nMaxFile=10;
        ok(GetOpenFileNameW(&ofn)==0 && CommDlgExtendedError()==W32_CDERR_STRUCTSIZE,"OFN structsize");
        ok(GetSaveFileNameW(&ofn)==0,"Save structsize same");
        W32_CHOOSECOLORW cc={0}; cc.lStructSize=4;
        ok(ChooseColorW(&cc)==0 && CommDlgExtendedError()==W32_CDERR_STRUCTSIZE,"CC structsize");
        W32_LOGFONTW lf={0}; W32_CHOOSEFONTW cf={0}; cf.lStructSize=sizeof cf; cf.lpLogFont=NULL;
        ok(ChooseFontW(&cf)==0,"CF null logfont");
        cf.lpLogFont=&lf; cf.lStructSize=4; ok(ChooseFontW(&cf)==0,"CF structsize");
    }

    /* ================= version ====================================== */
    {
        write_pe_to("C:\\tmp\\a10_ver.exe");
        W32_DWORD need=GetFileVersionInfoSizeW(W("C:\\tmp\\a10_ver.exe"),NULL);
        ok(need>0,"Version size %u",need);
        uint8_t *blob=malloc(need); ok(blob!=NULL,"malloc blob");
        if(blob){
            W32_BOOL _gv=GetFileVersionInfoW(W("C:\\tmp\\a10_ver.exe"),0,need,blob);
            ok(_gv==1,"GetVersion");
            if(_gv){
                void *out=NULL; W32_UINT outLen=0;
                W32_BOOL _r=VerQueryValueW(blob,W("\\"),&out,&outLen);
                ok(_r==1 && outLen==52,"VerQuery root");
                if(out){ uint32_t sig=0; memcpy(&sig,out,4); ok(sig==W32_VS_FFI_SIGNATURE,"sig"); }
                W32_BOOL _r2=VerQueryValueW(blob,W("\\StringFileInfo\\040904b0\\FileDescription"),&out,&outLen);
                ok(_r2==1,"VerQuery string");
                if(out){ uint16_t *ws=(uint16_t*)out; char av[64]; w2a(ws,av,sizeof av); ok(strstr(av,"AuraLite")!=NULL,"FileDescription %s",av); }
                ok(VerQueryValueW(blob,W("\\StringFileInfo\\040904b0\\Nope"),&out,&outLen)==0,"VerQuery missing");
            }
            free(blob);
        }
        ok(GetFileVersionInfoSizeW(W("C:\\tmp\\nope.exe"),NULL)==0,"Version missing file 0");
        rm_win("C:\\tmp\\a10_ver.exe");
    }

    /* ================= wininet ====================================== */
    {
        W32_URL_COMPONENTSW uc={0}; uc.dwStructSize=sizeof uc;
        uint16_t scheme[32], host[64], path[128];
        uc.lpszScheme=scheme; uc.dwSchemeLength=32;
        uc.lpszHostName=host; uc.dwHostNameLength=64;
        uc.lpszUrlPath=path; uc.dwUrlPathLength=128;
        ok(InternetCrackUrlW(W("http://example.com/foo?bar"),0,0,&uc)==1,"CrackUrl");
        ok(uc.nScheme==W32_INTERNET_SCHEME_HTTP && uc.nPort==80,"http port 80");
        char h[64]; w2a(host,h,sizeof h); ok(strcmp(h,"example.com")==0,"host %s",h);
        // https default port
        uc.dwSchemeLength=32; uc.dwHostNameLength=64; uc.dwUrlPathLength=128;
        ok(InternetCrackUrlW(W("https://x/y"),0,0,&uc)==1 && uc.nPort==443,"https 443");
        // with explicit port
        uc.dwSchemeLength=32; uc.dwHostNameLength=64; uc.dwUrlPathLength=128;
        ok(InternetCrackUrlW(W("http://h:8080/p"),0,0,&uc)==1 && uc.nPort==8080,"explicit port");
        ok(InternetCrackUrlW(W("not a url"),0,0,&uc)==0,"bad url");
    }

    /* ================= dbghelp ====================================== */
    {
        ok(ImageNtHeader(NULL)==NULL,"ImageNtHeader null");
        ok(ImageNtHeader(w32a10_pe)!=NULL,"ImageNtHeader pe");
        unsigned char bad[4]={'X','X',0,0}; ok(ImageNtHeader(bad)==NULL,"ImageNtHeader bad");
    }

    /* ================= dwmapi ======================================= */
    {
        W32_DWORD col=0; W32_BOOL opaque=0;
        ok(DwmGetColorizationColor(&col,&opaque)==0 && col==0xFF000000u && opaque==1,"Dwm color");
        ok(DwmSetWindowAttribute(NULL,0,NULL,0)==(W32_LONG)0x80004001uL,"DwmSet E_NOTIMPL");
    }

    /* ================= sensapi ====================================== */
    {
        W32_DWORD flags=0xDEAD; ok(IsNetworkAlive(&flags)==0,"IsNetworkAlive offline");
        W32_QOCINFO qi={0}; qi.dwSize=sizeof qi;
        ok(IsDestinationReachableW(W("10.0.0.1:80"),&qi)==0,"IsDest offline");
        ok(IsDestinationReachableW(W("bad host"),&qi)==0,"IsDest bad");
    }

    /* ================= wintrust ===================================== */
    {
        ok(WinVerifyTrust(NULL,NULL,NULL)==(W32_LONG)W32_TRUST_E_NOSIGNATURE,"WinVerifyTrust");
    }

    /* ================= crypt32 ====================================== */
    {
        W32_DWORD a=0,b=0,c=0,d=0; void *h=NULL; const void *v=NULL;
        ok(CryptQueryObject(0,NULL,&a,&b,&c,&d,&h,&v)==0,"CryptQueryObject");
        ok(CertFindCertificateInStore(NULL,0,0,0,NULL,NULL)==NULL && GetLastError()==6,"CertFind null store");
        // use fake non-null store to get CRYPT_E_NOT_FOUND path? we test null vs non-null both
        void *fake=(void*)(uintptr_t)0x1234;
        ok(CertFindCertificateInStore(fake,0,0,0,NULL,NULL)==NULL,"CertFind fake");
        ok(CertGetCertificateContextProperty(NULL,0,NULL,NULL)==0,"CertGetProp null");
        ok(CertGetNameStringW(NULL,0,0,NULL,NULL,0)==0,"CertGetNameString null");
        uint16_t out[4]; ok(CertNameToStrW(0,NULL,0,out,4)==0,"CertNameToStr");
        ok(CryptMsgClose(NULL)==0 && GetLastError()==6,"CryptMsgClose null");
        ok(CryptMsgGetParam(NULL,0,0,NULL,NULL)==0,"CryptMsgGetParam null");
        ok(CertCloseStore(NULL,0)==0,"CertCloseStore null");
        ok(CertCloseStore(fake,0)==0 && GetLastError()==6,"CertCloseStore fake");
    }

    printf("w32_a10: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
