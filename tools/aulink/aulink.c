/* tools/aulink/aulink.c -- AuraLite's own ELF linker (SELFHOST_PLAN.md SH3).
 *
 * Links ELF64 x86_64 relocatable objects into a static ELF64 executable
 * following a linker-script subset, so the guest toolchain can link the
 * userland (and, in SH5, the kernel) without ld.lld.
 *
 * Scope (deliberate, documented in SH3):
 *   - ELF64 LE inputs and output only.
 *   - RELA relocations: R_X86_64_64/_32/_32S/_PC32/_PLT32/_16/_8/_PC16/_PC8/
 *     _GOTPCREL (everything clang and tcc emit; no GOT32/TLS/PIE).
 *   - Linker-script subset: OUTPUT_FORMAT/ARCH (ignored), ENTRY(),
 *     NAME = expr;  PHDRS { name PT_LOAD FLAGS(expr); }, SECTIONS {
 *     ". = expr", ". += expr", "sym = expr", "sec ALIGN(n) : { inputs } :ph",
 *     input groups "*(a b.*)", KEEP()/SORT_BY_INIT_PRIORITY() wrappers,
 *     *(COMMON), /DISCARD/, ALIGN(), CONSTANT(MAXPAGESIZE), << >>.
 *   - No --gc-sections (footprint nicety, not semantics -- SH3 D3).
 *   - Simple .a archive support (all ELF members).
 *
 * Host unit test: tests/unit/test_aulink.sh (parity vs ld.lld).
 * In-guest: test_selfhost_aulink.sh compiles this with tcc and links.
 *
 * Portability: plain C99, no GNU extensions, no VLAs, tcc-compatible.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef HAVE_STRNLEN
static size_t aulink_strnlen(const char *s, size_t n){
    size_t i=0; while(i<n&&s[i]) i++; return i;
}
#define strnlen aulink_strnlen
#endif

#define MAX_INPUTS   256
#define MAX_SECTIONS 4096
#define MAX_SYMS     65536
#define MAX_OUT_SECS 64
#define MAX_PHDRS    8
#define MAX_PATTERNS 32
#define MAX_GOT      1024

#define ET_REL 1
#define ET_EXEC 2
#define EM_X86_64 62
#define EV_CURRENT 1
#define ELFCLASS64 2
#define ELFDATA2LSB 1

#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15

#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_MERGE 0x10
#define SHF_STRINGS 0x20
#define SHF_EXECINSTR 0x4

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4

#define STB_GLOBAL 1
#define STB_WEAK 2
#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define SHN_COMMON 0xfff2

#define R_X86_64_64       1
#define R_X86_64_PC32     2
#define R_X86_64_GOTPCREL 9
#define R_X86_64_PLT32    4
#define R_X86_64_32       10
#define R_X86_64_32S      11
#define R_X86_64_16       12
#define R_X86_64_PC16     13
#define R_X86_64_8        14
#define R_X86_64_PC8      15

struct elf64_shdr {
    uint32_t sh_name, sh_type, sh_link, sh_info;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size, sh_addralign, sh_entsize;
};

struct in_obj {
    char *shstrtab; size_t shstrtab_len;
    char *strtab; size_t strtab_len;
    struct elf64_shdr *shdrs; uint16_t shnum;
    uint8_t *file; size_t file_size;
    struct in_sec *secs; int n_secs;
    struct in_sym *syms; uint32_t sym_count;
};

struct in_sec {
    char name[64];
    uint32_t type; uint64_t flags, align, size;
    uint8_t *data; /* into file; NULL for NOBITS */
    int out_idx; /* -1 unassigned, -2 discard, >=0 out */
    uint64_t out_off;
    struct in_obj *obj;
    /* SH5b: SHF_MERGE|SHF_STRINGS support.  A merge section's contents are
     * deduplicated into a per-entsize pool that is placed FIRST in the
     * owning output section; every relocation against the original section
     * is re-based onto the pool via mpool[] (binary-searched by morig[]). */
    uint64_t entsize;
    int is_merge, is_pool;
    struct in_sec *pool;       /* for a merge section: its pool */
    uint32_t *morig, *mpool, *mlen;
    int mcount;
};

struct in_sym {
    char name[64];
    unsigned char info; uint16_t shndx; uint64_t value, addr;
    int defined, is_script, out_sec;
    struct in_obj *obj;
};

struct out_sec {
    char name[64]; uint32_t type; uint64_t flags, align, addr, size, name_off;
    int phdr;
};

struct phdr_def { char name[32]; uint64_t flags; };

struct script {
    char *text; size_t len, pos;
    char tok_text[128]; int tok; uint64_t tok_num;
    char entry[64];
    struct phdr_def phdrs[MAX_PHDRS]; int phdr_count;
};

static struct in_obj objs[MAX_INPUTS]; static int n_objs;
static struct in_sec *all_secs[MAX_SECTIONS]; static int n_secs;
static struct in_sym *all_syms[MAX_SYMS]; static int n_syms;
static struct out_sec out_secs[MAX_OUT_SECS]; static int n_out;
static struct script sc;
static uint64_t cur_addr;
static int errors;

static struct in_sym *got_syms[MAX_GOT]; static int n_got;
static int got_out_idx = -1;

/* ---- ELF helpers ---- */
static uint16_t rd16(const uint8_t *p){return (uint16_t)(p[0]|p[1]<<8);}
static uint32_t rd32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t rd64(const uint8_t *p){return (uint64_t)rd32(p)|(uint64_t)rd32(p+4)<<32;}
static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static void put64(uint8_t *p,uint64_t v){put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32));}

/* ---- script lexer ---- */
static int is_ident_char(char c){
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='.'||c=='$'||c=='*';
}
static int is_dot(void){return sc.tok==1 && strcmp(sc.tok_text,".")==0;}
static int is_sym_c(char c){return sc.tok==3 && sc.tok_text[0]==c && sc.tok_text[1]==0;}

