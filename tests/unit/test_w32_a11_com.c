/* W32A-11 in-process COM init depth + task allocator, multi-threaded host. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include "w32/ole32.h"
#include "w32/w32_errno.h"
static _Thread_local W32_DWORD tid;
W32ABI W32_DWORD GetCurrentThreadId(void) { return tid; }
static W32_DWORD error;
void w32_set_last_error(W32_DWORD code) { error=code; }
#include "../../w32/src/ole32.c"
static volatile int n,f;
#define CHECK(x) do { __sync_fetch_and_add(&n,1); if (!(x)) { __sync_fetch_and_add(&f,1); fprintf(stderr,"FAIL:%d: %s\n",__LINE__,#x); } } while(0)
static void *worker(void *arg) {
    tid=(W32_DWORD)(uintptr_t)arg;
    CHECK(CoInitialize(NULL)==W32_COM_S_OK);
    CHECK(OleInitialize(NULL)==W32_COM_S_FALSE);
    OleUninitialize(); CoUninitialize();
    return NULL;
}
int main(void) {
    tid=1;
    CHECK(CoInitialize((void *)1)==W32_COM_E_INVALIDARG);
    CHECK(CoInitialize(NULL)==W32_COM_S_OK);
    CHECK(CoInitialize(NULL)==W32_COM_S_FALSE);
    CHECK(OleInitialize(NULL)==W32_COM_S_FALSE);
    CHECK(OleInitialize(NULL)==W32_COM_S_FALSE);
    void *p=CoTaskMemAlloc(16);
    CHECK(p!=NULL);
    if (p) {
        ((char *)p)[15]='m';
        CHECK(((char *)p)[15]=='m');
        void *q=CoTaskMemRealloc(p,32);
        CHECK(q!=NULL && ((char *)q)[15]=='m');
        CoTaskMemFree(q);
    }
    void *zero=CoTaskMemAlloc(0);
    CHECK(zero!=NULL); CoTaskMemFree(zero);
    OleUninitialize(); OleUninitialize(); CoUninitialize(); CoUninitialize();
    OleUninitialize(); CoUninitialize(); /* underflow must not corrupt next */
    CHECK(CoInitialize(NULL)==W32_COM_S_OK);
    CoUninitialize();
    pthread_t threads[64];
    for (uintptr_t i=0;i<64;i++) CHECK(pthread_create(&threads[i],NULL,worker,(void *)(i+2))==0);
    for (int i=0;i<64;i++) pthread_join(threads[i],NULL);
    CHECK(CoInitialize(NULL)==W32_COM_S_OK);
    CoUninitialize();
    fprintf(stderr,"w32a11-com: %d checks, %d failures\n",n,f);
    return f?1:0;
}
