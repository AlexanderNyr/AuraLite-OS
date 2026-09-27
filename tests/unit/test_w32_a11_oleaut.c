/* W32A-11 BSTR/VARIANT byte ownership and COM refcounts, ASan/UBSan. */
#define AURALITE_W32_HOST_TEST 1
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "w32/oleaut32.h"
#include "../../w32/src/w32_oleaut32.c"
static int n,f;
#define CHECK(x) do { ++n; if (!(x)) { ++f; fprintf(stderr,"FAIL:%d: %s\n",__LINE__,#x); } } while(0)
typedef struct { void **vtable; int refs, added, released; } mock_iunk;
static W32_DWORD W32ABI addref(void *p) {
    mock_iunk *o=p; o->added++; return (W32_DWORD)++o->refs;
}
static W32_DWORD W32ABI release(void *p) {
    mock_iunk *o=p; o->released++; return (W32_DWORD)--o->refs;
}
int main(void) {
    static const uint16_t embedded[]={'A',0,'B',0};
    W32_BSTR b=SysAllocStringLen(embedded,3);
    CHECK(b && SysStringLen(b)==3 && SysStringByteLen(b)==6);
    CHECK(b[0]=='A' && b[1]==0 && b[2]=='B' && b[3]==0);
    W32_VARIANT src={0},dst={0};
    src.vt=W32_VT_BSTR; src.u.ptr=b;
    CHECK(VariantCopy(&dst,&src)==W32_S_OK);
    CHECK(dst.u.ptr && dst.u.ptr!=b && ((uint16_t *)dst.u.ptr)[2]=='B');
    CHECK(VariantCopy(&src,&src)==W32_S_OK && src.u.ptr==b);
    CHECK(VariantClear(&src)==W32_S_OK && src.vt==W32_VT_EMPTY);
    CHECK(VariantClear(&dst)==W32_S_OK && dst.u.ptr==NULL);
    CHECK(VariantClear(NULL)==W32_E_INVALIDARG);
    CHECK(VariantCopy(NULL,&src)==W32_E_INVALIDARG);
    static const char odd[3]={'x',0,'z'};
    src.vt=W32_VT_BSTR; src.u.ptr=SysAllocStringByteLen(odd,3);
    CHECK(src.u.ptr && SysStringByteLen((W32_BSTR)src.u.ptr)==3);
    CHECK(VariantCopy(&dst,&src)==W32_S_OK);
    CHECK(SysStringByteLen((W32_BSTR)dst.u.ptr)==3);
    CHECK(!memcmp(src.u.ptr,dst.u.ptr,3));
    CHECK(VariantClear(&src)==W32_S_OK && VariantClear(&dst)==W32_S_OK);
    src.vt=W32_VT_I4; src.u.lval=-1242;
    CHECK(VariantCopy(&dst,&src)==W32_S_OK);
    CHECK(dst.vt==W32_VT_I4 && dst.u.lval==-1242);
    CHECK(VariantClear(&dst)==W32_S_OK && dst.u.u64==0);
    src.vt=W32_VT_BOOL; src.u.lval=-1;
    CHECK(VariantCopy(&dst,&src)==W32_S_OK && dst.u.lval==-1);
    CHECK(VariantClear(&dst)==W32_S_OK);
    dst.vt=W32_VT_I4; dst.u.lval=666;
    src.vt=W32_VT_ARRAY|W32_VT_BSTR; src.u.ptr=NULL;
    CHECK(VariantCopy(&dst,&src)==W32_E_NOTIMPL);
    CHECK(dst.vt==W32_VT_I4 && dst.u.lval==666);
    CHECK(VariantClear(&src)==W32_E_NOTIMPL && src.vt==(W32_VT_ARRAY|W32_VT_BSTR));
    VariantInit(&src);
    CHECK(VariantClear(&dst)==W32_S_OK);
    void *vtable[3]={NULL,(void *)addref,(void *)release};
    mock_iunk o={vtable,1,0,0};
    src.vt=W32_VT_UNKNOWN; src.u.ptr=&o;
    CHECK(VariantCopy(&dst,&src)==W32_S_OK && o.refs==2 && o.added==1);
    CHECK(VariantClear(&dst)==W32_S_OK && o.refs==1 && o.released==1);
    CHECK(VariantClear(&src)==W32_S_OK && o.refs==0 && o.released==2);
    o.refs=1;
    src.vt=W32_VT_BYREF|W32_VT_UNKNOWN; src.u.ptr=&o;
    CHECK(VariantCopy(&dst,&src)==W32_S_OK && o.refs==1);
    CHECK(VariantClear(&src)==W32_S_OK && VariantClear(&dst)==W32_S_OK);
    CHECK(o.refs==1 && o.released==2);
    fprintf(stderr,"w32a11-oleaut: %d checks, %d failures\n",n,f);
    return f?1:0;
}
