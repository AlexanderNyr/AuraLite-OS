/* W32A-11 A/W named clipboard IDs: identity, capacity, error hygiene. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "w32/user32.h"
#include "w32/w32_errno.h"
static W32_DWORD error;
void w32_set_last_error(W32_DWORD e) { error=e; }
#include "../../w32/src/w32_clipfmt.c"
static int n,f;
#define CHECK(x) do { ++n; if (!(x)) { ++f; fprintf(stderr,"FAIL:%d: %s\n",__LINE__,#x); } } while(0)
int main(void) {
    uint16_t w[]={'F','I','L','E','N','A','M','E','W',0};
    W32_UINT id=RegisterClipboardFormatA("FileNameW");
    CHECK(id==0xC000u);
    CHECK(RegisterClipboardFormatW(w)==id);
    CHECK(RegisterClipboardFormatA("FILENAMEW")==id);
    W32_UINT other=RegisterClipboardFormatA("FileContents");
    CHECK(other!=id && other==0xC001u);
    CHECK(RegisterClipboardFormatA(NULL)==0 && error==W32_ERROR_INVALID_PARAMETER);
    CHECK(RegisterClipboardFormatW(NULL)==0 && error==W32_ERROR_INVALID_PARAMETER);
    uint16_t malformed[]={0xD800,0};
    CHECK(RegisterClipboardFormatW(malformed)==0 && error==W32_ERROR_INVALID_PARAMETER);
    char name[40];
    for (int i=2;i<256;i++) {
        snprintf(name,sizeof name,"W32A11-%d",i);
        CHECK(RegisterClipboardFormatA(name)==(W32_UINT)(0xC000+i));
    }
    CHECK(RegisterClipboardFormatA("overflow")==0 && error==W32_ERROR_NOT_ENOUGH_MEMORY);
    CHECK(RegisterClipboardFormatA("FileNameW")==id); /* lookup even when full */
    fprintf(stderr,"w32a11-clipfmt: %d checks, %d failures\n",n,f);
    return f?1:0;
}