static int next_tok(void){
    const char *t=sc.text; size_t p=sc.pos, n=sc.len;
    while(p<n && (t[p]==' '||t[p]=='\t'||t[p]=='\n'||t[p]=='\r')) p++;
    if(p>=n){sc.tok=0; return 0;}
    char c=t[p];
    if(c=='/'&&p+1<n&&t[p+1]=='/'){p+=2; while(p<n&&t[p]!='\n') p++; sc.pos=p; return next_tok();}
    if(c=='/'&&p+1<n&&t[p+1]=='*'){p+=2; while(p+1<n&&!(t[p]=='*'&&t[p+1]=='/')) p++; sc.pos=(p+2<n)?p+2:n; return next_tok();}
    if(c>='0'&&c<='9'){
        uint64_t v=0;
        if(c=='0'&&p+1<n&&(t[p+1]=='x'||t[p+1]=='X')){
            p+=2; while(p<n){char h=t[p]; int d; if(h>='0'&&h<='9') d=h-'0'; else if(h>='a'&&h<='f') d=h-'a'+10; else if(h>='A'&&h<='F') d=h-'A'+10; else break; v=v*16+(uint64_t)d; p++;}
        }else{while(p<n&&t[p]>='0'&&t[p]<='9'){v=v*10+(uint64_t)(t[p]-'0'); p++;}}
        sc.tok_num=v; sc.tok=2; sc.pos=p; return 2;
    }
    if(is_ident_char(c)){
        size_t s=p; while(p<n&&is_ident_char(t[p])) p++;
        size_t len=p-s; if(len>=sizeof(sc.tok_text)) len=sizeof(sc.tok_text)-1;
        memcpy(sc.tok_text,t+s,len); sc.tok_text[len]=0; sc.tok=1; sc.pos=p; return 1;
    }
    sc.tok_text[0]=c; sc.tok_text[1]=0; sc.tok=3; sc.pos=p+1; return 3;
}
static int expect_sym_c(char c){
    if(!is_sym_c(c)){fprintf(stderr,"aulink: script: expected '%c' got '%s'\n",c,sc.tok_text); errors++; return 0;}
    next_tok(); return 1;
}
static uint64_t expr_value(void);
static uint64_t expr_factor(void){
    if(sc.tok==2){uint64_t v=sc.tok_num; next_tok(); return v;}
    if(is_sym_c('(')){next_tok(); uint64_t v=expr_value(); expect_sym_c(')'); return v;}
    if(sc.tok==1){
        char name[64]; snprintf(name,sizeof name,"%s",sc.tok_text);
        if(strcmp(name,".")==0){next_tok(); return cur_addr;}
        next_tok();
        if(strcmp(name,"ALIGN")==0&&is_sym_c('(')){next_tok(); uint64_t a=expr_value(); expect_sym_c(')'); return (cur_addr+a-1)&~(a-1);}
        if(strcmp(name,"CONSTANT")==0&&is_sym_c('(')){
            next_tok(); char cn[64]; snprintf(cn,sizeof cn,"%s",sc.tok_text); next_tok(); expect_sym_c(')');
            if(strcmp(cn,"MAXPAGESIZE")==0) return 4096;
            fprintf(stderr,"aulink: unknown CONSTANT(%s)\n",cn); errors++; return 0;
        }
        for(int i=0;i<n_syms;i++) if(all_syms[i]->is_script&&all_syms[i]->defined&&strcmp(all_syms[i]->name,name)==0) return all_syms[i]->addr;
        fprintf(stderr,"aulink: unknown identifier '%s'\n",name); errors++; return 0;
    }
    fprintf(stderr,"aulink: bad factor at %zu\n",sc.pos); errors++; return 0;
}
static uint64_t expr_term(void){
    uint64_t v=expr_factor();
    for(;;){
        if(is_sym_c('*')){next_tok(); v*=expr_factor();}
        else if(is_sym_c('/')){next_tok(); uint64_t d=expr_factor(); if(d) v/=d;}
        else if(is_sym_c('<')){next_tok(); if(is_sym_c('<')){next_tok(); v<<=expr_factor();}}
        else if(is_sym_c('>')){next_tok(); if(is_sym_c('>')){next_tok(); v>>=expr_factor();}}
        else break;
    }
    return v;
}
static uint64_t expr_value(void){
    uint64_t v=expr_term();
    for(;;){
        if(is_sym_c('+')){next_tok(); v+=expr_term();}
        else if(is_sym_c('-')){next_tok(); v-=expr_term();}
        else if(is_sym_c('|')){next_tok(); v|=expr_term();}
        else if(is_sym_c('&')){next_tok(); v&=expr_term();}
        else break;
    }
    return v;
}
static struct in_sym *script_sym(const char *name,uint64_t addr){
    for(int i=0;i<n_syms;i++) if(strcmp(all_syms[i]->name,name)==0){
        all_syms[i]->addr=addr; all_syms[i]->defined=1; all_syms[i]->is_script=1; return all_syms[i];
    }
    if(n_syms>=MAX_SYMS){fprintf(stderr,"aulink: too many syms\n"); exit(1);}
    struct in_sym *s=calloc(1,sizeof *s); snprintf(s->name,sizeof s->name,"%s",name);
    s->addr=addr; s->defined=1; s->is_script=1; s->info=(unsigned char)(STB_GLOBAL<<4);
    all_syms[n_syms++]=s; return s;
}
static struct in_sym *find_global(const char *name){
    for(int i=0;i<n_syms;i++) if(strcmp(all_syms[i]->name,name)==0&&all_syms[i]->defined) return all_syms[i];
    return NULL;
}

