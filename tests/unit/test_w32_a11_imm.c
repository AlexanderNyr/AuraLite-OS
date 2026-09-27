/* W32A-11: all ten typed IME refusals; compiler sanitizers enabled. */
#include <stdio.h>
#include <stdint.h>
#include "w32/w32_errno.h"
#include "w32/imm32.h"
static W32_DWORD error;
void w32_set_last_error(W32_DWORD e) { error=e; }
static int n,f;
#define CHECK(x) do { ++n; if (!(x)) { ++f; fprintf(stderr,"FAIL:%d: %s\n",__LINE__,#x); } } while(0)
#include "../../w32/src/imm32.c"
#define REFUSE(x) do { error=0; CHECK(!(x)); CHECK(error==W32_ERROR_NOT_SUPPORTED); } while(0)
int main(void) {
    uint32_t buffer=0x87654321u;
    REFUSE(ImmGetContext(NULL));
    REFUSE(ImmReleaseContext(NULL,NULL));
    REFUSE(ImmGetCompositionStringW(NULL,0,&buffer,sizeof buffer));
    CHECK(buffer==0x87654321u);
    REFUSE(ImmSetCompositionWindow(NULL,&buffer));
    REFUSE(ImmSetCompositionFontA(NULL,&buffer));
    REFUSE(ImmSetCompositionFontW(NULL,&buffer));
    REFUSE(ImmSetCandidateWindow(NULL,&buffer));
    REFUSE(ImmSetCompositionStringW(NULL,0,&buffer,4,&buffer,4));
    REFUSE(ImmEscapeW(NULL,NULL,0,&buffer));
    REFUSE(ImmNotifyIME(NULL,0,0,0));
    fprintf(stderr,"w32a11-imm: %d checks, %d failures\n",n,f);
    return f?1:0;
}
