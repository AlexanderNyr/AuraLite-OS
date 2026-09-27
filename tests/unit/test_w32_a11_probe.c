/* W32A-11: generated CoCreateInstance/CLSIDFromProgID TODO probes must be
 * lossless, bounded and fail-clean. These GUIDs/ProgIDs are synthetic test
 * vectors, NOT observations from any pinned application. Apache-2.0. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "w32/w32_errno.h"
#include "w32/kernel32.h"

static W32_DWORD last_error;
void w32_set_last_error(W32_DWORD e) { last_error = e; }
W32ABI void ExitProcess(W32_UINT code) { (void)code; abort(); }
/* Generated from the committed TSV: test the code the actual PE binder uses. */
#include "../../w32/src/w32_stubs_gen.c"

static int n, failures;
#define CHECK(x) do { ++n; if (!(x)) { ++failures; \
    fprintf(stderr,"FAIL:%d: %s\n",__LINE__,#x); } } while (0)

static int count(const char *s, const char *part) {
    int hits=0;
    while ((s=strstr(s,part))!=NULL) { ++hits; s+=strlen(part); }
    return hits;
}

int main(void) {
    uint8_t clsid[16]={0x78,0x56,0x34,0x12,0xbc,0x9a,0xf0,0xde,
                       0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
    uint8_t iid[16]={0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
    char guid[37];
    probe_guid(clsid,guid);
    CHECK(strcmp(guid,"12345678-9abc-def0-1122-334455667788")==0);
    probe_guid(iid,guid);
    CHECK(strcmp(guid,"00000000-0000-0000-c000-000000000046")==0);
    probe_guid(NULL,guid);
    CHECK(strcmp(guid,"(null)")==0);

    uint16_t progid[]={'N','P','P','.',0x00e9,0xd840,0xdc00,'\\','\n','=',' ',0};
    char name[W32A11_PROGID_MAX_UNITS*6u+1u];
    CHECK(probe_progid(progid,name)==0);
    CHECK(strcmp(name,"NPP.\\u00e9\\ud840\\udc00\\u005c\\u000a\\u003d\\u0020")==0);
    CHECK(probe_progid(NULL,name)==0 && !strcmp(name,"(null)"));
    uint16_t long_id[W32A11_PROGID_MAX_UNITS+2u];
    for (unsigned i=0;i<W32A11_PROGID_MAX_UNITS+1u;i++) long_id[i]='x';
    long_id[W32A11_PROGID_MAX_UNITS+1u]=0;
    CHECK(probe_progid(long_id,name)==1);
    CHECK(strlen(name)==W32A11_PROGID_MAX_UNITS);
    long_id[W32A11_PROGID_MAX_UNITS]=0;
    CHECK(probe_progid(long_id,name)==0); /* exact bound is complete */

    /* One line per invocation, not a first-call-only print. Capture stdout
     * to assert the actual generated stubs' format and truncation markers. */
    int saved=dup(STDOUT_FILENO);
    FILE *cap=tmpfile();
    CHECK(saved>=0 && cap!=NULL);
    if (saved<0 || !cap) return 1;
    fflush(stdout);
    CHECK(dup2(fileno(cap),STDOUT_FILENO)>=0);
    void *out=(void *)(uintptr_t)0x1234;
    last_error=0;
    CHECK(w32_stub_ole32_CoCreateInstance(clsid,NULL,1,iid,&out)==W32_E_NOTIMPL);
    CHECK(out==NULL && last_error==W32_ERROR_NOT_SUPPORTED);
    out=(void *)(uintptr_t)0x1234;
    CHECK(w32_stub_ole32_CoCreateInstance(clsid,NULL,4,NULL,&out)==W32_E_NOTIMPL);
    CHECK(out==NULL);
    uint8_t out_guid[16]; memset(out_guid,0xa5,sizeof out_guid);
    last_error=0;
    CHECK(w32_stub_ole32_CLSIDFromProgID(progid,out_guid)==W32_E_NOTIMPL);
    uint8_t zeros[16]={0};
    CHECK(!memcmp(out_guid,zeros,sizeof out_guid) && last_error==W32_ERROR_NOT_SUPPORTED);
    CHECK(w32_stub_ole32_CLSIDFromProgID(long_id,NULL)==W32_E_NOTIMPL);
    long_id[W32A11_PROGID_MAX_UNITS]='x';
    CHECK(w32_stub_ole32_CLSIDFromProgID(long_id,NULL)==W32_E_NOTIMPL);
    fflush(stdout);
    CHECK(dup2(saved,STDOUT_FILENO)>=0);
    close(saved);
    rewind(cap);
    char log[4096]={0};
    size_t size=fread(log,1,sizeof log-1,cap);
    CHECK(size>0 && !ferror(cap));
    fclose(cap);
    CHECK(strstr(log,"CLSID=12345678-9abc-def0-1122-334455667788 IID=00000000-0000-0000-c000-000000000046 CLSCTX=1")!=NULL);
    CHECK(strstr(log,"IID=(null) CLSCTX=4")!=NULL);
    CHECK(count(log,"w32a11-clsid-probe:")==2);
    CHECK(strstr(log,"w32a11-progid-probe: UTF16=NPP.\\u00e9\\ud840\\udc00\\u005c\\u000a\\u003d\\u0020 truncated=0")!=NULL);
    CHECK(strstr(log,"truncated=1")!=NULL);
    CHECK(count(log,"w32a11-progid-probe:")==3);
    CHECK(count(log,"TODO ole32.dll!CoCreateInstance")==1);
    fprintf(stderr,"w32a11-probe: %d checks, %d failures\n",n,failures);
    return failures?1:0;
}
