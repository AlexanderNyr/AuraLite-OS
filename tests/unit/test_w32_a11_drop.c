/* W32A-11 owned HDROP lifecycle. Host uses a REAL temporary path/file.
 * This is not a compositor-to-WM_DROPFILES integration claim. */
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#define AURALITE_W32_HOST_TEST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include "w32/shell32.h"
#include "w32/w32_errno.h"
static W32_DWORD last_error;
void w32_set_last_error(W32_DWORD e) { last_error=e; }
#include "../../w32/src/shell32.c"
static int n,f;
#define CHECK(x) do { ++n; if (!(x)) { ++f; fprintf(stderr,"FAIL:%d: %s\n",__LINE__,#x); } } while(0)
int main(void) {
    char path[]="/tmp/w32a11-drop-XXXXXX";
    int fd=mkstemp(path);
    CHECK(fd>=0);
    if (fd<0) return 1;
    CHECK(write(fd,"REAL-CONTENT",12)==12);
    close(fd);
    const char *paths[]={path,"/tmp/independent-second"};
    const char *other[]={"/tmp/other-path"};
    W32_POINT p={17,23}, q={2,4}, out={0};
    W32_HANDLE a=w32_shell_drop_create(paths,2,p,1);
    W32_HANDLE b=w32_shell_drop_create(other,1,q,0);
    CHECK(a!=NULL && b!=NULL && a!=b);
    CHECK(DragQueryFileW(a,0xffffffffu,NULL,0)==2);
    CHECK(DragQueryFileW(b,0xffffffffu,NULL,0)==1);
    uint16_t wide[260];
    W32_UINT len=DragQueryFileW(a,0,wide,260);
    CHECK(len==strlen(path) && wide[len]==0);
    char got[260]; for (unsigned i=0;i<=len;i++) got[i]=(char)wide[i];
    fd=open(got,O_RDONLY);
    CHECK(fd>=0);
    char content[20]={0}; if(fd>=0){CHECK(read(fd,content,12)==12);close(fd);}
    CHECK(!memcmp(content,"REAL-CONTENT",12));
    CHECK(DragQueryFileW(a,0,NULL,0)==len);
    uint16_t shortbuf[4];
    CHECK(DragQueryFileW(a,0,shortbuf,4)==3 && shortbuf[3]==0);
    CHECK(DragQueryPoint(a,&out)==1 && out.x==17 && out.y==23);
    CHECK(DragQueryPoint(b,&out)==0 && out.x==2 && out.y==4);
    DragFinish(a);
    last_error=0;
    CHECK(DragQueryFileW(a,0xffffffffu,NULL,0)==0 && last_error==W32_ERROR_INVALID_HANDLE);
    CHECK(DragQueryFileW(b,0xffffffffu,NULL,0)==1);
    W32_HANDLE c=w32_shell_drop_create(other,1,q,1);
    CHECK(c!=NULL && c!=a); /* generation protects recycled slot */
    DragFinish(b); DragFinish(c);
    CHECK(DragQueryFileW(NULL,0xffffffffu,NULL,0)==0);
    CHECK(w32_shell_drop_create(NULL,1,p,1)==NULL);
    CHECK(DragQueryPoint(NULL,&out)==0 && out.x==0 && out.y==0);
    CHECK(unlink(path)==0);
    fprintf(stderr,"w32a11-drop: %d checks, %d failures\n",n,f);
    return f?1:0;
}