/* ---- ELF reader ---- */
static const char *sec_name(const struct in_obj *o,uint32_t idx){
    if(!o->shdrs) return ""; uint32_t off=o->shdrs[idx].sh_name;
    if(off>=o->shstrtab_len) return ""; return o->shstrtab+off;
}
static int alloc_index_for_shndx(const struct in_obj *o,uint16_t shndx){
    int acc=0; for(int j=0;j<o->shnum;j++){if(!(o->shdrs[j].sh_flags&SHF_ALLOC)) continue; if(j==(int)shndx) return acc; acc++;} return -1;
}
static int read_object_buf(uint8_t *buf,size_t sz,int idx,const char *path){
    if(sz<64){fprintf(stderr,"aulink: %s too small\n",path); return -1;}
    if(buf[4]!=ELFCLASS64||buf[5]!=ELFDATA2LSB){fprintf(stderr,"aulink: %s: not ELF64 LE\n",path); return -1;}
    if(rd16(buf+16)!=ET_REL){fprintf(stderr,"aulink: %s: not relocatable\n",path); return -1;}
    struct in_obj *o=&objs[idx]; memset(o,0,sizeof *o);
    o->file=buf; o->file_size=sz;
    uint64_t shoff=rd64(buf+40); uint16_t shentsize=rd16(buf+58); uint16_t shnum=rd16(buf+60); uint16_t shstrndx=rd16(buf+62);
    if(shoff==0||shnum==0){o->shnum=0; o->shdrs=NULL; return 0;}
    o->shnum=shnum; o->shdrs=calloc(shnum?shnum:1,sizeof(struct elf64_shdr));
    const uint8_t *sh=buf+shoff;
    for(int i=0;i<shnum;i++){
        const uint8_t *s=sh+(size_t)i*shentsize;
        o->shdrs[i].sh_name=rd32(s); o->shdrs[i].sh_type=rd32(s+4);
        o->shdrs[i].sh_flags=rd64(s+8); o->shdrs[i].sh_addr=rd64(s+16);
        o->shdrs[i].sh_offset=rd64(s+24); o->shdrs[i].sh_size=rd64(s+32);
        o->shdrs[i].sh_link=rd32(s+40); o->shdrs[i].sh_info=rd32(s+44);
        o->shdrs[i].sh_addralign=rd64(s+48); o->shdrs[i].sh_entsize=rd64(s+56);
    }
    if(shstrndx<shnum){o->shstrtab=(char*)(buf+o->shdrs[shstrndx].sh_offset); o->shstrtab_len=(size_t)o->shdrs[shstrndx].sh_size;}
    int n_alloc=0; for(int i=0;i<shnum;i++) if(o->shdrs[i].sh_flags&SHF_ALLOC) n_alloc++;
    o->secs=calloc((size_t)(n_alloc?n_alloc:1),sizeof(struct in_sec));
    o->n_secs=n_alloc; int si=0;
    for(int i=0;i<shnum;i++){
        struct elf64_shdr *h=&o->shdrs[i]; if(!(h->sh_flags&SHF_ALLOC)) continue;
        struct in_sec *sec=&o->secs[si];
        snprintf(sec->name,sizeof sec->name,"%s",sec_name(o,(uint32_t)i));
        sec->type=h->sh_type; sec->flags=h->sh_flags; sec->align=h->sh_addralign?h->sh_addralign:1;
        if(h->sh_flags&SHF_MERGE){
            sec->is_merge=1; sec->entsize=h->sh_entsize?h->sh_entsize:1;
        }
        sec->size=h->sh_size; sec->data=(h->sh_type==SHT_NOBITS)?NULL:(buf+h->sh_offset);
        sec->out_idx=-1; sec->obj=o; sec->out_off=0;
        if(n_secs>=MAX_SECTIONS){fprintf(stderr,"aulink: too many sections\n"); return -1;}
        all_secs[n_secs++]=sec; si++;
    }
    for(int i=0;i<shnum;i++){
        if(o->shdrs[i].sh_type!=SHT_SYMTAB) continue;
        uint32_t count=(uint32_t)(o->shdrs[i].sh_size/o->shdrs[i].sh_entsize);
        uint32_t str_idx=o->shdrs[i].sh_link;
        if(str_idx>= (uint32_t)shnum) continue;
        o->strtab=(char*)(buf+o->shdrs[str_idx].sh_offset);
        o->strtab_len=(size_t)o->shdrs[str_idx].sh_size;
        o->syms=calloc(count?count:1,sizeof(struct in_sym)); o->sym_count=count;
        const uint8_t *st=buf+o->shdrs[i].sh_offset;
        for(uint32_t k=0;k<count;k++){
            const uint8_t *s=st+(size_t)k*o->shdrs[i].sh_entsize;
            uint32_t name_off=rd32(s); struct in_sym *sym=&o->syms[k];
            sym->info=s[4]; sym->shndx=rd16(s+6); sym->value=rd64(s+8); sym->obj=o;
            if((sym->info&0xf)==3){ /* STT_SECTION */ if(sym->shndx<o->shnum) snprintf(sym->name,sizeof sym->name,"%s",sec_name(o,sym->shndx));}
            else if(name_off<o->strtab_len) snprintf(sym->name,sizeof sym->name,"%s",o->strtab+name_off);
            if(n_syms>=MAX_SYMS){fprintf(stderr,"aulink: too many syms\n"); return -1;}
            all_syms[n_syms++]=sym;
        }
        break;
    }
    return 0;
}
static int read_object(const char *path,int idx){
    FILE *f=fopen(path,"rb"); if(!f){fprintf(stderr,"aulink: cannot open %s\n",path); return -1;}
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<64){fclose(f); fprintf(stderr,"aulink: %s too small\n",path); return -1;}
    uint8_t *buf=malloc((size_t)sz); if(!buf){fclose(f); return -1;}
    if(fread(buf,1,(size_t)sz,f)!=(size_t)sz){fclose(f); free(buf); return -1;}
    fclose(f);
    return read_object_buf(buf,(size_t)sz,idx,path);
}
static int load_archive(const char *path,int *p_idx){
    FILE *f=fopen(path,"rb"); if(!f){fprintf(stderr,"aulink: cannot open archive %s\n",path); return -1;}
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<8){fclose(f); return 0;}
    uint8_t *buf=malloc((size_t)sz); if(!buf){fclose(f); return -1;}
    if(fread(buf,1,(size_t)sz,f)!=(size_t)sz){fclose(f); free(buf); return -1;}
    fclose(f);
    if(sz<8||memcmp(buf,"!<arch>\n",8)!=0){free(buf); fprintf(stderr,"aulink: %s: not an archive\n",path); return -1;}
    size_t off=8; int loaded=0;
    while(off+60<=(size_t)sz){
        const uint8_t *hdr=buf+off;
        if(memcmp(hdr+58,"`\n",2)!=0) break;
        char sizebuf[12]; memcpy(sizebuf,hdr+48,10); sizebuf[10]=0;
        long msize=strtol(sizebuf,NULL,10); if(msize<0) break;
        size_t data_off=off+60; size_t data_end=data_off+(size_t)msize;
        if(data_end>(size_t)sz) break;
        const uint8_t *member=buf+data_off;
        if((size_t)msize>=4&&memcmp(member,"\x7f""ELF",4)==0){
            if(*p_idx>=MAX_INPUTS){fprintf(stderr,"aulink: too many inputs\n"); free(buf); return -1;}
            if(read_object_buf((uint8_t*)member,(size_t)msize,*p_idx,path)==0){(*p_idx)++; loaded++;}
        }
        off=data_end; if(off&1) off++;
    }
    /* keep buf alive -- members point into it */
    return loaded;
}

