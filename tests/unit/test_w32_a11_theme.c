/* W32A-11 BUTTON part effects on a real-sized mocked GDI bitmap. */
#define AURALITE_W32_HOST_TEST 1
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "w32/uxtheme.h"
#include "w32/comctl32.h"
#include "w32/w32_errno.h"
static int v6=6,draw_count;
static uint32_t pixels[16][16];
static w32_comctl_palette_t palette={0x00E0E0E0,0,0x00555555,0x00AA3311,0x000011AA};
static W32_DWORD error;
void w32_set_last_error(W32_DWORD e){error=e;}
int w32_comctl_version(void){return v6;}
const w32_comctl_palette_t *w32_comctl_palette(void){return &palette;}
W32ABI W32_HBRUSH CreateSolidBrush(W32_DWORD c){return (W32_HBRUSH)(uintptr_t)(c+1);}
W32ABI W32_BOOL DeleteObject(void *h){return h!=NULL;}
W32ABI int32_t FillRect(W32_HDC dc, const W32_RECT *r, W32_HBRUSH b){
    if (!dc || !r || !b) return 0;
    uint32_t c=(uint32_t)(uintptr_t)b-1; draw_count++;
    for (int y=r->top;y<r->bottom;y++) for (int x=r->left;x<r->right;x++)
        if (x>=0 && x<16 && y>=0 && y<16) pixels[y][x]=c;
    return 1;
}
W32ABI int32_t FrameRect(W32_HDC dc, const W32_RECT *r, W32_HBRUSH b){
    if (!dc || !r || !b) return 0;
    draw_count++; return 1;
}
#include "../../w32/src/uxtheme.c"
static int n,f;
#define CHECK(x) do { ++n; if (!(x)) { ++f; fprintf(stderr,"FAIL:%d: %s\n",__LINE__,#x); } } while(0)
int main(void){
    uint16_t button[]={'B','u','t','t','o','n',0}, other[]={'M','E','N','U',0};
    W32_RECT r={2,2,12,12}, content={0}, clip={3,3,9,9};
    CHECK(OpenThemeData(NULL,other)==NULL && error==W32_ERROR_NOT_SUPPORTED);
    v6=5;
    CHECK(OpenThemeData(NULL,button)==NULL);
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
    CHECK(CloseThemeData(h)==W32_THEME_S_OK);
    CHECK(CloseThemeData(h)==W32_THEME_E_HANDLE);
    CHECK(DrawThemeBackground(h,(W32_HDC)1,1,1,&r,NULL)==W32_THEME_E_HANDLE);
    fprintf(stderr,"w32a11-theme: %d checks, %d failures\n",n,f);
    return f?1:0;
}
