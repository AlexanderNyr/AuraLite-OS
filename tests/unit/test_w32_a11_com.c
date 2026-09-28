/* W32A-11 in-process COM init depth + task allocator, multi-threaded host. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include "w32/ole32.h"
#include "w32/w32_errno.h"
#include "w32/advapi32.h"
static _Thread_local W32_DWORD tid;
W32ABI W32_DWORD GetCurrentThreadId(void) { return tid; }
static W32_DWORD error;
void w32_set_last_error(W32_DWORD code) { error=code; }
/* Activation's hive access is the probe gate's concern (test_w32_a11_probe
 * amalgamates the real advapi32); the init-depth tests here only need the
 * symbols to resolve, with the always-miss result the empty hive gives. */
W32_LONG W32ABI RegOpenKeyExW(W32_HKEY key, const uint16_t *subkey,
                              W32_ULONG reserved, W32_ULONG sam, W32_HKEY *out) {
    (void)key; (void)subkey; (void)reserved; (void)sam; (void)out;
    return 2; /* ERROR_FILE_NOT_FOUND: no ProgID is registered here */
}
W32_LONG W32ABI RegQueryValueExW(W32_HKEY key, const uint16_t *name,
                                 W32_ULONG *reserved, W32_DWORD *type,
                                 uint8_t *data, W32_DWORD *len) {
    (void)key; (void)name; (void)reserved; (void)type; (void)data; (void)len;
    return 2;
}
W32_LONG W32ABI RegCloseKey(W32_HKEY key) { (void)key; return 0; }
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