/* ---- layout helpers ---- */
static int pat_match(const char *pat,const char *name){
    size_t pl=strlen(pat); if(pl>=2&&strcmp(pat+pl-2,".*")==0) return strncmp(pat,name,pl-2)==0;
    return strcmp(pat,name)==0;
}
static int discard_section(const char *name){
    static const char *disc[]={".eh_frame",".eh_frame_hdr",".note",".comment",".gnu.hash",".hash",NULL};
    for(int i=0;disc[i];i++) if(strncmp(name,disc[i],strlen(disc[i]))==0) return 1;
    return 0;
}
static int cmp_sec_name(const void *a,const void *b){
    const struct in_sec *sa=*(const struct in_sec *const *)a;
    const struct in_sec *sb=*(const struct in_sec *const *)b;
    return strcmp(sa->name,sb->name);
}
static void parse_input_group(int out_idx,int sort_by_name){
    next_tok(); /* '(' */
    char pats[MAX_PATTERNS][64]; int npat=0;
    while(!is_sym_c(')')&&sc.tok!=0){
        if(sc.tok==1&&npat<MAX_PATTERNS){snprintf(pats[npat],sizeof pats[npat],"%s",sc.tok_text); npat++; next_tok();}
        else next_tok();
    }
    if(is_sym_c(')')) next_tok();
    struct in_sec *match[MAX_SECTIONS]; int nm=0;
    for(int i=0;i<n_secs;i++){struct in_sec *s=all_secs[i]; if(s->out_idx!=-1) continue;
        if(s->is_merge&&!s->is_pool) continue;   /* SH5b: contents live in the pool */
        for(int p=0;p<npat;p++) if(pat_match(pats[p],s->name)){if(nm<MAX_SECTIONS) match[nm++]=s; s->out_idx=out_idx; break;}}
    if(sort_by_name&&nm>1) qsort(match,(size_t)nm,sizeof(match[0]),cmp_sec_name);
    struct out_sec *o=&out_secs[out_idx];
    for(int i=0;i<nm;i++){
        uint64_t a=match[i]->align?match[i]->align:1;
        o->size=(o->size+a-1)&~(a-1);
        match[i]->out_off=o->size;
        o->size+=match[i]->size;
        if(match[i]->align>o->align) o->align=match[i]->align;
        o->flags|=match[i]->flags;
    }
}
static void parse_keep_wrapper(int out_idx){
    int sort=(strcmp(sc.tok_text,"SORT_BY_INIT_PRIORITY")==0);
    next_tok(); if(is_sym_c('(')){next_tok();
        while(!is_sym_c(')')&&sc.tok!=0){
            if(sc.tok_text[0]=='*'){next_tok(); if(is_sym_c('(')) parse_input_group(out_idx,sort);}
            else next_tok();
        }
        if(is_sym_c(')')) next_tok();
    }
}
static void skip_parens(void){
    if(!is_sym_c('(')) return; int d=0; do{if(is_sym_c('(')) d++; if(is_sym_c(')')) d--; if(d) next_tok();}while(d&&sc.tok!=0); if(is_sym_c(')')) next_tok();
}
static void parse_script(void){
    next_tok();
    while(sc.tok!=0){
        if(sc.tok==1){
            char kw[64]; snprintf(kw,sizeof kw,"%s",sc.tok_text); next_tok();
            if(strcmp(kw,"OUTPUT_FORMAT")==0||strcmp(kw,"OUTPUT_ARCH")==0){skip_parens();}
            else if(strcmp(kw,"ENTRY")==0){
                if(is_sym_c('(')){next_tok(); if(sc.tok==1){snprintf(sc.entry,sizeof sc.entry,"%s",sc.tok_text); next_tok();} if(is_sym_c(')')) next_tok();}
            }else if(strcmp(kw,"PHDRS")==0){
                if(is_sym_c('{')) next_tok();
                while(!is_sym_c('}')&&sc.tok!=0){
                    if(sc.tok==1&&sc.phdr_count<MAX_PHDRS){
                        struct phdr_def *ph=&sc.phdrs[sc.phdr_count];
                        snprintf(ph->name,sizeof ph->name,"%s",sc.tok_text); next_tok();
                        if(sc.tok==1&&strcmp(sc.tok_text,"PT_LOAD")==0) next_tok();
                        if(sc.tok==1&&strcmp(sc.tok_text,"FLAGS")==0){
                            next_tok(); if(is_sym_c('(')){next_tok(); ph->flags=expr_value(); if(is_sym_c(')')) next_tok();}
                        }
                        sc.phdr_count++; if(is_sym_c(';')) next_tok();
                    }else next_tok();
                }
                if(is_sym_c('}')) next_tok();
            }else if(strcmp(kw,"SECTIONS")==0){
                if(is_sym_c('{')) next_tok();
                while(!is_sym_c('}')&&sc.tok!=0){
                    if(is_dot()){
                        next_tok();
                        if(is_sym_c('=')){next_tok(); cur_addr=expr_value(); if(is_sym_c(';')) next_tok();}
                        else if(is_sym_c('+')){next_tok(); if(is_sym_c('=')){next_tok(); cur_addr+=expr_value(); if(is_sym_c(';')) next_tok();}}
                    }else if(is_sym_c('/')){
                        next_tok();
                        if(sc.tok==1&&strcmp(sc.tok_text,"DISCARD")==0){
                            next_tok(); if(is_sym_c('/')) next_tok(); if(is_sym_c(':')) next_tok(); if(is_sym_c('{')) next_tok();
                            while(!is_sym_c('}')&&sc.tok!=0){
                                if(sc.tok_text[0]=='*'){next_tok(); if(is_sym_c('(')) skip_parens();}
                                else if(is_sym_c('(')) skip_parens(); else next_tok();
                            }
                            if(is_sym_c('}')) next_tok();
                            for(int i=0;i<n_secs;i++) if(all_secs[i]->out_idx==-1&&discard_section(all_secs[i]->name)) all_secs[i]->out_idx=-2;
                        }else next_tok();
                    }else if(sc.tok==1){
                        char name[64]; snprintf(name,sizeof name,"%s",sc.tok_text); next_tok();
                        if(is_sym_c('=')){next_tok(); script_sym(name,expr_value()); if(is_sym_c(';')) next_tok();}
                        else{
                            uint64_t sec_align=0;
                            if(sc.tok==1&&strcmp(sc.tok_text,"ALIGN")==0){
                                next_tok(); if(is_sym_c('(')){next_tok(); sec_align=expr_value(); if(is_sym_c(')')) next_tok();}
                            }
                            if(is_sym_c(':')) next_tok();
                            int ph=-1;
                            if(is_sym_c('{')){
                                next_tok();
                                if(n_out>=MAX_OUT_SECS){fprintf(stderr,"aulink: too many out secs\n"); errors++; break;}
                                struct out_sec *o=&out_secs[n_out]; memset(o,0,sizeof *o);
                                snprintf(o->name,sizeof o->name,"%s",name);
                                o->type=SHT_PROGBITS; o->align=1;
                                if(strcmp(name,".bss")==0){o->type=SHT_NOBITS; o->flags=SHF_ALLOC|SHF_WRITE;}
                                else if(strcmp(name,".init_array")==0){o->type=SHT_INIT_ARRAY; o->flags=SHF_ALLOC|SHF_WRITE;}
                                else if(strcmp(name,".fini_array")==0){o->type=SHT_FINI_ARRAY; o->flags=SHF_ALLOC|SHF_WRITE;}
                                else if(strcmp(name,".text")==0){o->flags=SHF_ALLOC|SHF_EXECINSTR;}
                                else if(strcmp(name,".data")==0){o->flags=SHF_ALLOC|SHF_WRITE;}
                                else o->flags=SHF_ALLOC;
                                int out_idx=n_out; n_out++;
                                if(sec_align>o->align) o->align=sec_align;
                                uint64_t a=o->align?o->align:1; cur_addr=(cur_addr+a-1)&~(a-1); o->addr=cur_addr;
                                while(!is_sym_c('}')&&sc.tok!=0){
                                    if(sc.tok_text[0]=='*'){next_tok(); if(is_sym_c('(')) parse_input_group(out_idx,0);
                                        /* symbols defined INSIDE this block
                                         * (e.g. kernel.ld's __bss_start/__bss_end
                                         * around *(.bss)) must see the current
                                         * end of the output section, not the
                                         * section's start. */
                                        cur_addr=o->addr+o->size;}
                                    else if(sc.tok==1&&(strcmp(sc.tok_text,"KEEP")==0||strcmp(sc.tok_text,"SORT_BY_INIT_PRIORITY")==0)) parse_keep_wrapper(out_idx);
                                    else if(is_dot()){next_tok(); if(is_sym_c('=')){next_tok(); cur_addr=expr_value(); if(is_sym_c(';')) next_tok();}}
                                    else if(sc.tok==1){
                                        char sn2[64]; snprintf(sn2,sizeof sn2,"%s",sc.tok_text); next_tok();
                                        if(is_sym_c('=')){next_tok(); script_sym(sn2,expr_value()); if(is_sym_c(';')) next_tok();}
                                    }else if(is_sym_c(';')) next_tok(); else next_tok();
                                }
                                if(is_sym_c('}')) next_tok();
                                if(is_sym_c(':')){next_tok(); if(sc.tok==1){for(int p=0;p<sc.phdr_count;p++) if(strcmp(sc.phdrs[p].name,sc.tok_text)==0) ph=p; next_tok();}}
                                out_secs[out_idx].phdr=ph;
                                /* SH5b: the input groups may have raised
                                 * o->align past what the script said (no
                                 * ALIGN() in kernel.ld -> it was 1); GNU
                                 * ld aligns the section start by the max
                                 * input align, so re-align now that the
                                 * groups are known. */
                                if(o->align>1){
                                    uint64_t na=(o->addr+o->align-1)&~(o->align-1);
                                    if(na!=o->addr){ cur_addr+=(na-o->addr); o->addr=na; }
                                }
                                uint64_t end=o->addr+o->size; if(cur_addr>end) end=cur_addr; cur_addr=end;
                            }
                        }
                    }else next_tok();
                }
                if(is_sym_c('}')) next_tok();
            }else if(is_sym_c('=')){next_tok(); script_sym(kw,expr_value()); if(is_sym_c(';')) next_tok();}
            else{while(sc.tok!=0&&!is_sym_c(';')) next_tok(); if(is_sym_c(';')) next_tok();}
        }else next_tok();
    }
}

