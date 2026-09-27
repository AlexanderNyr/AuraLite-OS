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

    /* A real UTF-8 filename traverses owned HDROP -> UTF-16 query -> VFS.
     * The suffix has U+00E9 (one unit) and U+20000 (non-emoji surrogate pair). */
    char dir[]="/tmp/w32a11-unicode-XXXXXX";
    CHECK(mkdtemp(dir)!=NULL);
    char unicode[260], original[260];
    CHECK(snprintf(unicode,sizeof unicode,"%s/\xC3\xA9-\xF0\xA0\x80\x80.txt",dir)<(int)sizeof unicode);
    strcpy(original,unicode);
    fd=open(unicode,O_WRONLY|O_CREAT|O_EXCL,0600);
    CHECK(fd>=0);
    if (fd>=0) { CHECK(write(fd,"UTF16-CONTENT",13)==13); close(fd); }
    const char *upaths[]={unicode};
    W32_HANDLE u=w32_shell_drop_create(upaths,1,p,1);
    CHECK(u!=NULL);
    unicode[0]='X'; /* the HDROP owns its own copy */
    size_t units=strlen(original)-3; /* UTF-8 2+4 bytes -> 1+2 units */
    CHECK(DragQueryFileW(u,0,NULL,0)==units);
    uint16_t uw[260]={0};
    CHECK(DragQueryFileW(u,0,uw,260)==units && uw[units]==0);
    CHECK(uw[units-8]==0x00e9 && uw[units-6]==0xd840 && uw[units-5]==0xdc00);
    char decoded[260]={0}; size_t got_bytes=0;
    CHECK(w32_utf16_to_utf8(uw,units,decoded,sizeof decoded-1,&got_bytes)==W32_UTF_OK);
    decoded[got_bytes]=0;
    CHECK(!strcmp(decoded,original));
    fd=open(decoded,O_RDONLY);
    CHECK(fd>=0);
    memset(content,0,sizeof content);
    if (fd>=0) { CHECK(read(fd,content,13)==13); close(fd); }
    CHECK(!memcmp(content,"UTF16-CONTENT",13));
    /* Truncation may not return a dangling high surrogate. */
    uint16_t truncated[260];
    memset(truncated,0xa5,sizeof truncated);
    CHECK(DragQueryFileW(u,0,truncated,(W32_UINT)(units-4))==units-6);
    CHECK(truncated[units-6]==0);
    CHECK(DragQueryFileW(u,0,truncated,1)==0 && truncated[0]==0);
    DragFinish(u);
    CHECK(unlink(original)==0 && rmdir(dir)==0);

    const char *malformed[]={"/tmp/\xc0\xaf"}; /* overlong slash */
    last_error=0;
    CHECK(w32_shell_drop_create(malformed,1,p,1)==NULL &&
          last_error==W32_ERROR_NO_UNICODE_TRANSLATION);
    const char *surrogate[]={"/tmp/\xed\xa0\x80"};
    last_error=0;
    CHECK(w32_shell_drop_create(surrogate,1,p,1)==NULL &&
          last_error==W32_ERROR_NO_UNICODE_TRANSLATION);
    const char *cut[]={"/tmp/\xf0\xa0\x80"};
    last_error=0;
    CHECK(w32_shell_drop_create(cut,1,p,1)==NULL &&
          last_error==W32_ERROR_NO_UNICODE_TRANSLATION);
    /* Legacy borrowed paths also refuse invalid sequences at query time. */
    w32_shell_set_drop_list(malformed,1);
    last_error=0;
    CHECK(DragQueryFileW(NULL,0,NULL,0)==0 &&
          last_error==W32_ERROR_NO_UNICODE_TRANSLATION);
    w32_shell_set_drop_list(NULL,0);
    /* The bounded path limit still accepts its exact maximum. */
    char maxpath[513]; memset(maxpath,'a',511); maxpath[0]='/'; maxpath[511]=0;
    const char *longpaths[]={maxpath};
    W32_HANDLE maxdrop=w32_shell_drop_create(longpaths,1,p,1);
    CHECK(maxdrop!=NULL);
    CHECK(DragQueryFileW(maxdrop,0,NULL,0)==511);
    DragFinish(maxdrop);
    maxpath[511]='a'; maxpath[512]=0;
    last_error=0;
    CHECK(w32_shell_drop_create(longpaths,1,p,1)==NULL &&
          last_error==W32_ERROR_INVALID_PARAMETER);
    CHECK(unlink(path)==0);
    fprintf(stderr,"w32a11-drop: %d checks, %d failures\n",n,f);
    return f?1:0;
}
