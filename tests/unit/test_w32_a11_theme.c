/* W32A-11 flat theme: known part matrix vs refusal, pixel-backed palette,
 * per-HWND override, Win64 layouts and zero-duration buffered frames.
 * SPDX-License-Identifier: Apache-2.0 */
#define AURALITE_W32_HOST_TEST 1
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "w32/uxtheme.h"
#include "w32/comctl32.h"
#include "w32/w32_errno.h"
static int v6=6,draw_count,theme_changes;
static uint32_t pixels[16][16];
static w32_comctl_palette_t palette={0x00E0E0E0,0x00000022,0x00555555,0x00AA3311,0x000011AA};
static W32_DWORD error;
void w32_set_last_error(W32_DWORD e){error=e;}
int w32_comctl_version(void){return v6;}
const w32_comctl_palette_t *w32_comctl_palette(void){return &palette;}
static const W32_HWND win=(W32_HWND)(uintptr_t)0x100;
W32ABI W32_BOOL IsWindow(W32_HWND hwnd){return hwnd==win;}
W32ABI W32_LRESULT SendMessageW(W32_HWND hwnd, W32_UINT m, W32_WPARAM a, W32_LPARAM b) {
    (void)a;(void)b; if(hwnd==win && m==0x031a)++theme_changes;
    return 0;
}
W32ABI W32_BOOL GetClientRect(W32_HWND hwnd,W32_RECT *r){
    if (hwnd != win) return 0;
    *r = (W32_RECT){0,0,16,16};
    return 1;
}
typedef struct { uint32_t pix[16][16]; } bmp;
typedef struct { bmp *selected; bmp stock; } dc;
static bmp *allocated[32];
static dc *contexts[16];
static bmp *find_bmp(W32_HDC h){
    uintptr_t idx=(uintptr_t)h;
    if(idx==1)return NULL;
    return idx<16 && contexts[idx] ? contexts[idx]->selected : NULL;
}
static uint32_t *cell(W32_HDC h,int x,int y){
    if(x<0||x>=16||y<0||y>=16)return NULL;
    if((uintptr_t)h==1)return &pixels[y][x];
    bmp *b=find_bmp(h);return b?&b->pix[y][x]:NULL;
}
W32ABI W32_HDC CreateCompatibleDC(W32_HDC other){
    if(!other)return NULL;
    for(uintptr_t i=2;i<16;i++) if(!contexts[i]){
        contexts[i]=calloc(1,sizeof(dc));
        if(!contexts[i])return NULL;
        contexts[i]->selected=&contexts[i]->stock;
        return (W32_HDC)i;
    }
    return NULL;
}
W32ABI W32_BOOL DeleteDC(W32_HDC h){
    uintptr_t i=(uintptr_t)h;
    if(i<2||i>=16||!contexts[i])return 0;
    free(contexts[i]);contexts[i]=NULL;return 1;
}
W32ABI W32_HBITMAP CreateCompatibleBitmap(W32_HDC h,int32_t w,int32_t ht){
    if(!h||w<1||w>16||ht<1||ht>16)return NULL;
    for(int i=0;i<32;i++)if(!allocated[i]){
        allocated[i]=calloc(1,sizeof(bmp));return allocated[i];
    }
    return NULL;
}
W32ABI W32_HGDIOBJ SelectObject(W32_HDC h,W32_HGDIOBJ obj){
    uintptr_t i=(uintptr_t)h;
    if(i<2||i>=16||!contexts[i]||!obj)return NULL;
    bmp *old=contexts[i]->selected;
    contexts[i]->selected=obj;
    return old;
}
W32ABI W32_BOOL BitBlt(W32_HDC dest,int32_t x,int32_t y,int32_t w,int32_t h,
                         W32_HDC src,int32_t sx,int32_t sy,W32_DWORD rop){
    if(rop!=W32_SRCCOPY)return 0;
    for(int j=0;j<h;j++)for(int i=0;i<w;i++){
        uint32_t *s=cell(src,sx+i,sy+j),*d=cell(dest,x+i,y+j);
        if (!s || !d) return 0;
        *d = *s;
    }
    return 1;
}
W32ABI W32_HBRUSH CreateSolidBrush(W32_DWORD c){return (W32_HBRUSH)(uintptr_t)(c+1);}
W32ABI W32_BOOL DeleteObject(void *h){
    if(!h)return 0;
    for(int i=0;i<32;i++)if(allocated[i]==(bmp *)h){
        free(allocated[i]);allocated[i]=NULL;return 1;
    }
    return 1;
}
W32ABI int32_t FillRect(W32_HDC h,const W32_RECT *r,W32_HBRUSH b){
    if(!h||!r||!b)return 0;
    uint32_t c=(uint32_t)(uintptr_t)b-1;draw_count++;
    for(int y=r->top;y<r->bottom;y++)for(int x=r->left;x<r->right;x++){
        uint32_t *d=cell(h,x,y); if(d)*d=c;
    }
    return 1;
}
W32ABI int32_t FrameRect(W32_HDC h,const W32_RECT *r,W32_HBRUSH b){
    if(!h||!r||!b)return 0;
    draw_count++;return 1;
}
static W32_DWORD text_color=0x111111,mode=2;
W32ABI W32_DWORD SetTextColor(W32_HDC h,W32_DWORD c){
    if (!h) return 0xffffffffu;
    W32_DWORD old=text_color; text_color=c;return old;
}
W32ABI int32_t SetBkMode(W32_HDC h,int32_t m){
    if (!h) return 0;
    int32_t old=(int32_t)mode;mode=(W32_DWORD)m;return old;
}
W32ABI int DrawTextW(W32_HDC h,const uint16_t *t,int32_t len,W32_RECT *r,W32_UINT flags){
    if(!h||!t||!r||len==0)return 0;
    if(!(flags&W32_DT_CALCRECT)){
        uint32_t *d=cell(h,r->left,r->top);if(d)*d=text_color;
    }
    return 14;
}
W32ABI W32_HGDIOBJ GetCurrentObject(W32_HDC h,W32_UINT t){
    return h&&t==W32_OBJ_FONT ? (W32_HGDIOBJ)(uintptr_t)0x1000 : NULL;
}
W32ABI W32_HGDIOBJ GetStockObject(int idx){
    return idx==W32_DEFAULT_GUI_FONT ? (W32_HGDIOBJ)(uintptr_t)0x1000 : NULL;
}
W32ABI int32_t GetObjectW(W32_HGDIOBJ h,int32_t cb,void *buf){
    if(h!=(W32_HGDIOBJ)(uintptr_t)0x1000||cb<(int32_t)sizeof(W32_LOGFONTW))return 0;
    memset(buf,0,sizeof(W32_LOGFONTW));
    ((W32_LOGFONTW *)buf)->lfHeight=14;return sizeof(W32_LOGFONTW);
}
#include "../../w32/src/uxtheme.c"
static int n,f;
#define CHECK(x) do { ++n; if (!(x)) { ++f; fprintf(stderr,"FAIL:%d: %s\n",__LINE__,#x); } } while(0)
int main(void){
    uint16_t button[]={'B','u','t','t','o','n',0},other[]={'M','E','N','U',0};
    uint16_t edit[]={'E','d','i','t',0}, tab[]={'T','A','B',0};
    uint16_t progress[]={'P','r','o','g','r','e','s','s',0};
    uint16_t combo[]={'C','o','m','b','o','B','o','x',0};
    uint16_t blank[]={0},text[]={'T','e','x','t',0};
    W32_RECT r={2,2,12,12},content={0},clip={3,3,9,9};
    CHECK(OpenThemeData(NULL,other)==NULL && error==W32_ERROR_NOT_SUPPORTED);
    v6=5;CHECK(OpenThemeData(NULL,button)==NULL);
    v6=6;
    void *h=OpenThemeData(NULL,button);
    CHECK(h!=NULL);
    CHECK(DrawThemeBackground(h,(W32_HDC)1,1,1,&r,NULL)==W32_THEME_S_OK);
    uint32_t normal=pixels[5][5];
    CHECK(normal!=0 && draw_count==2);
    CHECK(DrawThemeBackground(h,(W32_HDC)1,1,2,&r,NULL)==W32_THEME_S_OK);
    CHECK(pixels[5][5]!=normal);
    int old=draw_count;
    CHECK(DrawThemeBackground(h,(W32_HDC)1,77,1,&r,NULL)==W32_THEME_E_NOTIMPL);
    CHECK(draw_count==old);
    CHECK(GetThemeBackgroundContentRect(h,(W32_HDC)1,1,2,&r,&content)==W32_THEME_S_OK);
    CHECK(content.left==4 && content.top==4 && content.right==10 && content.bottom==10);
    memset(pixels,0,sizeof pixels);
    CHECK(DrawThemeBackground(h,(W32_HDC)1,1,3,&r,&clip)==W32_THEME_S_OK);
    CHECK(pixels[5][5]!=0 && pixels[2][2]==0 && pixels[10][10]==0);
    W32_SIZE sz={0};W32_LOGFONTW lf={0};W32_DWORD dur=99;
    CHECK(GetThemePartSize(h,(W32_HDC)1,1,2,&r,W32_THEME_TS_DRAW,&sz)==W32_THEME_S_OK && sz.cx==10 && sz.cy==10);
    CHECK(GetThemePartSize(h,(W32_HDC)1,1,2,NULL,W32_THEME_TS_TRUE,&sz)==W32_THEME_S_OK && sz.cx==18);
    CHECK(GetThemeFont(h,(W32_HDC)1,1,2,W32_THEME_TMT_FONT,&lf)==W32_THEME_S_OK && lf.lfHeight==14);
    CHECK(GetThemeTransitionDuration(h,1,1,2,W32_THEME_TMT_TRANSITIONDURATIONS,&dur)==W32_THEME_S_OK && dur==0);
    CHECK(GetThemeTransitionDuration(h,77,1,2,6000,&dur)==W32_THEME_E_NOTIMPL);
    W32_DTTOPTS opts={.dwSize=sizeof opts,.dwFlags=W32_THEME_DTT_TEXTCOLOR,.crText=0x00abcdef};
    CHECK(DrawThemeTextEx(h,(W32_HDC)1,1,1,text,-1,0,&r,&opts)==W32_THEME_S_OK);
    CHECK(pixels[2][2]==opts.crText && text_color==0x111111 && mode==2);
    opts.dwFlags=0x800;
    CHECK(DrawThemeTextEx(h,(W32_HDC)1,1,1,text,-1,0,&r,&opts)==W32_THEME_E_NOTIMPL);
    opts.dwSize=0;
    CHECK(DrawThemeTextEx(h,(W32_HDC)1,1,1,text,-1,0,&r,&opts)==W32_THEME_E_INVALIDARG);
    CHECK(DrawThemeParentBackground(win,(W32_HDC)1,NULL)==W32_THEME_S_OK);
    CHECK(pixels[0][0]!=0);
    CHECK(DrawThemeParentBackground(NULL,(W32_HDC)1,NULL)==W32_THEME_E_HANDLE);
    void *e=OpenThemeData(win,edit),*t=OpenThemeData(win,tab);
    void *p=OpenThemeData(win,progress),*c=OpenThemeData(win,combo);
    CHECK(e&&t&&p&&c);
    CHECK(DrawThemeBackground(e,(W32_HDC)1,1,7,&r,NULL)==W32_THEME_S_OK);
    CHECK(DrawThemeBackground(t,(W32_HDC)1,1,4,&r,NULL)==W32_THEME_S_OK);
    CHECK(DrawThemeBackground(p,(W32_HDC)1,3,0,&r,NULL)==W32_THEME_S_OK);
    CHECK(pixels[5][5]!=normal);
    CHECK(DrawThemeBackground(c,(W32_HDC)1,1,2,&r,NULL)==W32_THEME_S_OK);
    CHECK(DrawThemeBackground(c,(W32_HDC)1,88,1,&r,NULL)==W32_THEME_E_NOTIMPL);
    CHECK(CloseThemeData(e)==W32_THEME_S_OK);
    CHECK(CloseThemeData(t)==W32_THEME_S_OK);
    CHECK(CloseThemeData(p)==W32_THEME_S_OK);
    CHECK(CloseThemeData(c)==W32_THEME_S_OK);
    CHECK(EnableThemeDialogTexture(win,6)==W32_THEME_S_OK);
    CHECK(EnableThemeDialogTexture(win,7)==W32_THEME_E_INVALIDARG);
    void *wh=OpenThemeData(win,button);
    CHECK(wh!=NULL);
    CHECK(SetWindowTheme(win,blank,NULL)==W32_THEME_S_OK && theme_changes==1);
    CHECK(CloseThemeData(wh)==W32_THEME_E_HANDLE);
    CHECK(OpenThemeData(win,button)==NULL);
    CHECK(SetWindowTheme(win,NULL,edit)==W32_THEME_S_OK && theme_changes==2);
    wh=OpenThemeData(win,button);
    CHECK(wh && GetThemeBackgroundContentRect(wh,NULL,1,7,&r,&content)==W32_THEME_S_OK);
    w32_theme_window_destroyed(win);
    CHECK(CloseThemeData(wh)==W32_THEME_E_HANDLE);
    wh=OpenThemeData(win,button); /* override did not leak */
    CHECK(wh!=NULL);
    CHECK(CloseThemeData(wh)==W32_THEME_S_OK);
    CHECK(SetWindowTheme(NULL,NULL,NULL)==W32_THEME_E_HANDLE);
    CHECK(CloseThemeData(h)==W32_THEME_S_OK);
    CHECK(CloseThemeData(h)==W32_THEME_E_HANDLE);
    CHECK(DrawThemeBackground(h,(W32_HDC)1,1,1,&r,NULL)==W32_THEME_E_HANDLE);

    /* Every advertised class/part/state paints pixels; every adjacent
     * unsupported state and part refuses without modifying the DC. */
    struct { const uint16_t *name; int first, last, min_state, max_state; } parts[] = {
        {button,1,1,1,5}, {edit,1,1,1,7}, {tab,1,1,1,4},
        {progress,1,4,0,0}, {combo,1,1,1,4}
    };
    for (size_t i=0;i<sizeof parts/sizeof parts[0];++i) {
        void *th=OpenThemeData(win,parts[i].name);
        CHECK(th!=NULL);
        for (int part=parts[i].first;part<=parts[i].last;++part)
            for (int state=parts[i].min_state;state<=parts[i].max_state;++state) {
                memset(pixels,0,sizeof pixels);
                CHECK(DrawThemeBackground(th,(W32_HDC)1,part,state,&r,NULL)==W32_THEME_S_OK);
                CHECK(pixels[5][5]!=0);
                W32_RECT inner={0};W32_SIZE part_size={0};W32_DWORD duration=123;
                CHECK(GetThemeBackgroundContentRect(th,(W32_HDC)1,part,state,&r,&inner)==W32_THEME_S_OK &&
                      inner.left>=r.left && inner.right<=r.right);
                CHECK(GetThemePartSize(th,(W32_HDC)1,part,state,NULL,W32_THEME_TS_TRUE,&part_size)==W32_THEME_S_OK &&
                      part_size.cx>0 && part_size.cy>0);
                CHECK(GetThemeTransitionDuration(th,part,state,state,
                      W32_THEME_TMT_TRANSITIONDURATIONS,&duration)==W32_THEME_S_OK && duration==0);
            }
        uint32_t before=pixels[5][5];old=draw_count;
        CHECK(DrawThemeBackground(th,(W32_HDC)1,99,parts[i].min_state,&r,NULL)==W32_THEME_E_NOTIMPL);
        CHECK(DrawThemeBackground(th,(W32_HDC)1,parts[i].first,
              parts[i].max_state+1,&r,NULL)==W32_THEME_E_NOTIMPL);
        CHECK(pixels[5][5]==before && draw_count==old);
        CHECK(CloseThemeData(th)==W32_THEME_S_OK);
    }

    CHECK(BufferedPaintInit()==W32_THEME_S_OK);
    W32_HDC from=NULL,to=NULL;
    W32_BP_ANIMATIONPARAMS anim={sizeof anim,0,W32_THEME_BPAS_NONE,0};
    W32_RECT area={0,0,8,8};
    void *buf=BeginBufferedAnimation(win,(W32_HDC)1,&area,0,NULL,&anim,&from,&to);
    CHECK(buf && from && to && from!=to);
    W32_RECT small={1,1,4,4};
    CHECK(FillRect(to,&small,CreateSolidBrush(0x00112233))==1);
    CHECK(EndBufferedAnimation(buf,1)==W32_THEME_S_OK);
    CHECK(pixels[2][2]==0x00112233);
    CHECK(EndBufferedAnimation(buf,1)==W32_THEME_E_HANDLE);
    anim.dwDuration=100;
    CHECK(!BeginBufferedAnimation(win,(W32_HDC)1,&area,0,NULL,&anim,&from,&to) && error==W32_ERROR_NOT_SUPPORTED);
    anim.dwDuration=0;
    buf=BeginBufferedAnimation(win,(W32_HDC)1,&area,0,NULL,&anim,&from,&to);
    CHECK(buf!=NULL);
    CHECK(BufferedPaintStopAllAnimations(win)==W32_THEME_S_OK);
    CHECK(EndBufferedAnimation(buf,1)==W32_THEME_E_HANDLE);

    /* BufferedPaintRenderAnimation: the BOOL sibling of the HRESULT set --
     * blits every live zero-duration frame, TRUE no-op when none is live. */
    error=0;
    CHECK(BufferedPaintRenderAnimation(win,(W32_HDC)1)==1 && error==0);
    error=0;
    CHECK(!BufferedPaintRenderAnimation((W32_HWND)(uintptr_t)0xdead,(W32_HDC)1)
          && error==W32_ERROR_INVALID_HANDLE);
    error=0;
    CHECK(!BufferedPaintRenderAnimation(win,NULL) && error==W32_ERROR_INVALID_PARAMETER);
    buf=BeginBufferedAnimation(win,(W32_HDC)1,&area,0,NULL,&anim,&from,&to);
    CHECK(buf!=NULL);
    W32_RECT whole={0,0,8,8};
    CHECK(FillRect(to,&whole,CreateSolidBrush(0x00a5b6c7))==1);
    memset(pixels,0,sizeof pixels);
    error=0;
    CHECK(BufferedPaintRenderAnimation(win,(W32_HDC)1)==1 && error==0);
    CHECK(pixels[2][2]==0x00a5b6c7 && pixels[7][7]==0x00a5b6c7 && pixels[8][8]==0);
    /* A failed blit (dead target DC in the mock grid) is FALSE, not silence. */
    error=0;
    CHECK(!BufferedPaintRenderAnimation(win,(W32_HDC)(uintptr_t)100)
          && error==W32_ERROR_INVALID_HANDLE);
    memset(pixels,0,sizeof pixels);
    CHECK(EndBufferedAnimation(buf,0)==W32_THEME_S_OK);
    CHECK(BufferedPaintRenderAnimation(win,(W32_HDC)1)==1);
    CHECK(pixels[2][2]==0);
    CHECK(BufferedPaintUnInit()==W32_THEME_S_OK);
    CHECK(!BeginBufferedAnimation(win,(W32_HDC)1,&area,0,NULL,&anim,&from,&to));
    for(int i=0;i<32;i++)CHECK(allocated[i]==NULL);
    for(int i=2;i<16;i++)CHECK(contexts[i]==NULL);
    fprintf(stderr,"w32a11-theme: %d checks, %d failures\n",n,f);
    return f?1:0;
}