/* ---- symbol resolution ---- */
static void resolve_symbols(void){
    for(int i=0;i<n_syms;i++){
        struct in_sym *s=all_syms[i]; if(s->is_script) continue;
        if(s->shndx==SHN_ABS){s->addr=s->value; s->defined=1; continue;}
        s->defined=0;
        if(s->shndx==SHN_UNDEF||s->shndx==SHN_COMMON) continue;
        int ai=alloc_index_for_shndx(s->obj,s->shndx); if(ai<0) continue;
        struct in_sec *sec=&s->obj->secs[ai];
        if(sec->out_idx<0) continue;
        s->out_sec=sec->out_idx+1; s->addr=out_secs[sec->out_idx].addr+sec->out_off+s->value; s->defined=1;
    }
    for(int i=0;i<n_syms;i++){
        struct in_sym *s=all_syms[i]; if(s->defined||s->shndx!=SHN_UNDEF||s->name[0]==0) continue;
        if((s->info>>4)==STB_WEAK){s->addr=0; s->defined=1; continue;}
        struct in_sym *g=find_global(s->name);
        if(g&&g!=s){s->addr=g->addr; s->out_sec=g->out_sec; s->defined=1;}
    }
    for(int i=0;i<n_syms;i++){
        struct in_sym *s=all_syms[i]; if(s->defined||s->shndx!=SHN_UNDEF||s->name[0]==0) continue;
        if((s->info>>4)==STB_WEAK) continue;
        fprintf(stderr,"aulink: undefined reference to '%s'\n",s->name); errors++;
    }
}
static uint64_t sym_addr(const struct in_sym *sym){
    if(sym->defined) return sym->addr;
    if(sym->name[0]){struct in_sym *g=find_global(sym->name); if(g) return g->addr;}
    return 0;
}

/* ---- GOT handling (tcc emits R_X86_64_GOTPCREL) ---- */
static int got_find(const char *name){
    for(int i=0;i<n_got;i++) if(strcmp(got_syms[i]->name,name)==0) return i;
    return -1;
}
static void got_collect(int *out_n){
    n_got=0;
    for(int oi=0;oi<n_objs;oi++){
        struct in_obj *o=&objs[oi];
        for(int si=0;si<o->shnum;si++){
            struct elf64_shdr *h=&o->shdrs[si];
            if(h->sh_type!=SHT_RELA) continue;
            size_t nrel=(size_t)(h->sh_size/h->sh_entsize);
            const uint8_t *r=o->file+h->sh_offset;
            for(size_t k=0;k<nrel;k++){
                const uint8_t *re=r+k*h->sh_entsize;
                uint64_t info=rd64(re+8); uint32_t type=(uint32_t)(info&0xffffffff);
                if(type!=R_X86_64_GOTPCREL) continue;
                uint32_t sym_idx=(uint32_t)(info>>32);
                if(sym_idx>=o->sym_count) continue;
                struct in_sym *sym=&o->syms[sym_idx];
                if(sym->name[0]==0) continue;
                if(got_find(sym->name)>=0) continue;
                if(n_got>=MAX_GOT){fprintf(stderr,"aulink: too many GOT entries\n"); errors++; return;}
                got_syms[n_got++]=sym;
            }
        }
    }
    if(out_n) *out_n=n_got;
}
static void got_setup(void){
    if(n_got==0) return;
    struct out_sec *bss=NULL;
    for(int i=0;i<n_out;i++) if(strcmp(out_secs[i].name,".bss")==0) bss=&out_secs[i];
    struct out_sec *data=NULL;
    for(int i=0;i<n_out;i++) if(strcmp(out_secs[i].name,".data")==0) data=&out_secs[i];
    if(!data&&!bss) return;
    uint64_t base = data ? (data->addr+data->size) : bss->addr;
    if(n_out>=MAX_OUT_SECS){fprintf(stderr,"aulink: too many out secs for .got\n"); errors++; return;}
    struct out_sec *g=&out_secs[n_out];
    memset(g,0,sizeof *g); snprintf(g->name,sizeof g->name,".got");
    g->type=SHT_PROGBITS; g->flags=SHF_ALLOC|SHF_WRITE; g->align=8;
    g->size=(uint64_t)n_got*8; g->addr=(base+7)&~7ULL;
    g->phdr = data ? data->phdr : (bss?bss->phdr:-1);
    got_out_idx=n_out+1; /* 1-based (0 NULL) */
    n_out++;
    if(bss){
        uint64_t old_bss = bss->addr;
        uint64_t new_bss = (g->addr+g->size+15)&~15ULL;
        bss->addr=new_bss;
        cur_addr = new_bss + bss->size;
        /* SH5c (kernel link): the .got is inserted at the old .bss base and
         * .bss moves up, but every symbol address was already computed from
         * the pre-insertion layout.  Without this fixup __bss_start keeps
         * pointing at the OLD .bss base -- which is now inside .got -- and
         * a consumer that zeroes .bss through the script symbols (the
         * kernel's boot.asm does exactly that) wipes the relocation slots
         * and every GOT-relative address becomes 0.  The userland never
         * hit this: user.ld defines no __bss_* symbols and the kernel's
         * ELF loader zeroes user .bss from the PHDR, which the .got is
         * not part of.  Move every defined symbol at/after the old .bss
         * base with the section (object symbols inside .bss, script
         * symbols like __bss_start/__bss_end/end). */
        if(new_bss>old_bss){
            uint64_t delta=new_bss-old_bss;
            for(int i=0;i<n_syms;i++){
                struct in_sym *s=all_syms[i];
                if(!s->defined) continue;
                if(s->addr>=old_bss) s->addr+=delta;
            }
        }
    }
}

/* ---- relocations ---- */
static void apply_relocations(int collect_only){
    for(int oi=0;oi<n_objs;oi++){
        struct in_obj *o=&objs[oi];
        for(int si=0;si<o->shnum;si++){
            struct elf64_shdr *h=&o->shdrs[si];
            if(h->sh_type!=SHT_RELA) continue;
            const char *rn=sec_name(o,(uint32_t)si);
            if(strncmp(rn,".rela",5)!=0) continue;
            const char *tname=rn+5; int tsec=-1;
            for(int j=0;j<o->shnum;j++){
                if(!(o->shdrs[j].sh_flags&SHF_ALLOC)) continue;
                if(strcmp(sec_name(o,(uint32_t)j),tname)==0){tsec=alloc_index_for_shndx(o,(uint16_t)j); break;}
            }
            if(tsec<0) continue;
            struct in_sec *sec=&o->secs[tsec]; if(sec->out_idx<0) continue;
            struct out_sec *out=&out_secs[sec->out_idx];
            uint64_t base=out->addr+sec->out_off;
            size_t nrel=(size_t)(h->sh_size/h->sh_entsize);
            const uint8_t *r=o->file+h->sh_offset;
            for(size_t k=0;k<nrel;k++){
                const uint8_t *re=r+k*h->sh_entsize;
                uint64_t off=rd64(re); uint64_t info=rd64(re+8); int64_t addend=(int64_t)rd64(re+16);
                uint32_t sym_idx=(uint32_t)(info>>32); uint32_t type=(uint32_t)(info&0xffffffff);
                if(sym_idx>=o->sym_count) continue;
                struct in_sym *sym=&o->syms[sym_idx];
                if(collect_only){
                    if(type==R_X86_64_GOTPCREL&&sym->name[0]){
                        if(got_find(sym->name)<0&&n_got<MAX_GOT) got_syms[n_got++]=sym;
                    }
                    continue;
                }
                uint64_t S=sym_addr(sym); uint64_t P=base+off; uint8_t *where=sec->data?sec->data+off:NULL;
                /* SH5b: a relocation against a merge section targets one of
                 * its strings; the string lives in the pool now, so re-base
                 * the addend (which is the string's offset in the original
                 * section) onto the pool address. */
                {
                    struct in_sec *msym=NULL;
                    if(sym->obj){
                        int mai=alloc_index_for_shndx(sym->obj,sym->shndx);
                        if(mai>=0&&sym->obj->secs[mai].is_merge) msym=&sym->obj->secs[mai];
                    }
                    if(msym&&msym->pool&&msym->mcount){
                        /* PC-relative relocations carry the standard -4
                         * bias (disp32 is measured from the end of the
                         * instruction): the element offset is addend+4.
                         * Absolute ones (64/32/32S) use the addend as-is. */
                        int pc=(type==R_X86_64_PC32||type==R_X86_64_PLT32);
                        int64_t want=(int64_t)addend+(pc?4:0);
                        int lo=0,hi=msym->mcount-1,found=-1;
                        while(lo<=hi){int mid=(lo+hi)/2;
                            if(want>=(int64_t)msym->morig[mid]&&want<(int64_t)(msym->morig[mid]+msym->mlen[mid])){found=mid;break;}
                            else if(want<(int64_t)msym->morig[mid]) hi=mid-1; else lo=mid+1;}
                        if(found>=0){
                            struct in_sec *pool=msym->pool;
                            S=out_secs[pool->out_idx].addr+pool->out_off;
                            addend=(int64_t)msym->mpool[found]-(pc?4:0);
                        } else {
                            fprintf(stderr,"aulink: merge addend 0x%llx out of range in %s\n",(unsigned long long)addend,msym->name); errors++;
                        }
                    }
                }
                switch(type){
                case R_X86_64_64:{uint64_t v=S+(uint64_t)addend; if(where) for(int b=0;b<8;b++) where[b]=(uint8_t)(v>>(8*b)); break;}
                case R_X86_64_32: case R_X86_64_32S:{
                    uint64_t v=S+(uint64_t)addend;
                    if(type==R_X86_64_32S){int64_t sv=(int64_t)v; if(sv<INT32_MIN||sv>INT32_MAX){fprintf(stderr,"aulink: 32S overflow %s+0x%llx\n",sec->name,(unsigned long long)off); errors++;}}
                    else if(v>0xffffffffULL){fprintf(stderr,"aulink: 32 overflow %s+0x%llx\n",sec->name,(unsigned long long)off); errors++;}
                    if(where) for(int b=0;b<4;b++) where[b]=(uint8_t)(v>>(8*b)); break;
                }
                case R_X86_64_PC32: case R_X86_64_PLT32:{
                    uint64_t v=S+(uint64_t)addend-P; int64_t sv=(int64_t)v;
                    if(sv<INT32_MIN||sv>INT32_MAX){fprintf(stderr,"aulink: %s overflow %s+0x%llx\n",type==R_X86_64_PLT32?"PLT32":"PC32",sec->name,(unsigned long long)off); errors++;}
                    if(where) for(int b=0;b<4;b++) where[b]=(uint8_t)(v>>(8*b)); break;
                }
                case R_X86_64_GOTPCREL:{
                    int gi=got_find(sym->name); if(gi<0){fprintf(stderr,"aulink: GOTPCREL no entry for %s\n",sym->name); errors++; break;}
                    uint64_t got_addr=out_secs[got_out_idx-1].addr+(uint64_t)gi*8;
                    uint64_t v=got_addr+(uint64_t)addend-P;
                    if(where) for(int b=0;b<4;b++) where[b]=(uint8_t)(v>>(8*b)); break;
                }
                case R_X86_64_16: case R_X86_64_PC16:{
                    uint64_t v=(type==R_X86_64_16)?(S+(uint64_t)addend):(S+(uint64_t)addend-P);
                    if(where){where[0]=(uint8_t)v; where[1]=(uint8_t)(v>>8);} break;
                }
                case R_X86_64_8: case R_X86_64_PC8:{
                    uint64_t v=(type==R_X86_64_8)?(S+(uint64_t)addend):(S+(uint64_t)addend-P);
                    if(where) where[0]=(uint8_t)v; break;
                }
                default:fprintf(stderr,"aulink: unsupported reloc %u at %s+0x%llx\n",type,sec->name,(unsigned long long)off); errors++;
                }
            }
        }
    }
}

/* ---- output ---- */
static void write_output(const char *path){
    int n=n_out+4; struct out_sec *all=calloc((size_t)n,sizeof(struct out_sec));
    for(int i=0;i<n_out;i++) all[i+1]=out_secs[i];
    snprintf(all[n-3].name,sizeof all[n-3].name,".symtab");
    snprintf(all[n-2].name,sizeof all[n-2].name,".strtab");
    snprintf(all[n-1].name,sizeof all[n-1].name,".shstrtab");

    size_t shstr_len=1; for(int i=0;i<n;i++) shstr_len+=strlen(all[i].name)+1;
    char *shstr=malloc(shstr_len); size_t sp=1; shstr[0]=0;
    for(int i=0;i<n;i++){size_t l=strlen(all[i].name); memcpy(shstr+sp,all[i].name,l+1); all[i].name_off=(uint32_t)sp; sp+=l+1;}

    size_t str_len=1; for(int i=0;i<n_syms;i++) if(all_syms[i]->defined&&all_syms[i]->name[0]) str_len+=strlen(all_syms[i]->name)+1;
    char *strtab=malloc(str_len); size_t strp=1; strtab[0]=0;
    int nsym_out=1;
    for(int i=0;i<n_syms;i++) if(all_syms[i]->defined&&all_syms[i]->name[0]) nsym_out++;
    uint8_t *symtab=calloc((size_t)nsym_out*24,1);
    int sym_out=1;
    for(int i=0;i<n_syms;i++){
        struct in_sym *s=all_syms[i]; if(!(s->defined&&s->name[0])) continue;
        size_t l=strlen(s->name); memcpy(strtab+strp,s->name,l+1);
        uint8_t *e=symtab+(size_t)sym_out*24;
        put32(e,(uint32_t)strp); e[4]=s->info; put16(e+6,(uint16_t)(s->out_sec?s->out_sec:0));
        put64(e+8,s->addr); put64(e+16,0); strp+=l+1; sym_out++;
    }

    uint64_t off=64; uint64_t ph_off=off; int ph_count=sc.phdr_count; off+=(uint64_t)ph_count*56;
    uint64_t *sec_off=calloc((size_t)n,sizeof(uint64_t));
    uint64_t seg_vaddr[MAX_PHDRS]; uint64_t seg_off[MAX_PHDRS];
    for(int p=0;p<ph_count;p++) seg_vaddr[p]=0, seg_off[p]=0;
    for(int p=0;p<ph_count;p++){
        int first=1; uint64_t v=0, fo=0;
        for(int i=1;i<=n_out;i++) if(all[i].phdr==p){
            if(first){v=all[i].addr; fo=off; uint64_t a=all[i].align?all[i].align:1; off=(off+a-1)&~(a-1); fo=off; seg_vaddr[p]=v; seg_off[p]=fo; first=0;}
        }
        if(first) continue;
        for(int i=1;i<=n_out;i++) if(all[i].phdr==p){
            if(all[i].type==SHT_NOBITS) continue;
            sec_off[i]=seg_off[p]+(all[i].addr-seg_vaddr[p]);
            uint64_t end=sec_off[i]+all[i].size;
            if(end>off) off=end;
        }
    }
    off=(off+7)&~7ULL; sec_off[n-3]=off; off+=(uint64_t)nsym_out*24;
    sec_off[n-2]=off; off+=strp; sec_off[n-1]=off; off+=shstr_len;
    uint64_t sh_off=(off+7)&~7ULL; uint64_t file_size=sh_off+(uint64_t)n*64;
    uint8_t *out=calloc((size_t)file_size,1);

    for(int i=1;i<=n_out;i++){
        struct out_sec *o=&all[i]; if(o->type==SHT_NOBITS) continue;
        for(int k=0;k<n_secs;k++){
            struct in_sec *s=all_secs[k]; if(s->out_idx!=i-1||!s->data) continue;
            memcpy(out+sec_off[i]+s->out_off,s->data,(size_t)s->size);
        }
    }
    /* fill .got */
    if(got_out_idx>0&&n_got>0){
        int gi=got_out_idx; uint8_t *gd=out+sec_off[gi];
        for(int i=0;i<n_got&&i*8<(int)all[gi].size;i++){
            struct in_sym *sym=got_syms[i]; uint64_t a=sym_addr(sym);
            for(int b=0;b<8;b++) gd[i*8+b]=(uint8_t)(a>>(8*b));
        }
    }
    memcpy(out+sec_off[n-3],symtab,(size_t)nsym_out*24);
    memcpy(out+sec_off[n-2],strtab,strp);
    memcpy(out+sec_off[n-1],shstr,shstr_len);

    uint8_t *h=out;
    memcpy(h,"\x7f""ELF",4); h[4]=ELFCLASS64; h[5]=ELFDATA2LSB; h[6]=1;
    put16(h+16,ET_EXEC); put16(h+18,EM_X86_64); put32(h+20,EV_CURRENT);
    uint64_t entry=0; for(int i=0;i<n_syms;i++) if(all_syms[i]->defined&&strcmp(all_syms[i]->name,sc.entry)==0) entry=all_syms[i]->addr;
    put64(h+24,entry); put64(h+32,ph_off); put64(h+40,sh_off);
    put16(h+52,64); put16(h+54,56); put16(h+56,(uint16_t)ph_count); put16(h+58,64); put16(h+60,(uint16_t)n); put16(h+62,(uint16_t)(n-1));

    for(int p=0;p<ph_count;p++){
        uint8_t *ph=out+ph_off+(size_t)p*56;
        uint64_t vaddr=0, memsz=0, filesz=0, poff=0; int first=1;
        for(int i=1;i<=n_out;i++) if(all[i].phdr==p){
            if(first){vaddr=all[i].addr; poff=sec_off[i]; first=0;}
            uint64_t e=all[i].addr+all[i].size; if(e>vaddr+memsz) memsz=e-vaddr;
            if(all[i].type!=SHT_NOBITS){uint64_t fe=sec_off[i]+all[i].size-poff; if(fe>filesz) filesz=fe;}
        }
        if(first) continue;
        put32(ph,PT_LOAD); put32(ph+4,(uint32_t)sc.phdrs[p].flags);
        put64(ph+8,poff); put64(ph+16,vaddr); put64(ph+24,vaddr);
        put64(ph+32,filesz); put64(ph+40,memsz); put64(ph+48,4096);
    }
    for(int i=0;i<n;i++){
        uint8_t *sh=out+sh_off+(size_t)i*64; struct out_sec *o=&all[i];
        put32(sh,o->name_off);
        if(i==0){put32(sh+4,SHT_NULL); continue;}
        uint32_t type=o->type; if(i==n-3) type=SHT_SYMTAB; else if(i==n-2||i==n-1) type=SHT_STRTAB;
        put32(sh+4,type); put64(sh+8,o->flags);
        if(i==n-3){put64(sh+16,0); put64(sh+24,sec_off[i]); put64(sh+32,(uint64_t)nsym_out*24); put32(sh+40,(uint32_t)(n-2)); put32(sh+44,1); put64(sh+48,8); put64(sh+56,24); continue;}
        if(i==n-2||i==n-1){put64(sh+24,sec_off[i]); put64(sh+32,(i==n-2)?strp:shstr_len); put64(sh+48,1); continue;}
        put64(sh+16,o->addr); put64(sh+24,sec_off[i]); put64(sh+32,o->size); put64(sh+48,o->align?o->align:1);
        if(type==SHT_INIT_ARRAY||type==SHT_FINI_ARRAY) put64(sh+56,8);
    }
    FILE *f=fopen(path,"wb"); if(!f){fprintf(stderr,"aulink: cannot write %s\n",path); exit(1);}
    fwrite(out,1,(size_t)file_size,f); fclose(f);
    free(out); free(shstr); free(strtab); free(symtab); free(sec_off); free(all);
}

/* ---- SH5b: SHF_MERGE|SHF_STRINGS pools ---- */
static uint8_t *pool_data; static size_t pool_len, pool_cap;
static struct { uint32_t hash, off, len; } *pent; static int npent, cpent;
static int pool_buckets[8192]; static int *pnext; static int nbuckets=8192;

static uint32_t str_hash(const uint8_t *p, size_t n){
    uint32_t h=2166136261u; for(size_t i=0;i<n;i++){h^=p[i]; h*=16777619u;} return h;
}
/* find/add a byte string in the pool; returns the pool offset */
static uint32_t pool_add(const uint8_t *p, size_t n, size_t align){
    if(align>1){ pool_len=(pool_len+align-1)&~(align-1); }
    uint32_t h=str_hash(p,n);
    int b=(int)(h&(nbuckets-1));
    for(int idx=pool_buckets[b]; idx>=0; idx=pnext[idx]){
        if(pent[idx].hash==h&&pent[idx].len==(uint32_t)n&&
           memcmp(pool_data+pent[idx].off,p,n)==0) return pent[idx].off;
    }
    if(pool_len+n>pool_cap){ pool_cap=pool_cap?pool_cap*2:16384; while(pool_len+n>pool_cap) pool_cap*=2;
        pool_data=realloc(pool_data,pool_cap); if(!pool_data){fprintf(stderr,"aulink: oom pool\n"); exit(1);} }
    uint32_t off=(uint32_t)pool_len;
    memcpy(pool_data+off,p,n); pool_len+=n;
    if(npent>=cpent){ cpent=cpent?cpent*2:1024; pent=realloc(pent,(size_t)cpent*sizeof*pent);
        pnext=realloc(pnext,(size_t)cpent*sizeof(int)); if(!pent||!pnext){fprintf(stderr,"aulink: oom pent\n"); exit(1);} }
    pent[npent].hash=h; pent[npent].off=off; pent[npent].len=(uint32_t)n;
    pnext[npent]=pool_buckets[b]; pool_buckets[b]=npent; npent++;
    return off;
}

static void build_merge_pools(void){
    /* pool keys: the section-name suffix (str1.1, str1.16, cst4, cst16,
     * cst32) -- ld.lld merges per section NAME, not per entsize (str1.16
     * and cst16 share entsize 16 but get separate pools). */
    char keys[16][64]; int nkeys=0;
    for(int i=0;i<n_secs;i++) if(all_secs[i]->is_merge){
        const char *nm=all_secs[i]->name; const char *dot=strrchr(nm,'.');
        const char *key=dot?dot+1:nm;
        int seen=0; for(int k=0;k<nkeys;k++) if(strcmp(keys[k],key)==0){seen=1;break;}
        if(!seen&&nkeys<16) snprintf(keys[nkeys++],64,"%s",key);
    }
    if(!nkeys) return;
    for(int e=0;e<nkeys;e++){
        const char *key=keys[e];
        uint64_t es=0;
        for(int i=0;i<n_secs;i++) if(all_secs[i]->is_merge){
            const char *nm=all_secs[i]->name; const char *dot=strrchr(nm,'.');
            if(strcmp(dot?dot+1:nm,key)==0){es=all_secs[i]->entsize;break;}
        }
        pool_data=NULL; pool_len=0; pool_cap=0; npent=0; cpent=0;
        for(int i=0;i<nbuckets;i++) pool_buckets[i]=-1;
        pnext=NULL;
        /* pool section created below; first pass assigns every merge section
         * a map and grows the pool in section order (== object order, which
         * is how ld.lld orders the merged strings too) */
        for(int i=0;i<n_secs;i++){
            struct in_sec *s=all_secs[i];
            if(!s->is_merge||s->type==SHT_NOBITS) continue;
            { const char *nm=s->name; const char *dot=strrchr(nm,'.');
              if(strcmp(dot?dot+1:nm,key)!=0) continue; }
            /* count elements */
            int is_str=(s->flags&SHF_STRINGS)!=0;
            int cnt=0; size_t off=0;
            while(off<s->size){
                if(is_str&&es==1){ const uint8_t *z=memchr(s->data+off,0,s->size-off); if(!z) break; off+=(size_t)(z-(s->data+off))+1; }
                else off+=es;
                cnt++;
            }
            if(!cnt) continue;
            s->morig=malloc((size_t)cnt*sizeof(uint32_t));
            s->mpool=malloc((size_t)cnt*sizeof(uint32_t));
            s->mlen  =malloc((size_t)cnt*sizeof(uint32_t));
            s->mcount=cnt;
            int idx=0; off=0;
            while(off<s->size){
                size_t alen;
                uint32_t poff;
                if(is_str&&es==1){
                    const uint8_t *z=memchr(s->data+off,0,s->size-off);
                    if(!z) break;
                    alen=(size_t)(z-(s->data+off))+1;   /* incl NUL */
                    poff=pool_add(s->data+off,alen,1);
                }else if(is_str){
                    alen=strnlen((const char*)s->data+off,(size_t)es);
                    poff=pool_add(s->data+off,alen,(size_t)s->align?s->align:1);
                }else{
                    alen=(size_t)es;                    /* fixed-size constants */
                    poff=pool_add(s->data+off,alen,(size_t)s->align?s->align:1);
                }
                s->morig[idx]=(uint32_t)off;
                s->mpool[idx]=poff;
                s->mlen[idx]=(uint32_t)alen;
                idx++;
                off+=(is_str&&es==1)?alen:es;
            }
            s->mcount=idx;
        }
        /* build the pool section and prepend it to all_secs */
        if(pool_len){
            struct in_sec *pool=calloc(1,sizeof *pool);
            snprintf(pool->name,sizeof pool->name,".rodata.%s.pool",key);
            pool->type=SHT_PROGBITS; pool->flags=SHF_ALLOC;
            pool->align=1; for(int i=0;i<n_secs;i++)
                if(all_secs[i]->is_merge&&all_secs[i]->align>pool->align){
                    const char *nm=all_secs[i]->name; const char *dot=strrchr(nm,'.');
                    if(strcmp(dot?dot+1:nm,key)==0) pool->align=all_secs[i]->align; }
            pool->size=pool_len;
            pool->data=malloc(pool_len); memcpy(pool->data,pool_data,pool_len);
            pool->out_idx=-1; pool->is_pool=1; pool->entsize=es;
            for(int i=0;i<n_secs;i++) if(all_secs[i]->is_merge){
                const char *nm=all_secs[i]->name; const char *dot=strrchr(nm,'.');
                if(strcmp(dot?dot+1:nm,key)==0) all_secs[i]->pool=pool; }
            /* insert before the FIRST merge section of this entsize, so the
             * pool lands where ld.lld places the merged strings (the other
             * merge sections of this entsize are dropped from the layout;
             * their bytes live in the pool). */
            int pos=n_secs;
            for(int i=0;i<n_secs;i++)
                if(all_secs[i]->is_merge){
                    const char *nm=all_secs[i]->name; const char *dot=strrchr(nm,'.');
                    if(strcmp(dot?dot+1:nm,key)==0){pos=i;break;} }
            memmove(&all_secs[pos+1],&all_secs[pos],(size_t)(n_secs-pos)*sizeof(all_secs[0]));
            all_secs[pos]=pool; n_secs++;
        }
        free(pent); free(pnext); pent=NULL; pnext=NULL;
    }
}

int main(int argc,char **argv){
    const char *script=NULL,*out=NULL;
    int i=1;
    while(i<argc){if(strcmp(argv[i],"-T")==0&&i+1<argc) script=argv[++i]; else if(strcmp(argv[i],"-o")==0&&i+1<argc) out=argv[++i]; i++;}
    if(!script||!out){fprintf(stderr,"usage: aulink -T <script> -o <out> <obj>... [.a]\n"); return 2;}
    for(i=1;i<argc;i++){
        if(strcmp(argv[i],"-T")==0||strcmp(argv[i],"-o")==0){i++; continue;}
        if(argv[i][0]=='-'&&argv[i][1]!=0) continue;
        const char *p=argv[i];
        size_t l=strlen(p);
        if(l>=2&&strcmp(p+l-2,".a")==0){load_archive(p,&n_objs);}
        else if(read_object(p,n_objs)==0) n_objs++;
    }
    if(n_objs==0){fprintf(stderr,"aulink: no inputs\n"); return 1;}
    FILE *sf=fopen(script,"rb"); if(!sf){fprintf(stderr,"aulink: cannot open script %s\n",script); return 1;}
    fseek(sf,0,SEEK_END); long ssz=ftell(sf); fseek(sf,0,SEEK_SET);
    sc.text=malloc((size_t)ssz+1); if(fread(sc.text,1,(size_t)ssz,sf)!=(size_t)ssz) return 1;
    sc.text[ssz]=0; sc.len=(size_t)ssz; fclose(sf);
    build_merge_pools();
    cur_addr=0;
    parse_script();
    int got_n=0; apply_relocations(1); got_n=n_got; /* collect GOT */
    got_setup();
    resolve_symbols();
    apply_relocations(0);
    if(errors){fprintf(stderr,"aulink: %d error(s)\n",errors); return 1;}
    write_output(out);
    return 0;
}
