/*
 * mini-asm -- a small NASM-dialect assembler for AuraLite OS self-hosting.
 *
 * SELFHOST_PLAN.md phase SH4a.  The goal of SH4 is to assemble the tree's
 * `.asm` without nasm; SH4a lands the core and proves it BYTE-FOR-BYTE
 * against nasm on the simplest real flat-binary file
 * (boot/bios/stage1/mbr_dual.asm).  The gate (tests/unit/test_asm_parity.sh)
 * fails the build on any byte difference, so this file is held to nasm's
 * exact output, not merely "an assembler that runs".
 *
 * Scope of SH4a (what this file implements today):
 *   - `-f bin` output only (flat image at `org`; no relocations, no symtab,
 *     so byte-parity is purely encoder + preprocessor + layout).
 *   - directives: bits, org, equ, db/dw/dd/dq, resb/resw/resd/resq,
 *     align/alignb, times (data forms).
 *   - `$` (current pc) and `$$` (section/org base).
 *   - labels, including NASM local labels (`.x` scoped to the last global).
 *   - the x86 instruction subset the boot MBR uses (16-bit): mov/xor/cmp/
 *     test/int/jmp/jcc/call (incl. far `jmp seg:off`), with nasm's
 *     shortest-encoding-that-fits jump sizing resolved by fixed-point
 *     iteration.
 *
 * Deliberately NOT here yet (SH4b): %include, %define, %macro/%endmacro,
 * %rep, %if/%else, the elf64/elf32 emitters, and the wider encoder surface.
 * Those land when stage2_start.asm (which needs them) is brought to parity.
 *
 * Build (host):  cc -std=c99 -O2 -o mini-asm mini-asm.c
 * Usage:         mini-asm -f bin [-I dir] input.asm -o output.bin
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* fatal / helpers                                                      */
/* ------------------------------------------------------------------ */

static int g_line_no = 0;

static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "mini-asm: line %d: ", g_line_no);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "mini-asm: out of memory\n"); exit(2); }
    return p;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------ */
/* symbol table                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char      *name;
    long long  val;        /* offset within section (defined) or constant value */
    int        defined;
    int        is_global;  /* declared `global` */
    int        is_extern;  /* declared `extern` */
    int        is_equ;     /* `equ` constant: never appears in the symtab */
    int        sec;        /* section index for defined, -1 = UND, -2 = ABS */
    int        in_symtab;  /* already queued for the .symtab emission */
} Sym;

static Sym  g_syms[2048];
static int  g_nsyms = 0;

/* SH4d: symbol-table emission order.  nasm emits [NULL][FILE][SECTION...]
 * then all LOCAL symbols in definition order, then all GLOBAL symbols in
 * the order their entries are created (extern declaration, or label
 * definition for `global` symbols).  We keep the two lists separately. */
static Sym *g_locals[2048];   /* in definition order */
static int  g_nlocals = 0;
static Sym *g_globals[2048];  /* in extern/definition order */
static int  g_nglobals = 0;

/* ---- ELF64 output (SH4d) ---- */
static int g_fmt_elf = 0;     /* -f elf64 */
static int g_default_rel = 0; /* `default rel` */
static const char *g_file_sym = NULL;  /* --file-sym: override the FILE symbol (SH4e in-guest refs) */

#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4
#define SHT_NULL      0
#define SHT_PROGBITS  1
#define SHT_SYMTAB    2
#define SHT_STRTAB    3
#define SHT_RELA      4
#define SHT_NOBITS    8
#define R_X86_64_64   1
#define R_X86_64_PC32 2
#define SHN_UNDEF     0
#define SHN_ABS       0xFFF1

typedef struct {
    char    *name;
    int      type;          /* SHT_* */
    int      flags;         /* SHF_* */
    int      align;         /* section alignment (max of defaults + aligns) */
    long long size;         /* logical size */
    int      secidx;        /* 1-based section-header index */
    uint8_t *data;          /* emitted bytes (NOBITS: NULL) */
    size_t   len, cap;      /* data len */
} Section;

static Section g_secs[16];
static int g_nsecs = 0;
static int g_cursec = -1;   /* index in g_secs, -1 = none (bin) */

/* ELF relocations (R_X86_64_64 / R_X86_64_PC32), collected at final emit. */
typedef struct {
    uint32_t  off;          /* offset within the section */
    int       type;
    int       tsec;         /* target is the SECTION symbol of this section, or -1 */
    Sym      *tsym;         /* target is this symbol (extern), when tsec < 0 */
    long long addend;
    int       sec;          /* section the relocation applies to */
} Reloc;

static Reloc g_relocs[65536];
static int g_nrelocs = 0;

static int parse_symref(const char *expr, char *symname, size_t symsz);

static void reloc_add(int sec, uint32_t off, int type, int tsec, Sym *tsym,
                      long long addend) {
    if (g_nrelocs >= 65536) die("relocation table overflow");
    Reloc *r = &g_relocs[g_nrelocs++];
    r->off = off; r->type = type; r->tsec = tsec; r->tsym = tsym;
    r->addend = addend; r->sec = sec;
}

/* ---- preprocessor text macros (%define / %assign) ---- */
typedef struct {
    char *name;
    char *text;             /* %define: raw text; %assign: decimal text */
} TextMacro;

static TextMacro g_tmacros[1024];
static int g_ntmacros = 0;

static TextMacro *tmacro_find(const char *name) {
    for (int i = 0; i < g_ntmacros; i++)
        if (!strcmp(g_tmacros[i].name, name)) return &g_tmacros[i];
    return NULL;
}

static void tmacro_set(const char *name, const char *text) {
    TextMacro *m = tmacro_find(name);
    if (m) { free(m->text); m->text = xstrdup(text); return; }
    if (g_ntmacros >= 1024) die("text-macro table overflow");
    g_tmacros[g_ntmacros].name = xstrdup(name);
    g_tmacros[g_ntmacros].text = xstrdup(text);
    g_ntmacros++;
}

/* ---- macro definitions (%macro ... %endmacro) ---- */
typedef struct {
    char    name[64];
    int     nparams;
    char  (*body)[512];
    int     nbody;
} MacroDef;

static MacroDef g_macrodefs[64];
static int g_nmacrodefs = 0;

static MacroDef *macrodef_find(const char *name) {
    for (int i = 0; i < g_nmacrodefs; i++)
        if (!strcmp(g_macrodefs[i].name, name)) return &g_macrodefs[i];
    return NULL;
}

static Sym *sym_find(const char *name) {
    for (int i = 0; i < g_nsyms; i++)
        if (strcmp(g_syms[i].name, name) == 0)
            return &g_syms[i];
    return NULL;
}

static Sym *sym_intern(const char *name) {
    Sym *s = sym_find(name);
    if (s) return s;
    if (g_nsyms >= 2048) die("symbol table overflow");
    g_syms[g_nsyms].name = xstrdup(name);
    g_syms[g_nsyms].val = 0;
    g_syms[g_nsyms].defined = 0;
    g_syms[g_nsyms].is_global = 0;
    g_syms[g_nsyms].is_extern = 0;
    g_syms[g_nsyms].is_equ = 0;
    g_syms[g_nsyms].sec = -1;
    g_syms[g_nsyms].in_symtab = 0;
    return &g_syms[g_nsyms++];
}

/* SH4d: symtab emission order.  Locals queue at definition; globals queue
 * at the first of {extern declaration, label definition}. */
static void sym_local_add(Sym *s) {
    if (s->in_symtab) return;
    if (g_nlocals >= 2048) die("local symbol table overflow");
    s->in_symtab = 1;
    g_locals[g_nlocals++] = s;
}

static void sym_global_add(Sym *s) {
    if (s->in_symtab) return;
    if (g_nglobals >= 2048) die("global symbol table overflow");
    s->in_symtab = 1;
    g_globals[g_nglobals++] = s;
}

/* ---- ELF sections (SH4d) ---- */

static int sec_default_align(const char *name) {
    if (!strcmp(name, ".text")) return 16;
    return 4;   /* .rodata / .data / .bss and unknown sections */
}

static int sec_find(const char *name) {
    for (int i = 0; i < g_nsecs; i++)
        if (!strcmp(g_secs[i].name, name)) return i;
    return -1;
}

static int sec_add(const char *name) {
    if (g_nsecs >= 16) die("too many sections");
    Section *S = &g_secs[g_nsecs];
    memset(S, 0, sizeof *S);
    S->name = xstrdup(name);
    S->align = sec_default_align(name);
    if (!strcmp(name, ".bss"))      { S->type = SHT_NOBITS; S->flags = SHF_WRITE | SHF_ALLOC; }
    else if (!strcmp(name, ".text")){ S->type = SHT_PROGBITS; S->flags = SHF_ALLOC | SHF_EXECINSTR; }
    else if (!strcmp(name, ".data")){ S->type = SHT_PROGBITS; S->flags = SHF_WRITE | SHF_ALLOC; }
    else if (!strcmp(name, ".rodata")){ S->type = SHT_PROGBITS; S->flags = SHF_ALLOC; }
    else                              { S->type = SHT_PROGBITS; S->flags = SHF_ALLOC; }
    S->secidx = g_nsecs + 1;   /* section header index (1-based, null is 0) */
    return g_nsecs++;
}

static int sec_find_or_add(const char *name) {
    int i = sec_find(name);
    return i >= 0 ? i : sec_add(name);
}

/* ------------------------------------------------------------------ */
/* output buffer                                                       */
/* ------------------------------------------------------------------ */

static uint8_t *g_out = NULL;
static size_t   g_out_len = 0;
static size_t   g_out_cap = 0;

static void out_reset(void) { g_out_len = 0; }

/* SH4d: reset per-section size + data at the start of every assembly pass
 * (the sizing loop repeats passes; without this, section sizes would
 * accumulate across iterations and every symbol value would drift). */
static void sections_reset(void) {
    for (int i = 0; i < g_nsecs; i++) {
        g_secs[i].size = 0;
        g_secs[i].len = 0;
    }
}

/* SH4d: per-section output for -f elf64.  In ELF mode every byte lands in
 * the current section's buffer; NOBITS sections (.bss) swallow bytes (the
 * callers still advance g_pc) because no data exists in the object file. */
static void sec_append(int sec, uint8_t b) {
    Section *S = &g_secs[sec];
    if (S->type == SHT_NOBITS) return;
    if (S->len >= S->cap) {
        S->cap = S->cap ? S->cap * 2 : 256;
        S->data = realloc(S->data, S->cap);
        if (!S->data) { fprintf(stderr, "mini-asm: oom\n"); exit(2); }
    }
    S->data[S->len++] = b;
}

static void out_byte(uint8_t b) {
    if (g_fmt_elf) {
        if (g_cursec < 0) die("byte emitted outside any section");
        sec_append(g_cursec, b);
        return;
    }
    if (g_out_len + 1 > g_out_cap) {
        g_out_cap = g_out_cap ? g_out_cap * 2 : 4096;
        g_out = realloc(g_out, g_out_cap);
        if (!g_out) { fprintf(stderr, "mini-asm: oom\n"); exit(2); }
    }
    g_out[g_out_len++] = b;
}

static void out_le(unsigned long long v, int nbytes) {
    for (int i = 0; i < nbytes; i++)
        out_byte((uint8_t)((v >> (8 * i)) & 0xFF));
}

/* ------------------------------------------------------------------ */
/* expression evaluator                                                */
/* ------------------------------------------------------------------ */

static const char *g_cur_global = "";   /* last non-local label (load time) */
static long long   g_pc  = 0;           /* current address ($) */
static long long   g_org = 0;           /* section/org base ($$) */
static int         g_allow_undef = 0;   /* sizing passes tolerate fwd refs */

typedef struct { const char *p; } Expr;

static void skip_ws(Expr *e) {
    while (*e->p == ' ' || *e->p == '\t') e->p++;
}

static long long parse_expr(Expr *e);

static int is_ident_start(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '.' || c == '@' || c == '?';
}
static int is_ident_char(int c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static long long parse_rel(Expr *e);
static long long parse_land(Expr *e);
static long long parse_lor(Expr *e);

static long long parse_atom(Expr *e) {
    skip_ws(e);
    const char *s = e->p;

    if (*s == '(') {
        e->p++;
        long long v = parse_expr(e);
        skip_ws(e);
        if (*e->p != ')') die("expected ')'");
        e->p++;
        return v;
    }
    if (*s == '-') { e->p++; return -parse_atom(e); }
    if (*s == '+') { e->p++; return  parse_atom(e); }
    if (*s == '~') { e->p++; return ~parse_atom(e); }

    if (s[0] == '$') {
        if (s[1] == '$') { e->p += 2; return g_org; }
        e->p += 1; return g_pc;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        e->p += 2;
        long long v = 0; int any = 0;
        for (;;) {
            int c = *e->p, d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = v * 16 + d; any = 1; e->p++;
        }
        if (!any) die("malformed hex literal");
        return v;
    }
    if (*s == '\'') {
        e->p++;
        long long v = 0;
        while (*e->p && *e->p != '\'') { v = (v << 8) | (uint8_t)*e->p; e->p++; }
        if (*e->p != '\'') die("unterminated char literal");
        e->p++;
        return v;
    }
    if (*s >= '0' && *s <= '9') {
        long long v = 0;
        while (*e->p >= '0' && *e->p <= '9') { v = v * 10 + (*e->p - '0'); e->p++; }
        return v;
    }
    if (is_ident_start((uint8_t)*s)) {
        char buf[256]; int n = 0;
        while (is_ident_char((uint8_t)*e->p) && n < 255) buf[n++] = *e->p++;
        buf[n] = 0;
        char full[300];
        const char *key = buf;
        if (buf[0] == '.') {
            snprintf(full, sizeof full, "%s%s", g_cur_global, buf);
            key = full;
        }
        Sym *sym = sym_find(key);
        if (!sym || !sym->defined) {
            if (g_allow_undef) return 0;
            die("undefined symbol '%s'", key);
        }
        return sym->val;
    }
    die("cannot parse expression near '%.16s'", s);
    return 0;
}

static long long parse_mul(Expr *e) {
    long long v = parse_atom(e);
    for (;;) {
        skip_ws(e);
        char c = *e->p;
        if (c == '*') { e->p++; v *= parse_atom(e); }
        else if (c == '/') {
            e->p++; long long d = parse_atom(e);
            if (!d) die("division by zero");
            v /= d;
        }
        else if (c == '%') {
            e->p++; long long d = parse_atom(e);
            if (!d) die("modulo by zero");
            v %= d;
        }
        else if (c == '<' && e->p[1] == '<') { e->p += 2; v <<= parse_atom(e); }
        else if (c == '>' && e->p[1] == '>') { e->p += 2; v >>= parse_atom(e); }
        else break;
    }
    return v;
}
static long long parse_add(Expr *e) {
    long long v = parse_mul(e);
    for (;;) {
        skip_ws(e);
        char c = *e->p;
        if (c == '+') { e->p++; v += parse_mul(e); }
        else if (c == '-') { e->p++; v -= parse_mul(e); }
        else break;
    }
    return v;
}
static long long parse_bitor(Expr *e) {
    long long v = parse_add(e);
    for (;;) {
        skip_ws(e);
        char c = *e->p;
        /* `||` belongs to the logical layer above; never eat it here */
        if (c == '|' && e->p[1] != '|') { e->p++; v |= parse_add(e); }
        else if (c == '^') { e->p++; v ^= parse_add(e); }
        else break;
    }
    return v;
}
static long long parse_bitand(Expr *e) {
    long long v = parse_bitor(e);
    for (;;) {
        skip_ws(e);
        if (*e->p == '&' && e->p[1] != '&') { e->p++; v &= parse_bitor(e); }
        else break;
    }
    return v;
}
/* relational / equality (for %if).  Shifts (<< >>) are handled deeper in
 * parse_mul, so a lone < or > here is a comparison. */
static long long parse_land(Expr *e) {
    long long v = parse_rel(e);
    for (;;) {
        skip_ws(e);
        /* parse the right side unconditionally: `v && rhs` short-circuits in
         * C and would leave e->p unmoved when v is 0 (a %if must always
         * consume its full expression). */
        if (e->p[0] == '&' && e->p[1] == '&') { e->p += 2; long long r = parse_rel(e); v = v && r; }
        else break;
    }
    return v;
}

static long long parse_lor(Expr *e) {
    long long v = parse_land(e);
    for (;;) {
        skip_ws(e);
        if (e->p[0] == '|' && e->p[1] == '|') { e->p += 2; long long r = parse_land(e); v = v || r; }
        else break;
    }
    return v;
}

static long long parse_rel(Expr *e) {
    long long v = parse_bitand(e);
    for (;;) {
        skip_ws(e);
        const char *s = e->p;
        int op = 0;
        if (s[0]=='<' && s[1]=='=') { op=1; e->p+=2; }
        else if (s[0]=='>' && s[1]=='=') { op=2; e->p+=2; }
        else if (s[0]=='=' && s[1]=='=') { op=3; e->p+=2; }
        else if (s[0]=='!' && s[1]=='=') { op=4; e->p+=2; }
        else if (s[0]=='<' && s[1]!='<') { op=5; e->p+=1; }
        else if (s[0]=='>' && s[1]!='>') { op=6; e->p+=1; }
        else break;
        long long r = parse_bitand(e);
        switch (op) {
            case 1: v = v <= r; break;
            case 2: v = v >= r; break;
            case 3: v = v == r; break;
            case 4: v = v != r; break;
            case 5: v = v <  r; break;
            case 6: v = v >  r; break;
        }
    }
    return v;
}

static long long parse_expr(Expr *e) { return parse_lor(e); }

static long long eval_str(const char *s) {
    Expr e; e.p = s;
    long long v = parse_expr(&e);
    skip_ws(&e);
    if (*e.p) die("trailing garbage in expression: '%.16s'", e.p);
    return v;
}

/* ------------------------------------------------------------------ */
/* registers                                                           */
/* ------------------------------------------------------------------ */

static int g_bits = 16;   /* current `bits` mode: 16, 32 or 64 */

static int reg8(const char *s) {
    static const char *n[] = {"al","cl","dl","bl","ah","ch","dh","bh"};
    for (int i = 0; i < 8; i++) if (!strcmp(s, n[i])) return i;
    return -1;
}
static int reg16(const char *s) {
    static const char *n[] = {"ax","cx","dx","bx","sp","bp","si","di"};
    for (int i = 0; i < 8; i++) if (!strcmp(s, n[i])) return i;
    return -1;
}
static int reg32(const char *s) {
    static const char *n[] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi"};
    for (int i = 0; i < 8; i++) if (!strcmp(s, n[i])) return i;
    return -1;
}
static int reg64(const char *s) {
    static const char *n[] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                              "r8","r9","r10","r11","r12","r13","r14","r15"};
    for (int i = 0; i < 16; i++) if (!strcmp(s, n[i])) return i;
    return -1;
}
static int sreg(const char *s) {
    static const char *n[] = {"es","cs","ss","ds","fs","gs"};
    for (int i = 0; i < 6; i++) if (!strcmp(s, n[i])) return i;
    return -1;
}
static int creg(const char *s) {
    static const char *n[] = {"cr0","cr1","cr2","cr3","cr4","cr5","cr6","cr7"};
    for (int i = 0; i < 8; i++) if (!strcmp(s, n[i])) return i;
    return -1;
}
static int modrm(int mod, int reg, int rm) {
    return ((mod & 3) << 6) | ((reg & 7) << 3) | (rm & 7);
}

/* Operand-size prefix (0x66) is needed when the operand width differs from
 * the mode's default (16 in bits16, 32 in bits32/bits64).  64-bit operands
 * use REX.W instead, and 8-bit operands never take it. */
static int need_66(int width) {
    if (width == 8 || width == 64) return 0;
    int defw = (g_bits == 16) ? 16 : 32;
    return width != defw;
}

/* Address-size override (0x67): needed when the effective-address registers
 * are a different width than the mode default (e.g. [edi] in bits 16). */
static int need_67(int aregs) {
    if (g_bits == 16) return aregs == 32;
    if (g_bits == 32) return aregs == 16;
    return 0;
}

/* ------------------------------------------------------------------ */
/* operands                                                            */
/* ------------------------------------------------------------------ */

enum { OP_NONE, OP_REG, OP_SREG, OP_CR, OP_MEM, OP_IMM, OP_FAR };

typedef struct {
    int kind;
    int reg;        /* register index (0-15) for OP_REG */
    int width;      /* 8/16/32/64 for OP_REG */
    /* OP_MEM fields: */
    int seg;        /* segment-override prefix byte, or 0 */
    int base;       /* base reg index (32-bit numbering), -1 none */
    int index;      /* index reg index, -1 none */
    int scale;      /* 1/2/4/8 */
    int aregs;      /* 16 or 32: width of the address registers used */
    int has_disp;
    int rel;        /* -1 none, 0 `abs`, 1 `rel` (SH4d) */
    int memsize;    /* 0, or 8/16/32/64 from a byte/word/dword/qword specifier */
    char disp[256]; /* displacement expression text */
    char text[256]; /* IMM/FAR text */
} Operand;

/* Split on commas that are outside brackets and quotes. */
static int split_operands(const char *s, char out[][256], int max) {
    int n = 0, depth = 0, inq = 0; char qc = 0;
    const char *start = s;
    for (; *s; s++) {
        char c = *s;
        if (inq) { if (c == qc) inq = 0; continue; }
        if (c == '"' || c == '\'') { inq = 1; qc = c; continue; }
        if (c == '[') { depth++; continue; }
        if (c == ']') { depth--; continue; }
        if (c == ',' && depth == 0) {
            const char *a = start; size_t len = (size_t)(s - start);
            while (len && (*a == ' ' || *a == '\t')) { a++; len--; }
            while (len && (a[len-1] == ' ' || a[len-1] == '\t')) len--;
            if (len >= 256) len = 255;
            memcpy(out[n], a, len); out[n][len] = 0;
            if (++n >= max) return n;
            s++;
            while (*s == ' ' || *s == '\t') s++;
            start = s;
            s--;   /* compensate for the for-loop's s++ so the next operand's
                    * first char (e.g. an opening quote) is still scanned */
        }
    }
    {
        const char *a = start; size_t len = strlen(start);
        while (len && (*a == ' ' || *a == '\t')) { a++; len--; }
        while (len && (a[len-1] == ' ' || a[len-1] == '\t')) len--;
        if (len || n > 0) {
            if (len >= 256) len = 255;
            memcpy(out[n], a, len); out[n][len] = 0;
            n++;
        }
    }
    return n;
}

/* Parse a memory operand's interior (between the brackets).  Handles an
 * optional segment override, base/index registers (with *scale), and a
 * displacement expression.  SH4c: stage2 needs [abs], [base+disp],
 * [seg:base+disp] and three [base+index] forms. */
static void parse_mem(const char *s, Operand *o) {
    o->kind = OP_MEM;
    o->seg = 0; o->base = -1; o->index = -1; o->scale = 1;
    o->aregs = 0; o->has_disp = 0; o->disp[0] = 0; o->rel = -1;

    static const struct { const char *n; int pre; } segs[] = {
        {"es",0x26},{"cs",0x2E},{"ss",0x36},{"ds",0x3E},{"fs",0x64},{"gs",0x65}
    };
    for (int i = 0; i < 6; i++) {
        size_t L = strlen(segs[i].n);
        if (!strncmp(s, segs[i].n, L) && s[L] == ':') {
            o->seg = segs[i].pre;
            s += L + 1;
            while (*s == ' ' || *s == '\t') s++;
            break;
        }
    }
    /* optional abs/rel addressing hint (nasm).  For -f bin we always emit
     * the absolute form; for -f elf64 the hint (or `default rel`) selects
     * RIP-relative mod00/rm101 for a symbol-only address. */
    if ((!strncmp(s, "abs", 3) || !strncmp(s, "rel", 3)) &&
        (s[3] == ' ' || s[3] == '\t')) {
        o->rel = (s[0] == 'r') ? 1 : 0;
        s += 3;
        while (*s == ' ' || *s == '\t') s++;
    }

    /* walk terms separated by top-level + / - */
    char disp[256]; disp[0] = 0;
    int sign = 1;            /* sign of the current term */
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '+') { sign = 1; p++; continue; }
        if (*p == '-') { sign = -1; p++; continue; }
        if (!*p) break;
        /* read one term up to the next top-level + or - */
        const char *ts = p;
        int depth = 0;
        while (*p) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            else if ((*p == '+' || *p == '-') && depth == 0) break;
            p++;
        }
        size_t tl = (size_t)(p - ts);
        while (tl && (ts[tl-1] == ' ' || ts[tl-1] == '\t')) tl--;
        char term[256];
        if (tl >= sizeof term) tl = sizeof term - 1;
        memcpy(term, ts, tl); term[tl] = 0;

        /* is the term "reg" or "reg*scale" (or "(reg + const)*scale")? */
        char *star = strchr(term, '*');
        char regname[64]; int scale = 1; int isreg = 0; int r = -1;
        if (star) {
            size_t rl = (size_t)(star - term);
            while (rl && (term[rl-1]==' '||term[rl-1]=='\t')) rl--;
            if (rl < sizeof regname) {
                memcpy(regname, term, rl); regname[rl] = 0;
                char *sc = star + 1; while (*sc==' '||*sc=='\t') sc++;
                scale = (int)eval_str(sc);
                /* SH4e: "(reg + const)*scale" -> index=reg, disp += const*scale.
                 * Only a leading bare register may be an index, so peel the
                 * parens and fold the constant into the displacement. */
                if (regname[0] == '(') {
                    size_t rl2 = strlen(regname);
                    if (rl2 >= 2 && regname[rl2-1] == ')') {
                        char inner[128];
                        size_t il = rl2 - 2;
                        if (il >= sizeof inner) il = sizeof inner - 1;
                        memcpy(inner, regname + 1, il); inner[il] = 0;
                        char *plus = strchr(inner, '+');
                        char *minus = strchr(inner, '-');
                        char *op = plus ? plus : minus;
                        if (op) {
                            char regpart[64]; size_t pl = (size_t)(op - inner);
                            while (pl && (inner[pl-1]==' '||inner[pl-1]=='\t')) pl--;
                            if (pl < sizeof regpart) {
                                memcpy(regpart, inner, pl); regpart[pl] = 0;
                                char *cpart = op + 1; while (*cpart==' '||*cpart=='\t') cpart++;
                                long long cst = eval_str(cpart);
                                if (plus) cst = +cst; else cst = -cst;
                                /* fold const*scale into the disp string */
                                char fold[256];
                                snprintf(fold, sizeof fold, "%s%lld", disp[0] ? "+" : "", cst * scale);
                                if (strlen(disp) + strlen(fold) + 1 > sizeof disp)
                                    die("memory displacement too long");
                                strcat(disp, fold);
                                snprintf(regname, sizeof regname, "%.63s", regpart);
                            }
                        } else {
                            snprintf(regname, sizeof regname, "%.63s", inner);
                        }
                    }
                }
                isreg = 1;
            }
        } else {
            /* whole term might be a bare register */
            char *t2 = term; while (*t2==' '||*t2=='\t') t2++;
            size_t rl = strlen(t2);
            while (rl && (t2[rl-1]==' '||t2[rl-1]=='\t')) rl--;
            if (rl < sizeof regname) {
                memcpy(regname, t2, rl); regname[rl] = 0;
                isreg = 1;
            }
        }
        if (isreg) {
            int w = 0;
            if ((r = reg16(regname)) >= 0) w = 16;
            else if ((r = reg32(regname)) >= 0) w = 32;
            else if ((r = reg64(regname)) >= 0) w = 64;   /* SH4d: 64-bit bases */
            else isreg = 0;
            if (isreg) {
                /* address-size class: 16 if any 16-bit reg, else 32 (even
                 * for 64-bit registers -- the address size is 32) */
                if (o->aregs == 0) o->aregs = (w == 16) ? 16 : 32;
                if (star) { o->index = r; o->scale = scale; }
                else if (o->base < 0) o->base = r;
                else { o->index = r; o->scale = scale; }   /* second bare reg = index */
                continue;
            }
        }
        /* not a register: fold into the displacement expression */
        if (term[0]) {
            const char *pre = sign < 0 ? "-" : (disp[0] ? "+" : "");
            if (strlen(disp) + strlen(pre) + strlen(term) + 1 > sizeof disp)
                die("memory displacement expression too long");
            strcat(disp, pre);
            strcat(disp, term);
        }
    }
    if (disp[0]) { o->has_disp = 1; snprintf(o->disp, sizeof o->disp, "%s", disp); }
    if (o->aregs == 0) o->aregs = (g_bits == 16) ? 16 : 32;
}

static void parse_operand(const char *s, Operand *o) {
    memset(o, 0, sizeof *o);
    o->kind = OP_NONE; o->reg = 0; o->width = 0; o->base = -1; o->index = -1;
    o->scale = 1; o->rel = -1; o->text[0] = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return;
    /* optional memory size specifier: byte/word/dword/qword [...] */
    {
        static const struct { const char *n; int sz; } szs[] = {
            {"qword",64},{"dword",32},{"word",16},{"byte",8}
        };
        for (int i = 0; i < 4; i++) {
            size_t L = strlen(szs[i].n);
            if (!strncmp(s, szs[i].n, L)) {
                const char *t = s + L;
                while (*t == ' ' || *t == '\t') t++;
                if (*t == '[') { o->memsize = szs[i].sz; s = t; break; }
            }
        }
    }
    if (*s == '[') {
        size_t n = strlen(s);
        if (s[n-1] != ']') die("malformed memory operand '%s'", s);
        char inner[300];
        if (n - 2 >= sizeof inner) die("memory operand too long");
        memcpy(inner, s + 1, n - 2);
        inner[n-2] = 0;
        parse_mem(inner, o);
        return;
    }
    int r;
    if ((r = reg8(s)) >= 0)  { o->kind = OP_REG; o->reg = r; o->width = 8;  return; }
    if ((r = reg16(s)) >= 0) { o->kind = OP_REG; o->reg = r; o->width = 16; return; }
    if ((r = reg32(s)) >= 0) { o->kind = OP_REG; o->reg = r; o->width = 32; return; }
    if ((r = reg64(s)) >= 0) { o->kind = OP_REG; o->reg = r; o->width = 64; return; }
    if ((r = sreg(s)) >= 0)  { o->kind = OP_SREG; o->reg = r; return; }
    if ((r = creg(s)) >= 0)  { o->kind = OP_CR; o->reg = r; return; }
    for (const char *q = s; *q; q++)
        if (*q == ':') {
            const char *t = s;
            static const struct { const char *n; int sz; } fk[] = {
                {"qword",64},{"dword",32},{"word",16}
            };
            for (int i = 0; i < 3; i++) {
                size_t L = strlen(fk[i].n);
                if (!strncmp(t, fk[i].n, L) && (t[L]==' '||t[L]=='\t')) {
                    o->memsize = fk[i].sz; t += L;
                    while (*t==' '||*t=='\t') t++;
                    break;
                }
            }
            o->kind = OP_FAR; snprintf(o->text, sizeof o->text, "%s", t); return;
        }
    o->kind = OP_IMM;
    snprintf(o->text, sizeof o->text, "%s", s);
}

static int jcc_cc(const char *m) {
    static const struct { const char *n; int cc; } t[] = {
        {"jo",0},{"jno",1},{"jb",2},{"jc",2},{"jnae",2},{"jnb",3},{"jnc",3},
        {"jae",3},{"jz",4},{"je",4},{"jnz",5},{"jne",5},{"jbe",6},{"jna",6},
        {"ja",7},{"jnbe",7},{"js",8},{"jns",9},{"jp",10},{"jpe",10},
        {"jnp",11},{"jpo",11},{"jl",12},{"jnge",12},{"jge",13},{"jnl",13},
        {"jle",14},{"jng",14},{"jg",15},{"jnle",15},{NULL,0}
    };
    for (int i = 0; t[i].n; i++) if (!strcmp(m, t[i].n)) return t[i].cc;
    return -1;
}

/* ------------------------------------------------------------------ */
/* per-line model                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char *label;
    char *mnem;
    char  ops[6][256];
    int   nops;
    int   line_no;
    int   is_equ;
    char *equ_expr;
    int   is_jump;
    int   jump_long;
    int   jump_far;
    char *jump_target;
    char *global_set;   /* if this line defines a global label, its name */
} AsmLine;

static AsmLine *g_lines = NULL;
static int      g_nlines = 0;
static int      g_caplines = 0;

static AsmLine *new_line(void) {
    if (g_nlines >= g_caplines) {
        g_caplines = g_caplines ? g_caplines * 2 : 256;
        g_lines = realloc(g_lines, g_caplines * sizeof *g_lines);
        if (!g_lines) { fprintf(stderr, "mini-asm: oom\n"); exit(2); }
    }
    AsmLine *L = &g_lines[g_nlines++];
    memset(L, 0, sizeof *L);
    return L;
}

static char *lower(const char *s) {
    char *p = xstrdup(s);
    for (char *q = p; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
    return p;
}

/* ------------------------------------------------------------------ */
/* loading                                                             */
/* ------------------------------------------------------------------ */

static char *read_whole(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "mini-asm: cannot open '%s'\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "mini-asm: short read\n"); exit(2); }
    buf[n] = 0;
    fclose(f);
    return buf;
}

static void strip_comment(char *s) {
    int in_s = 0; char q = 0;
    for (char *p = s; *p; p++) {
        if (in_s) { if (*p == q) in_s = 0; continue; }
        if (*p == '"' || *p == '\'') { in_s = 1; q = *p; continue; }
        if (*p == ';') { *p = 0; return; }
    }
}

static void load_line(char *text, int line_no) {
    strip_comment(text);
    size_t n = strlen(text);
    while (n && (text[n-1] == '\r' || text[n-1] == '\n' ||
                 text[n-1] == ' '  || text[n-1] == '\t')) text[--n] = 0;

    char *p = text;
    while (*p == ' ' || *p == '\t') p++;

    AsmLine *L = new_line();
    L->line_no = line_no;

    /* leading label(s) */
    for (;;) {
        char *save = p;
        if (is_ident_start((uint8_t)*p)) {
            char *st = p;
            while (is_ident_char((uint8_t)*p)) p++;
            char *after = p;
            while (*after == ' ' || *after == '\t') after++;
            if (*after == ':') {
                *p = 0;
                char full[300];
                const char *nm = st;
                if (st[0] == '.') snprintf(full, sizeof full, "%s%s", g_cur_global, st);
                else { g_cur_global = xstrdup(st); L->global_set = xstrdup(st); }
                if (st[0] == '.') nm = full;
                L->label = xstrdup(nm);
                p = after + 1;
                while (*p == ' ' || *p == '\t') p++;
                continue;
            }
            p = save;
        }
        break;
    }

    if (!*p) return;

    char *ms = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    char *me = p;
    while (*p == ' ' || *p == '\t') p++;
    *me = 0;

    /* "NAME equ EXPR" -- keep the name's case; it is a symbol, not a mnemonic */
    if (!strncmp(p, "equ", 3) && (p[3] == ' ' || p[3] == '\t')) {
        L->is_equ = 1;
        L->mnem = xstrdup(ms);
        char *e = p + 3;
        while (*e == ' ' || *e == '\t') e++;
        L->equ_expr = xstrdup(e);
        return;
    }

    L->mnem = lower(ms);
    L->nops = split_operands(p, L->ops, 6);

    int disp_only = jcc_cc(L->mnem) >= 0 || !strcmp(L->mnem, "loop") ||
                    !strcmp(L->mnem, "loope") || !strcmp(L->mnem, "loopne") ||
                    !strcmp(L->mnem, "jcxz");
    if (!strcmp(L->mnem, "jmp") || !strcmp(L->mnem, "call") || disp_only) {
        if (L->nops == 1) {
            int is_reg = reg8(L->ops[0]) >= 0 || reg16(L->ops[0]) >= 0 ||
                         reg32(L->ops[0]) >= 0 || reg64(L->ops[0]) >= 0;
            int is_mem = strchr(L->ops[0], '[') != NULL;   /* SH4d: [mem] indirect */
            int is_far = 0;
            for (const char *q = L->ops[0]; *q; q++) if (*q == ':') { is_far = 1; break; }
            if (!disp_only && (is_reg || is_mem)) {
                L->is_jump = 0;   /* indirect jmp/call reg/[mem]: fixed size, encode_instr handles it */
            } else {
                L->is_jump = 1;
                L->jump_far = disp_only ? 0 : is_far;
                if (!L->jump_far) {
                    const char *t = L->ops[0];
                    char full[300];
                    if (t[0] == '.') snprintf(full, sizeof full, "%s%s", g_cur_global, t);
                    else snprintf(full, sizeof full, "%s", t);
                    L->jump_target = xstrdup(full);
                    sym_intern(full);
                }
            }
        }
    }
}

/* ---- include paths (-I) ---- */
static const char *g_incdirs[16];
static int g_nincdirs = 0;

/* ---- preprocessed line list ---- */
typedef struct { char *text; int line_no; } PLine;
static PLine *g_plines = NULL;
static int g_nplines = 0, g_capplines = 0;

static void pline_add(const char *text, int line_no) {
    if (g_nplines >= g_capplines) {
        g_capplines = g_capplines ? g_capplines * 2 : 1024;
        g_plines = realloc(g_plines, (size_t)g_capplines * sizeof(PLine));
        if (!g_plines) { fprintf(stderr, "mini-asm: oom\n"); exit(2); }
    }
    g_plines[g_nplines].text = xstrdup(text);
    g_plines[g_nplines].line_no = line_no;
    g_nplines++;
}

static char *find_include(const char *name) {
    FILE *f = fopen(name, "rb");
    if (f) { fclose(f); return xstrdup(name); }
    for (int i = 0; i < g_nincdirs; i++) {
        char buf[512];
        snprintf(buf, sizeof buf, "%s/%s", g_incdirs[i], name);
        f = fopen(buf, "rb");
        if (f) { fclose(f); return xstrdup(buf); }
    }
    return NULL;
}

/* Preprocess one file: expand %include (recursively, via -I paths), resolve
 * %if/%else/%endif, honour %error.  Emits the surviving lines to g_plines.
 * SH4c: the stage2 boot chain needs %include (13 .inc files) and one %if. */
/* ---- text-macro substitution (%define / %assign are preprocessor-time
 * ---- text macros, exactly like nasm: they are substituted into lines
 * ---- BEFORE assembly, which is what makes `%rep` + `%assign` counters +
 * ---- `%macro` work: `TABLE_ENTRY i` becomes `TABLE_ENTRY 0` first. ---- */

static int ident_char(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '.';
}

static char *subst_macros(const char *s) {
    /* iterate to a fixed point so %define A B; %define B 1 resolves A; a
     * bounded loop defeats accidental cycles the same way the jump sizing
     * loop does. */
    char *cur = xstrdup(s);
    for (int pass = 0; pass < 8; pass++) {
        int changed = 0;
        char out[8192]; size_t n = 0;
        const char *p = cur;
        while (*p) {
            /* SH4e: nasm `%+` token pasting (`isr_stub_%+v` -> `isr_stub_0`).
             * The left token is the identifier already sitting at the end of
             * `out`; the right token is resolved through the text macros. */
            if (p[0] == '%' && p[1] == '+') {
                size_t lstart = n;
                while (lstart > 0 && ident_char(out[lstart-1])) lstart--;
                p += 2;
                const char *rt = p;
                while (ident_char(*p)) p++;
                char rname[256];
                size_t rl = (size_t)(p - rt);
                if (rl >= sizeof rname) rl = sizeof rname - 1;
                memcpy(rname, rt, rl); rname[rl] = 0;
                const char *resolved = rname;
                TextMacro *rm = tmacro_find(rname);
                if (rm) resolved = rm->text;
                size_t r2 = strlen(resolved);
                if (n + r2 + 1 > sizeof out) die("substitution too long");
                memcpy(out + n, resolved, r2); n += r2;
                changed = 1;
                continue;
            }
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_' ||
                *p == '$' || (*p == '.' && p[1] && ident_char(p[1]))) {
                const char *st = p;
                while (ident_char(*p)) p++;
                size_t L = (size_t)(p - st);
                char name[256];
                if (L >= sizeof name) L = sizeof name - 1;
                memcpy(name, st, L); name[L] = 0;
                TextMacro *m = tmacro_find(name);
                if (m) {
                    size_t tl = strlen(m->text);
                    if (n + tl + 1 > sizeof out) die("substitution too long");
                    memcpy(out + n, m->text, tl);
                    n += tl;
                    changed = 1;
                    continue;
                }
                if (n + L + 1 > sizeof out) die("substitution too long");
                memcpy(out + n, st, L); n += L;
                continue;
            }
            if (n + 1 >= sizeof out) die("substitution too long");
            out[n++] = *p++;
        }
        out[n] = 0;
        if (!changed) { free(cur); return xstrdup(out); }
        free(cur);
        cur = xstrdup(out);
    }
    return cur;
}

static void macrodef_register(const char *name, int nparams, char (*body)[512], int nbody) {
    if (g_nmacrodefs >= 64) die("too many %%macro definitions");
    MacroDef *md = &g_macrodefs[g_nmacrodefs++];
    snprintf(md->name, sizeof md->name, "%s", name);
    md->nparams = nparams;
    md->body = NULL;
    md->nbody = nbody;
    if (nbody > 0) {
        md->body = xmalloc((size_t)nbody * 512);
        for (int i = 0; i < nbody; i++) memcpy(md->body[i], body[i], 512);
    }
}

/* split the argument tail after a macro name on top-level commas */
static void split_args(const char *s, char out[][128], int max, int *n) {
    *n = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return;
    while (*s && *n < max) {
        const char *st = s;
        while (*s && *s != ',') s++;
        size_t L = (size_t)(s - st);
        while (L && (st[L-1]==' '||st[L-1]=='\t')) L--;
        if (L >= 128) L = 127;
        memcpy(out[*n], st, L); out[*n][L] = 0;
        (*n)++;
        if (*s == ',') s++;
    }
}

typedef struct { char *text; int line_no; } SrcLine;

static void pline_add(const char *text, int line_no);

static void pp_lines(SrcLine *lines, int n);

static void macro_invoke(MacroDef *md, const char *argtail, int line_no);

/* substitute %1..%N (and %% -> %) in a macro body line */
static void macro_subst(const char *in, char *out, size_t outsz,
                        char args[][128], int nargs) {
    size_t n = 0;
    const char *p = in;
    while (*p && n + 1 < outsz) {
        if (p[0] == '%' && p[1] == '%') { out[n++] = '%'; p += 2; continue; }
        if (p[0] == '%' && p[1] >= '1' && p[1] <= '9') {
            int idx = p[1] - '1';
            const char *a = (idx < nargs) ? args[idx] : "";
            size_t al = strlen(a);
            if (n + al + 1 > outsz) die("macro substitution overflow");
            memcpy(out + n, a, al); n += al;
            p += 2;
            continue;
        }
        out[n++] = *p++;
    }
    out[n] = 0;
}

static void macro_invoke(MacroDef *md, const char *argtail, int line_no) {
    char args[8][128];
    int nargs = 0;
    split_args(argtail, args, 8, &nargs);
    for (int i = 0; i < md->nbody; i++) {
        char expanded[512];
        macro_subst(md->body[i], expanded, sizeof expanded, args, nargs);
        SrcLine l;
        l.text = expanded;
        l.line_no = line_no;
        pp_lines(&l, 1);
    }
}

/* Process an array of source lines.  Recurses for %include, %rep bodies
 * and %macro invocations, so each invocation level has its own %if stack.
 * The tree's files never nest %if inside %rep/%macro bodies, so the simple
 * per-level stacks are faithful to the dialect we support. */
static void pp_lines(SrcLine *lines, int n) {
    int taking[64], taken[64], parent[64], sp = 0;

    /* %macro definition collection (body on the heap: 512x512 on the stack
     * would join the %rep body below at >2 MiB per frame -- fine on a host,
     * but the in-guest user stack is only 4 MiB and %rep 256 recursion
     * pushed it over). */
    char mdef_name[64]; int mdef_params = 0;
    char (*mdef_body)[512] = NULL; int mdef_nbody = 0;
    int in_mdef = 0;

    /* %rep collection */
    long long rep_count = 0;
    char (*rep_body)[512] = NULL; int rep_nbody = 0;
    int in_rep = 0;

    for (int i = 0; i < n; i++) {
        char *s = lines[i].text;
        int ln = lines[i].line_no;
        g_line_no = ln;
        char *t = s;
        while (*t == ' ' || *t == '\t') t++;
        int cur = (sp == 0) ? 1 : taking[sp - 1];

        if (in_mdef) {
            if (!strncmp(t, "%endmacro", 9) &&
                (t[9] == 0 || t[9] == ' ' || t[9] == '\t')) {
                if (!cur) die("%%endmacro inside an inactive %%if branch (unsupported)");
                macrodef_register(mdef_name, mdef_params, mdef_body, mdef_nbody);
                in_mdef = 0;
            } else if (cur) {
                if (mdef_nbody >= 512) die("%%macro body too large");
                if (!mdef_body) mdef_body = xmalloc(512 * 512);
                snprintf(mdef_body[mdef_nbody], 512, "%s", s);
                mdef_nbody++;
            }
            continue;
        }
        if (in_rep) {
            if (!strncmp(t, "%endrep", 7) &&
                (t[7] == 0 || t[7] == ' ' || t[7] == '\t')) {
                if (!cur) die("%%endrep inside an inactive %%if branch (unsupported)");
                for (long long k = 0; k < rep_count; k++) {
                    SrcLine *bl = xmalloc((size_t)rep_nbody * sizeof(SrcLine));
                    for (int j = 0; j < rep_nbody; j++) {
                        bl[j].text = rep_body[j];
                        bl[j].line_no = ln;
                    }
                    pp_lines(bl, rep_nbody);
                    free(bl);
                }
                in_rep = 0;
            } else if (cur) {
                if (rep_nbody >= 4096) die("%%rep body too large");
                if (!rep_body) rep_body = xmalloc(4096 * 512);
                snprintf(rep_body[rep_nbody], 512, "%s", s);
                rep_nbody++;
            }
            continue;
        }

        if (!strncmp(t, "%include", 8) && (t[8]==' '||t[8]=='\t'||t[8]=='"')) {
            if (cur) {
                char *q = t + 8; while (*q==' '||*q=='\t') q++;
                if (*q != '"' && *q != '\'') die("malformed %%include");
                char qc = *q++; char *st = q;
                while (*q && *q != qc) q++;
                if (*q != qc) die("unterminated %%include name");
                *q = 0;
                char *full = find_include(st);
                if (!full) die("cannot open include file '%s'", st);
                char *buf = read_whole(full);
                free(full);
                /* split into lines and process recursively */
                int cnt = 1;
                for (char *c = buf; *c; c++) if (*c == '\n') cnt++;
                SrcLine *sub = xmalloc((size_t)cnt * sizeof(SrcLine));
                int m = 0;
                char *p2 = buf;
                while (*p2) {
                    char *nl = strchr(p2, '\n');
                    if (nl) *nl = 0;
                    sub[m].text = p2;
                    sub[m].line_no = ln;
                    m++;
                    if (!nl) break;
                    p2 = nl + 1;
                }
                pp_lines(sub, m);
                free(sub);
                free(buf);
            }
            continue;
        }
        if (!strncmp(t, "%if", 3) && (t[3]==' '||t[3]=='\t')) {
            parent[sp] = cur;
            int cond = 0;
            if (cur) {
                char *e = t + 3; while (*e==' '||*e=='\t') e++;
                char *ex = subst_macros(e);
                cond = eval_str(ex) != 0;
                free(ex);
            }
            taking[sp] = cur && cond;
            taken[sp]  = cond;
            sp++;
            continue;
        }
        if (!strncmp(t, "%elif", 5)) {
            die("%%elif not supported (SH4c subset)");
        }
        if (!strncmp(t, "%else", 5) && (t[5]==0||t[5]==' '||t[5]=='\t')) {
            if (sp > 0) {
                taking[sp-1] = parent[sp-1] && !taken[sp-1];
                if (taking[sp-1]) taken[sp-1] = 1;
            }
            continue;
        }
        if (!strncmp(t, "%endif", 6) && (t[6]==0||t[6]==' '||t[6]=='\t')) {
            if (sp > 0) sp--;
            continue;
        }
        if (!strncmp(t, "%error", 6) && (t[6]==0||t[6]==' '||t[6]=='\t')) {
            if (cur) {
                char *q = t + 6; while (*q==' '||*q=='\t') q++;
                die("%%error: %s", q);
            }
            continue;
        }
        if ((!strncmp(t, "%define", 7) && (t[7]==' '||t[7]=='\t')) ||
            (!strncmp(t, "%assign", 7) && (t[7]==' '||t[7]=='\t'))) {
            if (cur) {
                /* work on a local copy: %rep bodies are re-processed per
                 * iteration and must not be mutated by the first one */
                char lb[1024];
                snprintf(lb, sizeof lb, "%s", t);
                int is_assign = lb[1] == 'a';
                char *q = lb + 7; while (*q==' '||*q=='\t') q++;
                char *nm = q;
                while (*q && *q!=' ' && *q!='\t' && *q!='(') q++;
                if (*q == '(') die("function-like %%define not supported (SH4c subset)");
                int had = (*q != 0);
                if (had) { *q = 0; q++; while (*q==' '||*q=='\t') q++; }
                if (is_assign) {
                    char *ex = subst_macros(q);
                    long long v = eval_str(ex);
                    free(ex);
                    char buf[64];
                    snprintf(buf, sizeof buf, "%lld", v);
                    tmacro_set(nm, buf);
                } else {
                    char *ex = subst_macros(q);
                    tmacro_set(nm, ex);
                    free(ex);
                }
            }
            continue;
        }
        if (!strncmp(t, "%macro", 6) && (t[6]==' '||t[6]=='\t')) {
            if (cur) {
                char lb[1024];
                snprintf(lb, sizeof lb, "%s", t);
                char *q = lb + 6; while (*q==' '||*q=='\t') q++;
                char *nm = q;
                while (*q && *q!=' ' && *q!='\t') q++;
                if (*q) { *q = 0; q++; }
                while (*q==' '||*q=='\t') q++;
                mdef_params = (int)eval_str(q);
                if (mdef_params < 0 || mdef_params > 8) die("unsupported %%macro arity");
                snprintf(mdef_name, sizeof mdef_name, "%.63s", nm);
                mdef_nbody = 0;
                in_mdef = 1;
            }
            continue;
        }
        if (!strncmp(t, "%rep", 4) && (t[4]==' '||t[4]=='\t')) {
            if (cur) {
                char *q = t + 4; while (*q==' '||*q=='\t') q++;
                char *ex = subst_macros(q);
                rep_count = eval_str(ex);
                free(ex);
                if (rep_count < 0 || rep_count > 1000000) die("bad %%rep count");
                rep_nbody = 0;
                in_rep = 1;
            }
            continue;
        }
        if (!strncmp(t, "%endrep", 7)) die("%%endrep without %%rep");
        if (!strncmp(t, "%endmacro", 9)) die("%%endmacro without %%macro");

        if (cur) {
            char *ex = subst_macros(s);
            char *tk = ex;
            while (*tk == ' ' || *tk == '\t') tk++;
            char *tke = tk;
            while (*tke && *tke != ' ' && *tke != '\t' && *tke != ':') tke++;
            char nm[64];
            size_t tl = (size_t)(tke - tk);
            if (tl >= sizeof nm) tl = sizeof nm - 1;
            memcpy(nm, tk, tl); nm[tl] = 0;
            MacroDef *md = macrodef_find(nm);
            if (md) {
                char *argtail = tke;
                while (*argtail == ' ' || *argtail == '\t') argtail++;
                macro_invoke(md, argtail, ln);
                free(ex);
            } else {
                pline_add(ex, ln);
            }
        }
    }
    if (sp != 0) die("unterminated %%if");
    if (in_mdef) die("unterminated %%macro");
    if (in_rep) die("unterminated %%rep");
    free(mdef_body);
    free(rep_body);
}

static void preprocess(const char *path, int depth) {
    if (depth > 32) die("%%include nested too deep at '%s'", path);
    char *buf = read_whole(path);
    int cnt = 1;
    for (char *c = buf; *c; c++) if (*c == '\n') cnt++;
    SrcLine *lines = xmalloc((size_t)cnt * sizeof(SrcLine));
    int n = 0;
    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        lines[n].text = p;
        lines[n].line_no = n + 1;
        n++;
        if (!nl) break;
        p = nl + 1;
    }
    pp_lines(lines, n);
    free(lines);
    free(buf);
}

static void load_file(const char *path) {
    preprocess(path, 0);
    g_cur_global = "";
    for (int i = 0; i < g_nplines; i++) {

        load_line(g_plines[i].text, g_plines[i].line_no);
    }
}

/* ------------------------------------------------------------------ */
/* encoding                                                            */
/* ------------------------------------------------------------------ */

static int emit_data(const char *mnem, AsmLine *L, int emit) {
    int width = mnem[1] == 'b' ? 1 : mnem[1] == 'w' ? 2 : mnem[1] == 'd' ? 4 : 8;
    int total = 0;
    for (int i = 0; i < L->nops; i++) {
        const char *s = L->ops[i];
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '"' || *s == '\'') {
            char q = *s++;
            while (*s && *s != q) { if (emit) out_byte((uint8_t)*s); total++; s++; }
            if (*s != q) die("unterminated string");
            continue;
        }
        /* SH4d/SH4e: `dq sym` in ELF64 / `dd sym` in ELF32.  Same-section
         * targets resolve; defined cross-section targets relocate against
         * the SECTION symbol with the value as addend; externs relocate
         * against the symbol.  R_X86_64_64 == R_386_32 == 1, so the type
         * number is shared; ELF32 (REL) stores the addend in the field,
         * ELF64 (RELA) stores it in the entry with a zero field. */
        char symname[300];
        int symw = (g_fmt_elf == 2) ? 4 : 8;
        if (g_fmt_elf && width == symw &&
            parse_symref(s, symname, sizeof symname) == 0) {
            Sym *sy = sym_find(symname);
            if (sy && sy->is_equ) {
                if (emit) out_le((unsigned long long)sy->val, width);
            } else if (g_fmt_elf == 2) {
                /* ELF32 absolute pointers always relocate (R_386_32), even
                 * to the same section: the section base is a link-time
                 * constant, so the field carries the value as addend. */
                if (emit) {
                    if (sy && sy->defined) {
                        out_le((unsigned long long)sy->val, width);
                        reloc_add(g_cursec, (uint32_t)g_pc, 1, sy->sec, NULL, sy->val);
                    } else {
                        out_le(0, width);
                        reloc_add(g_cursec, (uint32_t)g_pc, 1,
                                  -1, sy ? sy : sym_intern(symname), 0);
                    }
                }
            } else if (sy && sy->defined && sy->sec == g_cursec) {
                if (emit) out_le((unsigned long long)sy->val, width);
            } else if (sy && sy->defined) {
                if (emit) {
                    out_le(0, width);
                    reloc_add(g_cursec, (uint32_t)g_pc, 1,
                              sy->sec, NULL, sy->val);
                }
            } else {
                if (emit) {
                    out_le(0, width);
                    reloc_add(g_cursec, (uint32_t)g_pc, 1,
                              -1, sy ? sy : sym_intern(symname), 0);
                }
            }
            total += width;
            continue;
        }
        long long v = eval_str(s);
        if (emit) out_le((unsigned long long)v, width);
        total += width;
    }
    return total;
}

/* Emit the ModRM (+SIB +disp) bytes for a memory operand `m` with register
 * field `rf`.  Returns the byte count (ModRM + SIB + disp).  The segment
 * override prefix is emitted by the caller.  Handles 16-bit (special rm)
 * and 32-bit (SIB) effective addresses and nasm's shortest-disp choice. */
/* If `expr` is exactly "sym" (possibly a local label), fill symname with the
 * fully-resolved name and return 0.  Any expression that is not a bare
 * symbol (numeric, arithmetic, register) returns 1.  Undefined names still
 * return 0: the caller decides between a relocation and an error. */
static int parse_symref(const char *expr, char *symname, size_t symsz) {
    const char *p = expr;
    while (*p == ' ' || *p == '\t') p++;
    if (!is_ident_start((uint8_t)*p)) return 1;
    char buf[256]; int n = 0;
    while (is_ident_char((uint8_t)*p) && n < 255) buf[n++] = *p++;
    buf[n] = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p) return 1;   /* symbol with a +const tail: unsupported (unused) */
    char full[300];
    if (buf[0] == '.') snprintf(full, sizeof full, "%s%s", g_cur_global, buf);
    else snprintf(full, sizeof full, "%s", buf);
    if (strlen(full) + 1 > symsz) die("symbol name too long");
    snprintf(symname, symsz, "%s", full);
    return 0;
}

/* REX.B for base regs >= 8, REX.X for index regs >= 8 (SH4d) */
static int mem_rex(Operand *m) {
    int r = 0;
    if (m->base >= 8) r |= 0x01;
    if (m->index >= 8) r |= 0x02;
    return r;
}

static int emit_mem(int rf, Operand *m, int emit) {
    char symname[300];
    int issym = (m->has_disp && parse_symref(m->disp, symname, sizeof symname) == 0);
    Sym *dsym = issym ? sym_find(symname) : NULL;
    int base = m->base, index = m->index;
    /* `disp` is used by every addressing path except the ELF64 RIP-relative
     * branch below, which handles the symbol itself (and must not evaluate
     * an extern at final emit).  For a bare symbol the value comes from the
     * symbol table; for arithmetic it goes through the expression evaluator. */
    /* RIP-relative only for a plain [sym]/[rel sym] with no segment prefix:
     * [gs:8] and friends are absolute SIB-no-base even under `default rel`
     * (measured against nasm). */
    int rip_rel = g_fmt_elf && g_bits == 64 && base < 0 && index < 0 && m->seg == 0 &&
                  (m->rel == 1 || (m->rel == -1 && g_default_rel));

    long long disp = 0;
    if (m->has_disp && !(rip_rel && issym && (!dsym || !dsym->defined))) {
        if (issym) {
            if (dsym && dsym->defined) disp = dsym->val;
            else if (g_allow_undef) disp = 0;
            else die("undefined symbol '%s'", symname);
        } else {
            disp = eval_str(m->disp);
        }
    }

    if (m->aregs == 16) {
        /* 16-bit: rm encodes the base/index combo; no SIB. */
        int rm = -1;
        int B = base, I = index;
        /* normalise: bx/bp are "base", si/di are "index" in 16-bit */
        int bx=3, sp=4, bp=5, si=6, di=7; (void)sp;
        if (B < 0 && I < 0) rm = 6;                       /* [disp16] */
        else if (B == bx && I == si) rm = 0;
        else if (B == bx && I == di) rm = 1;
        else if (B == bp && I == si) rm = 2;
        else if (B == bp && I == di) rm = 3;
        else if (B == si && I < 0) rm = 4;
        else if (B == di && I < 0) rm = 5;
        else if (B == bp && I < 0) rm = 6;
        else if (B == bx && I < 0) rm = 7;
        else if (B < 0 && I == si) rm = 4;
        else if (B < 0 && I == di) rm = 5;
        else die("unsupported 16-bit addressing [%s]", m->disp);
        int nodisp_absent = (rm == 6 && B != bp);   /* rm=6 with no base = [disp16] */
        int mod;
        int dsz;
        if (!m->has_disp && !nodisp_absent) {
            if (rm == 6) { mod = 1; dsz = 1; disp = 0; }  /* [bp] needs disp8=0 */
            else { mod = 0; dsz = 0; }
        } else if (!nodisp_absent && disp >= -128 && disp <= 127) {
            mod = 1; dsz = 1;
        } else {
            mod = 2; dsz = 2;
        }
        if (nodisp_absent) { mod = 0; dsz = 2; }   /* [disp16]: mod00 rm6 */
        if (emit) {
            out_byte((uint8_t)modrm(mod, rf, rm));
            if (dsz == 1) out_byte((uint8_t)disp);
            else if (dsz == 2) out_le((unsigned long long)disp, 2);
        }
        return 1 + dsz;
    }

    /* 32-bit (and 64-bit, though stage2's flat code is 16/32) */
    int need_sib = (index >= 0) || (base == 4);   /* esp/r12 as base needs SIB */
    int mod;
    int dsz;
    if (base < 0 && index < 0) {
        /* SH4d: [rel sym] / default-rel [sym] in 64-bit ELF -> RIP-relative
         * mod00/rm101 disp32.  Same-section targets resolve at assembly
         * (nasm emits no relocation); cross-section/extern targets get a
         * R_X86_64_PC32 with addend -4 (nasm style). */
        int rip_rel = g_fmt_elf && g_bits == 64 && m->seg == 0 &&
                      (m->rel == 1 || (m->rel == -1 && g_default_rel));
        if (rip_rel) {
            int n = 1 + 4;
            /* The displacement field sits right after the modrm byte; the
             * prefixes/opcode were already written by the caller, so the
             * section data length minus g_pc is the prefix count and the
             * disp field offset is (current length + 1). */
            size_t pre = (size_t)(g_secs[g_cursec].len - (size_t)g_pc);
            uint32_t disp_off = (uint32_t)(g_secs[g_cursec].len + 1);
            long long full = (long long)pre + 1 + 4;   /* full instruction size */
            if (issym) {
                if (dsym && dsym->defined && !dsym->is_equ && dsym->sec == g_cursec) {
                    long long v = dsym->val - (g_pc + full);
                    if (emit) { out_byte((uint8_t)modrm(0, rf, 5)); out_le((unsigned long long)v, 4); }
                } else if (dsym && dsym->defined && !dsym->is_equ) {
                    if (emit) {
                        out_byte((uint8_t)modrm(0, rf, 5));
                        out_le(0, 4);
                        reloc_add(g_cursec, disp_off, R_X86_64_PC32,
                                  dsym->sec, NULL, dsym->val - 4);
                    }
                } else {
                    if (emit) {
                        out_byte((uint8_t)modrm(0, rf, 5));
                        out_le(0, 4);
                        reloc_add(g_cursec, disp_off, R_X86_64_PC32,
                                  -1, dsym ? dsym : sym_intern(symname), -4);
                    }
                }
            } else {
                die("RIP-relative [disp] needs a symbol (SH4d subset)");
            }
            return n;
        }
        /* absolute [disp].  In 64-bit mode mod00/rm101 is RIP-relative, so
         * nasm uses SIB-with-no-base for an absolute address; in 32-bit it
         * is the plain mod00/rm101 disp32 form.  (ELF64 refuses a bare
         * [symbol] because the tree's dialect is `default rel`; bin keeps
         * the absolute form, which stage2's bits-64 check code uses.) */
        if (g_fmt_elf && issym && g_bits == 64)
            die("absolute [symbol] in 64-bit mode not supported (SH4d uses rel)");
        if (g_bits == 64) {
            if (emit) {
                out_byte((uint8_t)modrm(0, rf, 4));
                out_byte(0x25);
                out_le((unsigned long long)disp, 4);
            }
            return 1 + 1 + 4;
        }
        if (emit) {
            out_byte((uint8_t)modrm(0, rf, 5));
            /* SH4e: ELF32 absolute [sym] -> R_386_32 ALWAYS (even same
             * section: the base is a link-time constant). */
            if (g_fmt_elf == 2 && issym) {
                if (dsym && dsym->defined && !dsym->is_equ) {
                    out_le((unsigned long long)dsym->val, 4);
                    reloc_add(g_cursec, (uint32_t)(g_pc + 1), 1, dsym->sec, NULL, dsym->val);
                } else if (!dsym || !dsym->defined) {
                    out_le(0, 4);
                    reloc_add(g_cursec, (uint32_t)(g_pc + 1), 1,
                              -1, dsym ? dsym : sym_intern(symname), 0);
                } else {
                    out_le((unsigned long long)disp, 4);
                }
            } else {
                out_le((unsigned long long)disp, 4);
            }
        }
        return 1 + 4;
    }
    if (base < 0) {   /* SIB with no base: mod00 + SIB base=5 forces disp32 */
        mod = 0; dsz = 4;
    } else {
        long long eff = m->has_disp ? disp : 0;
        if (eff == 0 && base != 5) { mod = 0; dsz = 0; }              /* [base(+0)], not ebp */
        else if (eff == 0 && base == 5) { mod = 1; dsz = 1; disp = 0; }  /* [ebp] -> disp8=0 */
        else if (eff >= -128 && eff <= 127) { mod = 1; dsz = 1; }
        else { mod = 2; dsz = 4; }
    }

    if (!need_sib) {
        if (emit) {
            out_byte((uint8_t)modrm(mod, rf, base));
            if (dsz == 1) out_byte((uint8_t)disp);
            else if (dsz == 4) out_le((unsigned long long)disp, 4);
        }
        return 1 + dsz;
    }
    /* SIB */
    int ss = m->scale == 8 ? 3 : m->scale == 4 ? 2 : m->scale == 2 ? 1 : 0;
    int sib_base = (base < 0) ? 5 : (base & 7);    /* REX.B holds base bit 3 */
    int sib_index = (index < 0) ? 4 : (index & 7); /* REX.X holds index bit 3 */
    if (base < 0) { mod = 0; }   /* no base: disp32 via SIB base=5, mod00 */
    if (emit) {
        out_byte((uint8_t)modrm(mod, rf, 4));
        out_byte((uint8_t)((ss << 6) | (sib_index << 3) | sib_base));
        if (base < 0) out_le((unsigned long long)disp, 4);
        else if (dsz == 1) out_byte((uint8_t)disp);
        else if (dsz == 4) out_le((unsigned long long)disp, 4);
    }
    return 2 + (base < 0 ? 4 : dsz);
}

static int encode_instr(AsmLine *L, int emit, int *changed) {
    const char *m = L->mnem;
    Operand a, b;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.kind = b.kind = OP_NONE; a.base = a.index = b.base = b.index = -1;
    a.scale = b.scale = 1; a.rel = b.rel = -1;
    if (L->nops >= 1) parse_operand(L->ops[0], &a);
    if (L->nops >= 2) parse_operand(L->ops[1], &b);

    /* ---- o64 prefix (e.g. `o64 sysret` = 48 0F 07) ---- */
    if (!strcmp(m, "o64")) {
        if (L->nops == 1 && !strcmp(L->ops[0], "sysret")) {
            if (emit) { out_byte(0x48); out_byte(0x0F); out_byte(0x07); }
            return 3;
        }
        die("unsupported 'o64' prefix target (SH4d subset)");
    }

    /* ---- zero-operand ---- */
    if (L->nops == 0) {
        struct { const char *n; int op; } z1[] = {
            {"cli",0xFA},{"sti",0xFB},{"hlt",0xF4},{"ret",0xC3},{"lodsb",0xAC},
            {"cld",0xFC},{"std",0xFD},{"nop",0x90},{"clc",0xF8},{"stc",0xF9},
            {"cmc",0xF5},{"leave",0xC9},{"retf",0xCB},{NULL,0}
        };
        for (int i = 0; z1[i].n; i++) if (!strcmp(m, z1[i].n)) {
            if (emit) out_byte((uint8_t)z1[i].op);
            return 1;
        }
        struct { const char *n; int op; } z2[] = {   /* 0F xx */
            {"ud2",0x0B},{"rdmsr",0x32},{"wrmsr",0x30},{"cpuid",0xA2},
            {"syscall",0x05},{"sysret",0x07},{NULL,0}
        };
        for (int i = 0; z2[i].n; i++) if (!strcmp(m, z2[i].n)) {
            if (emit) { out_byte(0x0F); out_byte((uint8_t)z2[i].op); }
            return 2;
        }
        /* SH4d: 64-bit far/syscall-family single-byte forms.  Measured from
         * nasm: pushfq/popfq are plain 9C/9D (no REX.W), iretq/retfq carry
         * the REX.W prefix, fninit is DB E3, `o64 sysret` is 48 0F 07. */
        struct { const char *n; int op; } zr[] = {
            {"pushfq",0x9C},{"popfq",0x9D},{NULL,0}
        };
        for (int i = 0; zr[i].n; i++) if (!strcmp(m, zr[i].n)) {
            if (emit) out_byte((uint8_t)zr[i].op);
            return 1;
        }
        if (!strcmp(m, "iretq")) { if (emit) { out_byte(0x48); out_byte(0xCF); } return 2; }
        if (!strcmp(m, "retfq")) { if (emit) { out_byte(0x48); out_byte(0xCB); } return 2; }
        if (!strcmp(m, "fninit")) { if (emit) { out_byte(0xDB); out_byte(0xE3); } return 2; }

        if (!strcmp(m, "pusha") || !strcmp(m, "pushad") ||
            !strcmp(m, "popa")  || !strcmp(m, "popad")) {
            /* measured: 66 appears ONLY in bits 16 (pushad/popad there);
             * in bits 32 nasm emits 60/61 for both the a and ad forms. */
            int ispush = (m[1] == 'u');
            /* measured: 66 appears only in bits 16 AND only for the 'd'
             * forms (pushad); in bits 32 both pusha and pushad are 60. */
            int p66 = (g_bits == 16) && (m[strlen(m)-1] == 'd');
            if (emit) { if (p66) out_byte(0x66); out_byte((uint8_t)(ispush ? 0x60 : 0x61)); }
            return (p66?1:0) + 1;
        }
        if (!strcmp(m, "pushf") || !strcmp(m, "pushfd") ||
            !strcmp(m, "popf")  || !strcmp(m, "popfd")) {
            int ispush = (m[1] == 'u');
            int p66 = (g_bits == 16) && (m[strlen(m)-1] == 'd');
            if (emit) { if (p66) out_byte(0x66); out_byte((uint8_t)(ispush ? 0x9C : 0x9D)); }
            return (p66?1:0) + 1;
        }
        /* iret/iretd: CF; 66 prefix only in bits 16 (measured) */
        if (!strcmp(m, "iret") || !strcmp(m, "iretd")) {
            if (emit) { if (g_bits == 16) out_byte(0x66); out_byte(0xCF); }
            return (g_bits == 16) ? 2 : 1;
        }
        die("unsupported zero-operand instruction '%s'", m);
    }

    /* ---- int imm8 ---- */
    if (!strcmp(m, "int") && L->nops == 1 && a.kind == OP_IMM) {
        long long v = eval_str(a.text);
        if (emit) { out_byte(0xCD); out_byte((uint8_t)v); }
        return 2;
    }

    /* ---- jmp / call ---- */
    if (!strcmp(m, "jmp") || !strcmp(m, "call")) {
        int is_call = m[0] == 'c';
        if (a.kind == OP_REG) {                 /* indirect: FF /4 (jmp), FF /2 (call) */
            int rf = is_call ? 2 : 4;
            int rex = (g_bits == 64 && a.reg >= 8) ? 0x41 : 0;
            if (emit) {
                if (rex) out_byte((uint8_t)rex);
                out_byte(0xFF);
                out_byte((uint8_t)modrm(3, rf, a.reg));
            }
            return 2 + (rex ? 1 : 0);
        }
        if (a.kind == OP_MEM) {                 /* indirect through memory: FF /digit */
            int rf = is_call ? 2 : 4;
            int segpre = a.seg ? 1 : 0;
            int a67 = need_67(a.aregs);
            int memsz = emit_mem(rf, &a, 0);
            if (emit) {
                if (a.seg) out_byte((uint8_t)a.seg);
                if (a67) out_byte(0x67);
                out_byte(0xFF);
                emit_mem(rf, &a, 1);
            }
            return segpre + (a67?1:0) + 1 + memsz;
        }
        if (a.kind == OP_FAR) {                 /* far jmp seg:off -> EA */
            char buf[256]; snprintf(buf, sizeof buf, "%s", a.text);
            char *colon = strchr(buf, ':'); *colon = 0;
            long long seg = eval_str(buf);
            int offw = (a.memsize == 32) ? 4 : (a.memsize == 16) ? 2 : (g_bits == 16 ? 2 : 4);
            int defw = (g_bits == 16) ? 2 : 4;
            int p66 = (offw != defw);
            char offname[300];
            int offsym = parse_symref(colon + 1, offname, sizeof offname) == 0;
            Sym *osy = offsym ? sym_find(offname) : NULL;
            long long off = 0;
            if (offsym && osy && osy->defined && !osy->is_equ) {
                off = osy->val;
            } else if (offsym && osy && osy->is_equ) {
                off = osy->val;
            } else if (!offsym) {
                off = eval_str(colon + 1);
            }
            if (emit) {
                if (p66) out_byte(0x66);
                out_byte(0xEA);
                /* SH4e: ELF32 far-jump offset that names a label relocates
                 * (R_386_32 against the target section, field = value). */
                if (g_fmt_elf == 2 && offsym && osy && osy->defined && !osy->is_equ) {
                    out_le((unsigned long long)off, offw);
                    reloc_add(g_cursec, (uint32_t)(g_pc + 1 + (p66?1:0)), 1,
                              osy->sec, NULL, off);
                } else {
                    out_le((unsigned long long)off, offw);
                }
                out_le((unsigned long long)seg, 2);
            }
            return (p66?1:0) + 1 + offw + 2;
        }
        /* near displacement jump/call (jump_target resolved at load time) */
        Sym *t = sym_find(L->jump_target);
        long long target = (t && t->defined) ? t->val : 0;
        int cc = jcc_cc(m);
        /* SH4d: displacement width follows the mode (rel16 in bits 16,
         * rel32 in bits 32/64).  An ELF64 target that is extern or defined
         * in another section cannot be sized at assembly time: emit rel32
         * with a R_X86_64_PC32 (addend -4), nasm style. */
        int dispw = (g_bits == 16) ? 2 : 4;
        /* equ constants resolve like immediates; externs and cross-section
         * defined targets need a relocation and are always rel32.  A same-
         * section FORWARD reference is not a relocation: it must go through
         * the fixed-point sizing so nasm's rel8-vs-rel32 choice reproduces. */
        int need_reloc = g_fmt_elf && dispw == 4 && t && !t->is_equ &&
                         (t->is_extern ||
                          (t->defined && t->sec >= 0 && t->sec != g_cursec) ||
                          (emit && !t->defined));
        if (is_call) {
            if (need_reloc) {
                if (emit) {
                    out_byte(0xE8);
                    out_le((g_fmt_elf == 2) ? 0xFFFFFFFCULL : 0, 4);
                    reloc_add(g_cursec, (uint32_t)(g_pc + 1), R_X86_64_PC32,
                              (t && t->defined && !t->is_equ) ? t->sec : -1,
                              (t && t->defined && !t->is_equ) ? NULL : t,
                              -4);
                }
                return 1 + 4;
            }
            long long rel = target - (g_pc + 1 + dispw);
            if (emit) { out_byte(0xE8); out_le((unsigned long long)rel, dispw); }
            return 1 + dispw;
        }
        if (!emit) {
            if (need_reloc) {
                if (!L->jump_long) { L->jump_long = 1; if (changed) *changed = 1; }
                return 1 + dispw;
            }
            if (!L->jump_long) {
                long long rel = target - (g_pc + 2);
                if (rel < -128 || rel > 127) { L->jump_long = 1; if (changed) *changed = 1; }
            } else {
                long long rel = target - (g_pc + 1 + dispw);
                if (rel >= -128 && rel <= 127) { L->jump_long = 0; if (changed) *changed = 1; }
            }
            return L->jump_long ? (1 + dispw) : 2;
        }
        if (!L->jump_long) {
            long long rel = target - (g_pc + 2);
            if (cc >= 0) out_byte((uint8_t)(0x70 + cc)); else out_byte(0xEB);
            out_byte((uint8_t)rel);
            return 2;
        } else {
            if (need_reloc) {
                out_byte(0xE9);
                out_le((g_fmt_elf == 2) ? 0xFFFFFFFCULL : 0, 4);
                reloc_add(g_cursec, (uint32_t)(g_pc + 1), R_X86_64_PC32,
                          (t && t->defined && !t->is_equ) ? t->sec : -1,
                          (t && t->defined && !t->is_equ) ? NULL : t,
                          -4);
                return 1 + dispw;
            }
            long long rel = target - (g_pc + 1 + dispw);
            out_byte(0xE9); out_le((unsigned long long)rel, dispw);
            return 1 + dispw;
        }
    }

    /* ---- conditional jumps ---- */
    if (jcc_cc(m) >= 0) {
        int cc = jcc_cc(m);
        int dispw = (g_bits == 16) ? 2 : 4;
        Sym *t = sym_find(L->jump_target);
        long long target = (t && t->defined) ? t->val : 0;
        if (!emit) {
            if (!L->jump_long) {
                long long rel = target - (g_pc + 2);
                if (rel < -128 || rel > 127) { L->jump_long = 1; if (changed) *changed = 1; }
            } else {
                long long rel = target - (g_pc + 2 + dispw);
                if (rel >= -128 && rel <= 127) { L->jump_long = 0; if (changed) *changed = 1; }
            }
            return L->jump_long ? (2 + dispw) : 2;
        }
        if (!L->jump_long) {
            long long rel = target - (g_pc + 2);
            out_byte((uint8_t)(0x70 + cc)); out_byte((uint8_t)rel);
            return 2;
        } else {
            long long rel = target - (g_pc + 2 + dispw);
            out_byte(0x0F); out_byte((uint8_t)(0x80 + cc)); out_le((unsigned long long)rel, dispw);
            return 2 + dispw;
        }
    }

    /* ---- lgdt / lidt [mem] ---- */
    if ((!strcmp(m, "lgdt") || !strcmp(m, "lidt")) && a.kind == OP_MEM) {
        int rf = m[2] == 'd' ? 2 : 3;
        int segpre = a.seg ? 1 : 0;
        int a67 = need_67(a.aregs);
        int memsz = emit_mem(rf, &a, 0);
        if (emit) {
            if (a.seg) out_byte((uint8_t)a.seg);
            if (a67) out_byte(0x67);
            out_byte(0x0F); out_byte(0x01);
            emit_mem(rf, &a, 1);
        }
        return segpre + (a67?1:0) + 2 + memsz;
    }

    /* ---- ltr r/m16: 0F 00 /3 ---- */
    if (!strcmp(m, "ltr")) {
        int rf = 3;
        if (a.kind == OP_REG) {
            if (emit) { out_byte(0x0F); out_byte(0x00); out_byte((uint8_t)modrm(3, rf, a.reg)); }
            return 3;
        }
        if (a.kind == OP_MEM) {
            int segpre = a.seg ? 1 : 0;
            int a67 = need_67(a.aregs);
            int memsz = emit_mem(rf, &a, 0);
            if (emit) {
                if (a.seg) out_byte((uint8_t)a.seg);
                if (a67) out_byte(0x67);
                out_byte(0x0F); out_byte(0x00);
                emit_mem(rf, &a, 1);
            }
            return segpre + (a67?1:0) + 2 + memsz;
        }
        die("unsupported 'ltr' form");
    }

    /* ---- fxsave/fxrstor/ldmxcsr/stmxcsr [mem]: 0F AE /digit (SH4d) ---- */
    {
        struct { const char *n; int digit; } fx[] = {
            {"fxsave",0},{"fxrstor",1},{"ldmxcsr",2},{"stmxcsr",3},{NULL,0}
        };
        for (int i = 0; fx[i].n; i++) {
            if (strcmp(m, fx[i].n)) continue;
            if (a.kind != OP_MEM) die("'%s' needs a memory operand", m);
            int segpre = a.seg ? 1 : 0;
            int a67 = need_67(a.aregs);
            int memsz = emit_mem(fx[i].digit, &a, 0);
            if (emit) {
                if (a.seg) out_byte((uint8_t)a.seg);
                if (a67) out_byte(0x67);
                out_byte(0x0F); out_byte(0xAE);
                emit_mem(fx[i].digit, &a, 1);
            }
            return segpre + (a67?1:0) + 2 + memsz;
        }
    }

    /* ---- mov ---- */
    if (!strcmp(m, "mov")) {
        if (a.kind == OP_SREG && b.kind == OP_REG) {
            if (emit) { out_byte(0x8E); out_byte((uint8_t)modrm(3, a.reg, b.reg)); }
            return 2;
        }
        if (a.kind == OP_REG && b.kind == OP_SREG) {
            /* measured: the 66 prefix applies only when the GPR is 16-bit
             * (mov ax,ds = 66 8C D8); mov ds,ax = 8E D8 with no prefix. */
            int p66 = need_66(a.width);
            if (emit) { if (p66) out_byte(0x66); out_byte(0x8C); out_byte((uint8_t)modrm(3, b.reg, a.reg)); }
            return (p66?1:0) + 2;
        }
        if (a.kind == OP_CR && b.kind == OP_REG) {   /* mov crN, r32 */
            if (emit) { out_byte(0x0F); out_byte(0x22); out_byte((uint8_t)modrm(3, a.reg, b.reg)); }
            return 3;
        }
        if (a.kind == OP_REG && b.kind == OP_CR) {   /* mov r32, crN */
            if (emit) { out_byte(0x0F); out_byte(0x20); out_byte((uint8_t)modrm(3, b.reg, a.reg)); }
            return 3;
        }
        if (a.kind == OP_REG && b.kind == OP_IMM) {  /* mov reg, imm */
            /* SH4e: ELF32 `mov reg, sym` -> B8+r imm32 with R_386_32 when
             * the symbol is extern or in another section (boot32.asm's
             * `mov edi, __bss_start`).  Same-section targets resolve. */
            char immname[300];
            if (g_fmt_elf == 2 && parse_symref(b.text, immname, sizeof immname) == 0) {
                Sym *sy = sym_find(immname);
                int w = a.width;
                if (w != 32 && w != 16) die("ELF32 mov reg,sym only for 16/32-bit regs");
                int p66 = (w == 16);
                if (emit) {
                    if (p66) out_byte(0x66);
                    out_byte((uint8_t)((w == 8 ? 0xB0 : 0xB8) + (a.reg & 7)));
                    if (sy && sy->is_equ) {
                        out_le((unsigned long long)sy->val, w == 16 ? 2 : 4);
                    } else if (sy && sy->defined && !sy->is_equ) {
                        /* ELF32: always relocate; same-section field=value */
                        out_le((unsigned long long)sy->val, w == 16 ? 2 : 4);
                        reloc_add(g_cursec, (uint32_t)(g_pc + 1 + (p66?1:0)), 1, sy->sec, NULL, sy->val);
                    } else {
                        out_le(0, w == 16 ? 2 : 4);
                        reloc_add(g_cursec, (uint32_t)(g_pc + 1 + (p66?1:0)), 1,
                                  -1, sy ? sy : sym_intern(immname), 0);
                    }
                }
                return (p66?1:0) + 1 + (w == 16 ? 2 : 4);
            }
            long long v = eval_str(b.text);
            int w = a.width;
            /* nasm's 64-bit choices, shortest first:
             *   - imm fits unsigned 32  -> B8+r imm32, no REX.W (zero-extends)
             *   - imm fits signed 32    -> C7 /0 imm32 sign-extended (REX.W)
             *   - otherwise             -> B8+r imm64 (REX.W) */
            int c7 = 0;
            if (w == 64 && g_bits == 64) {
                if (v >= 0 && v <= 0xFFFFFFFFLL) w = 32;
                else if ((long long)v >= -0x80000000LL && (long long)v <= 0x7FFFFFFFLL) c7 = 1;
            }
            int p66 = need_66(w);
            int rex = (g_bits == 64 && (w == 64 || a.reg >= 8))
                      ? (0x40 | (w == 64 ? 8 : 0) | (a.reg >= 8 ? 1 : 0)) : 0;
            int immw = (w == 8) ? 1 : (w == 16) ? 2 : (w == 32) ? 4 : 8;
            if (c7) immw = 4;   /* C7 /0 imm32 */
            if (emit) {
                if (p66) out_byte(0x66);
                if (rex) out_byte((uint8_t)rex);
                if (c7) out_byte(0xC7);
                else out_byte((uint8_t)((w == 8 ? 0xB0 : 0xB8) + (a.reg & 7)));
                if (c7) out_byte((uint8_t)modrm(3, 0, a.reg));
                out_le((unsigned long long)v, immw);
            }
            return 1 + (p66?1:0) + (rex?1:0) + (c7?1:0) + immw;
        }
        if (a.kind == OP_REG && b.kind == OP_REG) {  /* mov reg, reg -> 88/89 /r */
            int w = a.width, p66 = need_66(w);
            int rex = (g_bits == 64 && (w == 64 || a.reg >= 8 || b.reg >= 8))
                      ? (0x40 | (w==64?8:0) | (b.reg>=8?4:0) | (a.reg>=8?1:0)) : 0;
            if (emit) {
                if (p66) out_byte(0x66);
                if (rex) out_byte((uint8_t)rex);
                out_byte((uint8_t)(w == 8 ? 0x88 : 0x89));
                out_byte((uint8_t)modrm(3, b.reg, a.reg));
            }
            return (p66?1:0) + (rex?1:0) + 2;
        }
        /* mov acc,[abs] / mov [abs],acc -> moffs (bits16/32; pure absolute only) */
        if (g_bits != 64 && a.kind == OP_REG && a.reg == 0 && b.kind == OP_MEM &&
            b.base < 0 && b.index < 0) {
            long long d = eval_str(b.disp);
            int w = a.width, p66 = need_66(w), addrw = (g_bits == 16) ? 2 : 4;
            int opc = (w == 8) ? 0xA0 : 0xA1;
            int segpre = b.seg ? 1 : 0;
            if (emit) { if (b.seg) out_byte((uint8_t)b.seg); if (p66) out_byte(0x66);
                        out_byte((uint8_t)opc); out_le((unsigned long long)d, addrw); }
            return segpre + 1 + (p66?1:0) + addrw;
        }
        if (g_bits != 64 && a.kind == OP_MEM && b.kind == OP_REG && b.reg == 0 &&
            a.base < 0 && a.index < 0) {
            long long d = eval_str(a.disp);
            int w = b.width, p66 = need_66(w), addrw = (g_bits == 16) ? 2 : 4;
            int opc = (w == 8) ? 0xA2 : 0xA3;
            int segpre = a.seg ? 1 : 0;
            if (emit) { if (a.seg) out_byte((uint8_t)a.seg); if (p66) out_byte(0x66);
                        out_byte((uint8_t)opc); out_le((unsigned long long)d, addrw); }
            return segpre + 1 + (p66?1:0) + addrw;
        }
        /* mov mem,reg (88/89) ; mov reg,mem (8A/8B) -- full addressing */
        if ((a.kind == OP_MEM && b.kind == OP_REG) || (a.kind == OP_REG && b.kind == OP_MEM)) {
            int memfirst = (a.kind == OP_MEM);
            Operand *rg = memfirst ? &b : &a;
            Operand *me = memfirst ? &a : &b;
            int w = rg->width, p66 = need_66(w);
            int rex = (g_bits == 64 && (w == 64 || rg->reg >= 8))
                      ? (0x40 | (w == 64 ? 8 : 0) | (rg->reg >= 8 ? 4 : 0)) : 0;
            rex |= mem_rex(me);
            int opc = memfirst ? (w == 8 ? 0x88 : 0x89) : (w == 8 ? 0x8A : 0x8B);
            int segpre = me->seg ? 1 : 0;
            int a67 = need_67(me->aregs);
            int memsz = emit_mem(rg->reg, me, 0);
            if (emit) {
                if (me->seg) out_byte((uint8_t)me->seg);
                if (p66) out_byte(0x66);
                if (a67) out_byte(0x67);
                if (rex) out_byte((uint8_t)rex);
                out_byte((uint8_t)opc);
                emit_mem(rg->reg, me, 1);
            }
            return segpre + (p66?1:0) + (a67?1:0) + (rex?1:0) + 1 + memsz;
        }
        /* mov mem, imm -> C6 /0 (8-bit) or C7 /0 (16/32-bit) */
        if (a.kind == OP_MEM && b.kind == OP_IMM) {
            /* SH4e: ELF32 `mov dword [mem], sym` -> C7 /0 imm32 with R_386_32
             * for extern/cross-section symbols. */
            char immname[300];
            if (g_fmt_elf == 2 && parse_symref(b.text, immname, sizeof immname) == 0) {
                Sym *sy = sym_find(immname);
                int w = a.memsize ? a.memsize : 32;
                if (w != 32 && w != 16) die("ELF32 mov mem,sym only for 16/32-bit");
                int p66 = (w == 16);
                int segpre = a.seg ? 1 : 0;
                int a67 = need_67(a.aregs);
                int memsz = emit_mem(0, &a, 0);
                int immw = (w == 16) ? 2 : 4;
                if (emit) {
                    if (a.seg) out_byte((uint8_t)a.seg);
                    if (p66) out_byte(0x66);
                    if (a67) out_byte(0x67);
                    out_byte(0xC7);
                    emit_mem(0, &a, 1);
                    uint32_t foff = (uint32_t)(g_pc + segpre + (p66?1:0) + (a67?1:0) + 1 + memsz);
                    if (sy && sy->is_equ) {
                        out_le((unsigned long long)sy->val, immw);
                    } else if (sy && sy->defined && !sy->is_equ) {
                        out_le((unsigned long long)sy->val, immw);
                        reloc_add(g_cursec, foff, 1, sy->sec, NULL, sy->val);
                    } else {
                        out_le(0, immw);
                        reloc_add(g_cursec, foff, 1, -1, sy ? sy : sym_intern(immname), 0);
                    }
                }
                return segpre + (p66?1:0) + (a67?1:0) + 1 + memsz + immw;
            }
            long long v = eval_str(b.text);
            int w = a.memsize ? a.memsize : (g_bits == 16 ? 16 : 32);
            int p66 = need_66(w);
            int rex = (g_bits == 64 && w == 64) ? 0x48 : 0;
            rex |= mem_rex(&a);
            int segpre = a.seg ? 1 : 0;
            int a67 = need_67(a.aregs);
            int memsz = emit_mem(0, &a, 0);
            int immw = (w == 8) ? 1 : (w == 16 ? 2 : 4);
            if (emit) {
                if (a.seg) out_byte((uint8_t)a.seg);
                if (p66) out_byte(0x66);
                if (a67) out_byte(0x67);
                if (rex) out_byte((uint8_t)rex);
                out_byte((uint8_t)(w == 8 ? 0xC6 : 0xC7));
                emit_mem(0, &a, 1);
                out_le((unsigned long long)v, immw);
            }
            return segpre + (p66?1:0) + (a67?1:0) + (rex?1:0) + 1 + memsz + immw;
        }
        die("unsupported mov form: [%s] , [%s]", L->nops>0?L->ops[0]:"", L->nops>1?L->ops[1]:"");
    }

    /* ---- ALU (add/or/adc/sbb/and/sub/xor/cmp) and test: all reg/mem/imm forms ---- */
    {
        int digit = -1, accimm = 0, istest = 0;
        struct { const char *n; int d; int acc; } ar[] = {
            {"add",0,0x05},{"or",1,0x0D},{"adc",2,0x15},{"sbb",3,0x1D},
            {"and",4,0x25},{"sub",5,0x2D},{"xor",6,0x35},{"cmp",7,0x3D},{NULL,0,0}
        };
        for (int i = 0; ar[i].n; i++) if (!strcmp(m, ar[i].n)) { digit = ar[i].d; accimm = ar[i].acc; }
        if (!strcmp(m, "test")) { istest = 1; digit = 0; }
        if (digit >= 0) {
            /* operand width: prefer the explicit reg/mem operand */
            Operand *dst = &a, *src = &b;
            int w = 0;
            if (dst->kind == OP_REG) w = dst->width;
            else if (dst->kind == OP_MEM) w = dst->memsize ? dst->memsize : (src->kind==OP_REG?src->width:(g_bits==16?16:32));
            else if (istest && src->kind == OP_REG) w = src->width;
            if (w == 0) die("cannot determine operand size for '%s'", m);

            /* reg/mem , reg */
            if (src->kind == OP_REG && (dst->kind == OP_REG || dst->kind == OP_MEM)) {
                int p66 = need_66(w);
                int segpre = (dst->kind == OP_MEM && dst->seg) ? 1 : 0;
                int a67 = (dst->kind == OP_MEM) ? need_67(dst->aregs) : 0;
                int rex = (g_bits==64 && (w==64 || src->reg>=8 || (dst->kind==OP_REG&&dst->reg>=8)))
                          ? (0x40|(w==64?8:0)|(src->reg>=8?4:0)|((dst->kind==OP_REG&&dst->reg>=8)?1:0)) : 0;
                int opc = (istest ? 0x84 : digit*8) + (w==8?0:1);
                if (emit) {
                    if (segpre) out_byte((uint8_t)dst->seg);
                    if (p66) out_byte(0x66);
                    if (a67) out_byte(0x67);
                    if (rex) out_byte((uint8_t)rex);
                    out_byte((uint8_t)opc);
                    if (dst->kind == OP_MEM) emit_mem(src->reg, dst, 1);
                    else out_byte((uint8_t)modrm(3, src->reg, dst->reg));
                }
                int memsz = (dst->kind==OP_MEM) ? emit_mem(src->reg, dst, 0) : 1;
                return segpre + (p66?1:0) + (a67?1:0) + (rex?1:0) + 1 + memsz;
            }
            /* reg , mem  (r, r/m form: opcode base+3) -- not for test (test r,m uses 84/85 too) */
            if (dst->kind == OP_REG && src->kind == OP_MEM) {
                int p66 = need_66(w);
                int segpre = src->seg ? 1 : 0;
                int a67 = need_67(src->aregs);
                int rex = (g_bits==64 && (w==64 || dst->reg>=8)) ? (0x40|(w==64?8:0)|(dst->reg>=8?4:0)) : 0;
                int opc = (istest ? 0x84 : digit*8 + 2) + (w==8?0:1);
                int memsz = emit_mem(dst->reg, src, 0);
                if (emit) {
                    if (segpre) out_byte((uint8_t)src->seg);
                    if (p66) out_byte(0x66);
                    if (a67) out_byte(0x67);
                    if (rex) out_byte((uint8_t)rex);
                    out_byte((uint8_t)opc);
                    emit_mem(dst->reg, src, 1);
                }
                return segpre + (p66?1:0) + (a67?1:0) + (rex?1:0) + 1 + memsz;
            }
            /* reg/mem , imm */
            if (src->kind == OP_IMM && (dst->kind == OP_REG || dst->kind == OP_MEM)) {
                long long v = eval_str(src->text);
                int p66 = need_66(w);
                int segpre = (dst->kind == OP_MEM && dst->seg) ? 1 : 0;
                int rex = (g_bits==64 && (w==64 || (dst->kind==OP_REG&&dst->reg>=8)))
                          ? (0x40|(w==64?8:0)|((dst->kind==OP_REG&&dst->reg>=8)?1:0)) : 0;
                int ismem = (dst->kind == OP_MEM);
                int a67 = ismem ? need_67(dst->aregs) : 0;
                int memsz = ismem ? emit_mem(digit, dst, 0) : 0;
                /* test acc, imm -> A8 (8-bit) / A9 (16/32-bit) accumulator form */
                if (istest && !ismem && dst->reg == 0) {
                    int opc = (w == 8) ? 0xA8 : 0xA9;
                    int immw = (w == 8) ? 1 : (w == 16 ? 2 : 4);
                    if (emit) {
                        if (p66) out_byte(0x66);
                        if (rex) out_byte((uint8_t)rex);
                        out_byte((uint8_t)opc);
                        out_le((unsigned long long)v, immw);
                    }
                    return (p66?1:0) + (rex?1:0) + 1 + immw;
                }
                if (w == 8) {   /* 0xF6-style: 80 /digit imm8 */
                    /* ALU al, imm8 -> accumulator short form 04/0C/14/1C/24/2C/34/3C */
                    if (!istest && !ismem && dst->reg == 0) {
                        if (emit) {
                            if (rex) out_byte((uint8_t)rex);
                            out_byte((uint8_t)(digit*8 + 4));
                            out_byte((uint8_t)v);
                        }
                        return (rex?1:0) + 1 + 1;
                    }
                    int opct = istest ? 0xF6 : 0x80;
                    if (emit) {
                        if (segpre) out_byte((uint8_t)dst->seg);
                        if (a67) out_byte(0x67);
                        if (rex) out_byte((uint8_t)rex);
                        out_byte((uint8_t)opct);
                        if (ismem) emit_mem(digit, dst, 1); else out_byte((uint8_t)modrm(3, digit, dst->reg));
                        out_byte((uint8_t)v);
                    }
                    return segpre + (a67?1:0) + (rex?1:0) + 1 + memsz + (ismem?0:1) + 1;
                }
                if (istest) {   /* test r/m16/32, imm -> F7 /0, imm at full width (no 0x83 form) */
                    int immw = (w == 16) ? 2 : 4;
                    if (emit) {
                        if (segpre) out_byte((uint8_t)dst->seg);
                        if (p66) out_byte(0x66);
                        if (a67) out_byte(0x67);
                        if (rex) out_byte((uint8_t)rex);
                        out_byte(0xF7);
                        if (ismem) emit_mem(0, dst, 1); else out_byte((uint8_t)modrm(3, 0, dst->reg));
                        out_le((unsigned long long)v, immw);
                    }
                    return segpre + (p66?1:0) + (a67?1:0) + (rex?1:0) + 1 + memsz + (ismem?0:1) + immw;
                }
                {
                    /* nasm picks 83 /digit imm8 whenever the value fits a
                     * sign-extended byte at the operand width (e.g. `and ax,
                     * 0xFFFB` -> 66 83 E0 FB), so interpret v as signed at
                     * that width before the imm8 test. */
                    long long sv = v;
                    if (w == 16 && (v & 0x8000)) sv = v - 0x10000;
                    else if (w == 8 && (v & 0x80)) sv = v - 0x100;
                    if (sv >= -128 && sv <= 127) {   /* 83 /digit imm8 sign-extended */
                        if (emit) {
                            if (segpre) out_byte((uint8_t)dst->seg);
                            if (p66) out_byte(0x66);
                            if (a67) out_byte(0x67);
                            if (rex) out_byte((uint8_t)rex);
                            out_byte(0x83);
                            if (ismem) emit_mem(digit, dst, 1); else out_byte((uint8_t)modrm(3, digit, dst->reg));
                            out_byte((uint8_t)sv);
                        }
                        return segpre + (p66?1:0) + (a67?1:0) + (rex?1:0) + 1 + memsz + (ismem?0:1) + 1;
                    }
                }
                int immw = (w == 16) ? 2 : 4;
                int is_acc = (!ismem && dst->reg == 0 && !istest);
                if (is_acc) {   /* 05/0D/... imm  (no ModRM) */
                    if (emit) {
                        if (p66) out_byte(0x66);
                        if (rex) out_byte((uint8_t)rex);
                        out_byte((uint8_t)accimm);
                        out_le((unsigned long long)v, immw);
                    }
                    return (p66?1:0) + (rex?1:0) + 1 + immw;
                }
                if (emit) {   /* 81 /digit imm */
                    if (segpre) out_byte((uint8_t)dst->seg);
                    if (p66) out_byte(0x66);
                    if (a67) out_byte(0x67);
                    if (rex) out_byte((uint8_t)rex);
                    out_byte(0x81);
                    if (ismem) emit_mem(digit, dst, 1); else out_byte((uint8_t)modrm(3, digit, dst->reg));
                    out_le((unsigned long long)v, immw);
                }
                return segpre + (p66?1:0) + (a67?1:0) + (rex?1:0) + 1 + memsz + (ismem?0:1) + immw;
            }
            die("unsupported '%s' form", m);
        }
    }

    /* ---- push / pop (reg, sreg) ---- */
    if (!strcmp(m, "push") || !strcmp(m, "pop")) {
        int ispush = m[1] == 'u';
        int defw = (g_bits == 16) ? 16 : (g_bits == 32) ? 32 : 64;
        if (a.kind == OP_REG) {
            int p66 = (a.width != defw) && (a.width == 16 || a.width == 32);
            int rex = (g_bits == 64 && a.reg >= 8) ? 0x41 : 0;
            int base = ispush ? 0x50 : 0x58;
            if (emit) { if (p66) out_byte(0x66); if (rex) out_byte((uint8_t)rex);
                        out_byte((uint8_t)(base + (a.reg & 7))); }
            return (p66?1:0) + (rex?1:0) + 1;
        }
        if (a.kind == OP_SREG) {
            int s = a.reg;   /* es0 cs1 ss2 ds3 fs4 gs5 */
            if (ispush) {
                if (s <= 3) {
                    static const int op[4] = {0x06,0x0E,0x16,0x1E};
                    if (emit) out_byte((uint8_t)op[s]);
                    return 1;
                }
                if (emit) { out_byte(0x0F); out_byte((uint8_t)(s==4?0xA0:0xA8)); }
                return 2;
            } else {
                if (s == 0) { if (emit) out_byte(0x07); return 1; }   /* pop es */
                if (s == 2) { if (emit) out_byte(0x17); return 1; }   /* pop ss */
                if (s == 3) { if (emit) out_byte(0x1F); return 1; }   /* pop ds */
                if (s >= 4) {
                    if (emit) { out_byte(0x0F); out_byte((uint8_t)(s==4?0xA1:0xA9)); }
                    return 2;
                }
                die("pop cs is not encodable");
            }
        }
        if (ispush && a.kind == OP_IMM) {
            /* push imm: 6A imm8 (sign-extended) when it fits, else 68 imm32
             * in 64-bit mode.  `push qword N` / `push dword N` / `push word N`
             * size words are accepted and select the operand width. */
            const char *t = a.text;
            int w = defw;
            while (*t == ' ' || *t == '\t') t++;
            if (!strncmp(t, "qword", 5) && (t[5]==' '||t[5]=='\t')) { w = 64; t += 5; }
            else if (!strncmp(t, "dword", 5) && (t[5]==' '||t[5]=='\t')) { w = 32; t += 5; }
            else if (!strncmp(t, "word", 4) && (t[4]==' '||t[4]=='\t')) { w = 16; t += 4; }
            while (*t == ' ' || *t == '\t') t++;
            long long v = eval_str(t);
            int p66 = (w != defw) && (w == 16 || w == 32);
            if (v >= -128 && v <= 127) {
                if (emit) { if (p66) out_byte(0x66); out_byte(0x6A); out_byte((uint8_t)v); }
                return (p66?1:0) + 2;
            }
            if (w == 64) {
                if (emit) { if (p66) out_byte(0x66); out_byte(0x68); out_le((unsigned long long)v, 4); }
                return (p66?1:0) + 5;
            }
            int immw = (w == 16) ? 2 : 4;
            if (emit) { if (p66) out_byte(0x66); out_byte(0x68); out_le((unsigned long long)v, immw); }
            return (p66?1:0) + 1 + immw;
        }
        die("unsupported '%s' form (SH4c: reg/sreg; SH4d adds imm)", m);
    }

    /* ---- string ops (with optional rep/repe/repne prefix) ---- */
    {
        int pre = 0;
        const char *so = m;
        if (!strcmp(m,"rep")||!strcmp(m,"repe")||!strcmp(m,"repz")) pre = 0xF3;
        else if (!strcmp(m,"repne")||!strcmp(m,"repnz")) pre = 0xF2;
        if (pre && L->nops == 1) so = L->ops[0];
        struct { const char *n; int op; int sz; } st[] = {
            {"movsb",0xA4,8},{"movsw",0xA5,16},{"movsd",0xA5,32},
            {"stosb",0xAA,8},{"stosw",0xAB,16},{"stosd",0xAB,32},
            {"lodsb",0xAC,8},{"lodsw",0xAD,16},{"lodsd",0xAD,32},
            {"scasb",0xAE,8},{"scasw",0xAF,16},{"scasd",0xAF,32},
            {"cmpsb",0xA6,8},{"cmpsw",0xA7,16},{"cmpsd",0xA7,32},{NULL,0,0}
        };
        for (int i = 0; st[i].n; i++) {
            if (strcmp(so, st[i].n)) continue;
            int defw = (g_bits == 16) ? 16 : 32;
            int p66 = ((st[i].sz == 16) != (defw == 16));   /* 66 flips 16<->32 only */
            if (emit) {
                if (pre) out_byte((uint8_t)pre);
                if (p66) out_byte(0x66);
                out_byte((uint8_t)st[i].op);
            }
            return (pre?1:0) + (p66?1:0) + 1;
        }
        if (pre) die("rep with unsupported string op '%s'", so);
    }

    /* ---- movzx r32, r/m8 | r/m16 ---- */
    if (!strcmp(m, "movzx") && a.kind == OP_REG && b.kind == OP_MEM) {
        int srcw = b.memsize ? b.memsize : 8;
        int p66 = need_66(a.width);
        int segpre = b.seg ? 1 : 0;
        int a67 = need_67(b.aregs);
        int memsz = emit_mem(a.reg, &b, 0);
        if (emit) {
            if (b.seg) out_byte((uint8_t)b.seg);
            if (p66) out_byte(0x66);
            if (a67) out_byte(0x67);
            out_byte(0x0F); out_byte((uint8_t)(srcw == 8 ? 0xB6 : 0xB7));
            emit_mem(a.reg, &b, 1);
        }
        return segpre + (p66?1:0) + (a67?1:0) + 2 + memsz;
    }

    /* ---- in / out ---- */
    if (!strcmp(m, "in") && a.kind == OP_REG) {
        int p66 = need_66(a.width);
        if (b.kind == OP_IMM) {
            long long port = eval_str(b.text);
            if (emit) { if (p66) out_byte(0x66); out_byte((uint8_t)(a.width==8?0xE4:0xE5));
                        out_byte((uint8_t)port); }
            return (p66?1:0) + 2;
        }
        if (b.kind == OP_REG && b.width == 16 && b.reg == 2) {  /* dx */
            if (emit) { if (p66) out_byte(0x66); out_byte((uint8_t)(a.width==8?0xEC:0xED)); }
            return (p66?1:0) + 1;
        }
        die("unsupported 'in' form");
    }
    if (!strcmp(m, "out") && b.kind == OP_REG) {
        int p66 = need_66(b.width);
        if (a.kind == OP_IMM) {
            long long port = eval_str(a.text);
            if (emit) { if (p66) out_byte(0x66); out_byte((uint8_t)(b.width==8?0xE6:0xE7));
                        out_byte((uint8_t)port); }
            return (p66?1:0) + 2;
        }
        if (a.kind == OP_REG && a.width == 16 && a.reg == 2) {  /* dx */
            if (emit) { if (p66) out_byte(0x66); out_byte((uint8_t)(b.width==8?0xEE:0xEF)); }
            return (p66?1:0) + 1;
        }
        die("unsupported 'out' form");
    }

    /* ---- lea r, m ---- */
    if (!strcmp(m, "lea") && a.kind == OP_REG && b.kind == OP_MEM) {
        int p66 = need_66(a.width);
        int rex = (g_bits == 64 && (a.width == 64 || a.reg >= 8))
                  ? (0x40 | (a.width==64?8:0) | (a.reg>=8?4:0)) : 0;
        rex |= mem_rex(&b);
        int segpre = b.seg ? 1 : 0;
        int a67 = need_67(b.aregs);
        int memsz = emit_mem(a.reg, &b, 0);
        if (emit) {
            if (b.seg) out_byte((uint8_t)b.seg);
            if (p66) out_byte(0x66);
            if (a67) out_byte(0x67);
            if (rex) out_byte((uint8_t)rex);
            out_byte(0x8D);
            emit_mem(a.reg, &b, 1);
        }
        return segpre + (p66?1:0) + (a67?1:0) + (rex?1:0) + 1 + memsz;
    }

    /* ---- imul ---- */
    if (!strcmp(m, "imul")) {
        if (a.kind == OP_REG && b.kind == OP_REG && L->nops == 3) { /* imul r,r,imm */
            Operand c; memset(&c,0,sizeof c); parse_operand(L->ops[2], &c);
            long long v = eval_str(c.text);
            int p66 = need_66(a.width);
            int immw = (a.width==16)?2:4;
            if (v >= -128 && v <= 127) {
                if (emit) { if (p66) out_byte(0x66); out_byte(0x6B);
                            out_byte((uint8_t)modrm(3, a.reg, b.reg)); out_byte((uint8_t)v); }
                return (p66?1:0) + 1 + 1 + 1;
            }
            if (emit) { if (p66) out_byte(0x66); out_byte(0x69);
                        out_byte((uint8_t)modrm(3, a.reg, b.reg)); out_le((unsigned long long)v, immw); }
            return (p66?1:0) + 1 + 1 + immw;
        }
        if (a.kind == OP_REG && b.kind == OP_REG) {       /* imul r, r -> 0F AF /r */
            int p66 = need_66(a.width);
            if (emit) { if (p66) out_byte(0x66); out_byte(0x0F); out_byte(0xAF);
                        out_byte((uint8_t)modrm(3, a.reg, b.reg)); }
            return (p66?1:0) + 2 + 1;
        }
        die("unsupported 'imul' form");
    }

    /* ---- mul / div / idiv / not / neg / inc / dec  r/m  (unary /digit) ---- */
    {
        struct { const char *n; int digit; int op0f; } un[] = {
            {"mul",4,0},{"div",6,0},{"idiv",7,0},{"not",2,0},{"neg",3,0},
            {"inc",0,1},{"dec",1,1},{NULL,0,0}
        };
        for (int i = 0; un[i].n; i++) {
            if (strcmp(m, un[i].n)) continue;
            if (a.kind == OP_REG) {
                int w = a.width, p66 = need_66(w);
                if (un[i].op0f) {   /* inc/dec reg */
                    if (w == 8) {
                        if (emit) { out_byte((uint8_t)(un[i].digit?0xFE:0xFE));
                                    out_byte((uint8_t)modrm(3, un[i].digit, a.reg)); }
                        return 2;
                    }
                    if (g_bits == 64) {
                        /* 0x40+r / 0x48+r are REX prefixes in 64-bit mode:
                         * nasm emits FF /digit (+REX.W/REX.B) instead. */
                        int rex = (w == 64 || a.reg >= 8)
                                  ? (0x40 | (w==64?8:0) | (a.reg>=8?1:0)) : 0;
                        if (emit) {
                            if (rex) out_byte((uint8_t)rex);
                            out_byte(0xFF);
                            out_byte((uint8_t)modrm(3, un[i].digit, a.reg));
                        }
                        return (rex?1:0) + 2;
                    }
                    int rex = (g_bits==64 && (w==64||a.reg>=8)) ? (0x40|(w==64?8:0)|(a.reg>=8?1:0)) : 0;
                    if (emit) { if (p66) out_byte(0x66); if (rex) out_byte((uint8_t)rex);
                                out_byte((uint8_t)((un[i].digit?0x48:0x40) + (a.reg&7))); }
                    return (p66?1:0)+(rex?1:0)+1;
                }
                /* mul/div/not/neg reg -> F7 /digit (or F6 for 8-bit) */
                if (emit) { if (p66) out_byte(0x66); out_byte((uint8_t)(w==8?0xF6:0xF7));
                            out_byte((uint8_t)modrm(3, un[i].digit, a.reg)); }
                return (p66?1:0)+2;
            }
            if (a.kind == OP_MEM) {
                int w = a.memsize ? a.memsize : (g_bits==16?16:32);
                int p66 = need_66(w);
                int segpre = a.seg?1:0;
                int a67 = need_67(a.aregs);
                int memsz = emit_mem(un[i].digit, &a, 0);
                if (emit) {
                    if (a.seg) out_byte((uint8_t)a.seg);
                    if (p66) out_byte(0x66);
                    if (a67) out_byte(0x67);
                    /* inc/dec use FE/FF; test/not/neg/mul/div use F6/F7 */
                    out_byte((uint8_t)(un[i].op0f ? (w==8?0xFE:0xFF) : (w==8?0xF6:0xF7)));
                    emit_mem(un[i].digit, &a, 1);
                }
                return segpre+(p66?1:0)+(a67?1:0)+1+memsz;
            }
            die("unsupported '%s' form", m);
        }
    }

    /* ---- loop / loope / loopne (rel8) ---- */
    if (!strcmp(m,"loop")||!strcmp(m,"loope")||!strcmp(m,"loopne")||!strcmp(m,"jcxz")) {
        int op = !strcmp(m,"loop")?0xE2 : !strcmp(m,"loope")?0xE1 :
                 !strcmp(m,"loopne")?0xE0 : 0xE3;
        Sym *t = sym_find(L->jump_target);
        long long target = (t && t->defined) ? t->val : 0;
        long long rel = target - (g_pc + 2);
        if (emit) { out_byte((uint8_t)op); out_byte((uint8_t)rel); }
        return 2;
    }

    /* ---- shifts: shl/shr/sar/rol/ror/rcl/rcr r/m, imm8|cl ---- */
    {
        struct { const char *n; int digit; } sh[] = {
            {"rol",0},{"ror",1},{"rcl",2},{"rcr",3},{"shl",4},{"shr",5},{"sar",7},{NULL,0}
        };
        for (int i = 0; sh[i].n; i++) {
            if (strcmp(m, sh[i].n)) continue;
            if (a.kind == OP_REG) {
                int w = a.width, p66 = need_66(w);
                int rex = (g_bits == 64 && w == 64) ? 0x48 : 0;
                int bycl = (b.kind == OP_REG && b.width == 8 && b.reg == 1);  /* cl */
                if (emit) {
                    if (p66) out_byte(0x66);
                    if (rex) out_byte((uint8_t)rex);
                    out_byte((uint8_t)(w==8?0xC0:0xC1));
                    out_byte((uint8_t)modrm(3, sh[i].digit, a.reg));
                    if (!bycl) { long long v = eval_str(b.text); out_byte((uint8_t)v); }
                }
                return (p66?1:0) + (rex?1:0) + 2 + (bycl?0:1);
            }
            die("unsupported shift form for '%s'", m);
        }
    }

    die("unsupported instruction '%s' (SH4b subset)", m);
    return 0;
}

static int assemble_line(AsmLine *L, int emit, int *changed);

static int do_times(AsmLine *L, int emit, int *changed) {
    char tail[512];
    snprintf(tail, sizeof tail, "%s", L->ops[0]);
    char *dpos = NULL;
    static const char *ds[] = {" db ", " dw ", " dd ", " dq ", NULL};
    for (int i = 0; ds[i]; i++) {
        char *f = strstr(tail, ds[i]);
        if (f && (!dpos || f < dpos)) dpos = f;
    }
    if (!dpos) die("times: only 'times N d? ...' supported in SH4a");
    *dpos = 0;
    char *dstart = dpos + 1;
    long long count = eval_str(tail);
    if (count < 0) die("times: negative count");

    AsmLine tmp; memset(&tmp, 0, sizeof tmp);
    tmp.line_no = L->line_no;
    char *sp = strchr(dstart, ' ');
    if (!sp) die("times: malformed repeated directive");
    *sp = 0;
    tmp.mnem = lower(dstart);
    tmp.nops = split_operands(sp + 1, tmp.ops, 6);

    long long save_pc = g_pc;
    long long per = assemble_line(&tmp, 0, changed);
    g_pc = save_pc;
    long long total = per * count;
    if (emit) {
        for (long long i = 0; i < count; i++) assemble_line(&tmp, 1, changed);
    } else {
        g_pc += total;
    }
    return (int)total;
}

static int assemble_line(AsmLine *L, int emit, int *changed) {
    g_line_no = L->line_no;
    if (L->global_set) g_cur_global = L->global_set;   /* track scope for local-label refs in expressions */

    if (L->label) {
        Sym *s = sym_intern(L->label);
        s->val = g_pc;
        s->defined = 1;
        if (g_fmt_elf) {
            if (s->is_equ) die("label '%s' redefines an equ constant", L->label);
            s->sec = g_cursec;
            if (s->is_global) sym_global_add(s); else sym_local_add(s);
        }
    }
    if (!L->mnem) return 0;
    if (L->is_equ) {
        Sym *s = sym_intern(L->mnem);
        s->val = eval_str(L->equ_expr);
        s->defined = 1;
        s->is_equ = 1;
        s->sec = -2;   /* ABS */
        if (g_fmt_elf) sym_local_add(s);   /* nasm: LOCAL ABS, definition order */
        return 0;
    }

    const char *m = L->mnem;

    if (!strcmp(m, "bits")) {
        long long v = eval_str(L->ops[0]);
        if (v != 16 && v != 32 && v != 64) die("unsupported 'bits %lld' (16/32/64)", v);
        g_bits = (int)v;
        return 0;
    }
    if (!strcmp(m, "org")) { g_org = eval_str(L->ops[0]); g_pc = g_org; return 0; }
    if (g_fmt_elf && !strcmp(m, "section")) {
        if (L->nops < 1) die("section needs a name");
        g_cursec = sec_find_or_add(L->ops[0]);
        g_pc = g_secs[g_cursec].size;   /* sections are independent address spaces */
        return 0;
    }
    if (!strcmp(m, "default") && L->nops >= 1) {
        if (!strcmp(L->ops[0], "rel")) g_default_rel = 1;
        else if (!strcmp(L->ops[0], "abs")) g_default_rel = 0;
        return 0;
    }
    if (!strcmp(m, "global")) {
        if (g_fmt_elf)
            for (int i = 0; i < L->nops; i++) sym_intern(L->ops[i])->is_global = 1;
        return 0;
    }
    if (!strcmp(m, "extern")) {
        if (g_fmt_elf)
            for (int i = 0; i < L->nops; i++) {
                Sym *s = sym_intern(L->ops[i]);
                s->is_extern = 1;
                s->is_global = 1;
                s->sec = -1;   /* UND */
                sym_global_add(s);   /* entry at extern declaration, like nasm */
            }
        return 0;
    }
    /* remaining symbol/section directives are no-ops in -f bin (no symbol
     * table); the label still gets its address from its own definition. */
    if (!strcmp(m, "common") || !strcmp(m, "segment") || !strcmp(m, "cpu") ||
        !strcmp(m, "default")) {
        return 0;
    }
    if (!strcmp(m, "db") || !strcmp(m, "dw") || !strcmp(m, "dd") || !strcmp(m, "dq")) {
        int n = emit_data(m, L, emit); g_pc += n;
        if (g_fmt_elf && g_cursec >= 0 && g_pc > g_secs[g_cursec].size)
            g_secs[g_cursec].size = g_pc;
        return n;
    }
    if (!strcmp(m, "resb") || !strcmp(m, "resw") || !strcmp(m, "resd") || !strcmp(m, "resq")) {
        int width = m[3] == 'b' ? 1 : m[3] == 'w' ? 2 : m[3] == 'd' ? 4 : 8;
        long long n = eval_str(L->ops[0]) * width;
        if (emit) for (long long i = 0; i < n; i++) out_byte(0);   /* NOBITS swallows */
        g_pc += n;
        if (g_fmt_elf && g_cursec >= 0 && g_pc > g_secs[g_cursec].size)
            g_secs[g_cursec].size = g_pc;
        return (int)n;
    }
    if (!strcmp(m, "align") || !strcmp(m, "alignb")) {
        long long a = eval_str(L->ops[0]);
        long long off = g_pc - g_org;
        long long rem = off % a;
        long long pad = rem ? (a - rem) : 0;
        /* fill byte: alignb -> 0; align -> 0x90 (NOP) unless an explicit
         * "align N, db X" fill is given (nasm syntax, used by stage2's tail). */
        uint8_t fill = !strcmp(m, "alignb") ? 0x00 : 0x90;
        if (L->nops >= 2) {
            char fop[256]; snprintf(fop, sizeof fop, "%s", L->ops[1]);
            char *sp = strchr(fop, ' ');
            if (sp && !strncmp(fop, "db", 2)) {
                char *e = sp + 1; while (*e==' '||*e=='\t') e++;
                fill = (uint8_t)eval_str(e);
            }
        }
        if (g_fmt_elf && g_cursec >= 0 && a > g_secs[g_cursec].align)
            g_secs[g_cursec].align = (int)a;   /* section Al = max aligns */
        if (emit) for (long long i = 0; i < pad; i++) out_byte(fill);
        g_pc += pad; return (int)pad;
    }
    if (!strcmp(m, "times")) return do_times(L, emit, changed);

    int n = encode_instr(L, emit, changed);
    g_pc += n;
    if (g_fmt_elf && g_cursec >= 0 && g_pc > g_secs[g_cursec].size)
        g_secs[g_cursec].size = g_pc;
    return n;
}

/* ------------------------------------------------------------------ */
/* driver                                                              */
/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/* ELF64 writer (SH4d)                                                 */
/* ------------------------------------------------------------------ */

typedef struct { uint32_t st_name; uint8_t st_info; uint8_t st_other;
                 uint16_t st_shndx; uint64_t st_value; uint64_t st_size; } Elf64_Sym;
typedef struct { uint32_t sh_name; uint32_t sh_type; uint64_t sh_flags;
                 uint64_t sh_addr; uint64_t sh_offset; uint64_t sh_size;
                 uint32_t sh_link; uint32_t sh_info; uint64_t sh_addralign;
                 uint64_t sh_entsize; } Elf64_Shdr;
typedef struct { uint64_t r_offset; uint64_t r_info; int64_t r_addend; } Elf64_Rela;

static void le16(uint8_t *p, uint16_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
static void le32(uint8_t *p, uint32_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
static void le64(uint8_t *p, uint64_t v) { for (int i=0;i<8;i++) p[i]=(v>>(8*i))&0xFF; }

/* simple growable byte buffer for the object file */
static uint8_t *obj; static size_t obj_len, obj_cap;
static void obj_byte(uint8_t b) {
    if (obj_len >= obj_cap) { obj_cap = obj_cap ? obj_cap*2 : 4096; obj = realloc(obj, obj_cap);
        if (!obj) { fprintf(stderr, "mini-asm: oom\n"); exit(2); } }
    obj[obj_len++] = b;
}
static void obj_bytes(const uint8_t *b, size_t n) { for (size_t i=0;i<n;i++) obj_byte(b[i]); }
static void obj_zero(size_t n) { for (size_t i=0;i<n;i++) obj_byte(0); }
static void obj_align(size_t a) { while (obj_len % a) obj_byte(0); }

static void write_elf(const char *inpath, const char *outpath) {
    /* section header indices */
    int shstr_idx = 1 + g_nsecs;
    int symtab_idx = shstr_idx + 1;
    int strtab_idx = symtab_idx + 1;
    /* count rela sections (sections with relocs, in section order) */
    int nrela_secs = 0;
    for (int i = 0; i < g_nsecs; i++)
        for (int j = 0; j < g_nrelocs; j++)
            if (g_relocs[j].sec == i) { nrela_secs++; break; }

    /* --- build .shstrtab / .strtab --- */
    uint8_t shstr[4096]; size_t shstr_len = 1; shstr[0] = 0;
    int sh_name[16];
    for (int i = 0; i < g_nsecs; i++) {
        sh_name[i] = (int)shstr_len;
        size_t L = strlen(g_secs[i].name);
        memcpy(shstr + shstr_len, g_secs[i].name, L);
        shstr_len += L; shstr[shstr_len++] = 0;
    }
    { const char *n = ".shstrtab"; memcpy(shstr+shstr_len, n, strlen(n)); shstr_len += strlen(n); shstr[shstr_len++]=0; }
    { const char *n = ".symtab";   memcpy(shstr+shstr_len, n, strlen(n)); shstr_len += strlen(n); shstr[shstr_len++]=0; }
    { const char *n = ".strtab";   memcpy(shstr+shstr_len, n, strlen(n)); shstr_len += strlen(n); shstr[shstr_len++]=0; }
    int rela_name[16];
    for (int i = 0; i < g_nsecs; i++) {
        int has = 0;
        for (int j = 0; j < g_nrelocs; j++) if (g_relocs[j].sec == i) { has = 1; break; }
        if (!has) continue;
        rela_name[i] = (int)shstr_len;
        char nm[64]; snprintf(nm, sizeof nm, ".rela%s", g_secs[i].name);
        size_t L = strlen(nm);
        memcpy(shstr + shstr_len, nm, L); shstr_len += L; shstr[shstr_len++] = 0;
    }

    uint8_t strtab[16384]; size_t strtab_len = 1; strtab[0] = 0;
    int sym_name[2048];
    sym_name[0] = 0;                       /* NULL sym: no name */
    sym_name[1] = (int)strtab_len;         /* FILE */
    { const char *fs = g_file_sym ? g_file_sym : inpath;
      size_t L = strlen(fs); memcpy(strtab+strtab_len, fs, L); strtab_len += L; strtab[strtab_len++]=0; }
    for (int i = 0; i < g_nsecs; i++) sym_name[2 + i] = 0;   /* nasm: SECTION syms have no name in .strtab */
    int local_base = 2 + g_nsecs;
    for (int i = 0; i < g_nlocals; i++) {
        sym_name[local_base + i] = (int)strtab_len;
        size_t L = strlen(g_locals[i]->name);
        memcpy(strtab + strtab_len, g_locals[i]->name, L);
        strtab_len += L; strtab[strtab_len++] = 0;
    }
    /* nasm drops externs that are never referenced (no relocation targets
     * them), so build the effective global list first. */
    static Sym *g_eff[2048];
    int ngeff = 0;
    for (int i = 0; i < g_nglobals; i++) {
        Sym *sy = g_globals[i];
        if (sy->is_extern && !sy->defined) {
            int used = 0;
            for (int j = 0; j < g_nrelocs; j++)
                if (g_relocs[j].tsec < 0 && g_relocs[j].tsym == sy) { used = 1; break; }
            if (!used) continue;
        }
        g_eff[ngeff++] = sy;
    }
    int global_base = local_base + g_nlocals;
    for (int i = 0; i < ngeff; i++) {
        sym_name[global_base + i] = (int)strtab_len;
        size_t L = strlen(g_eff[i]->name);
        memcpy(strtab + strtab_len, g_eff[i]->name, L);
        strtab_len += L; strtab[strtab_len++] = 0;
    }
    int nsyms = global_base + ngeff;

    /* --- build .symtab --- */
    Elf64_Sym *symtab = xmalloc((size_t)nsyms * sizeof(Elf64_Sym));
    memset(symtab, 0, (size_t)nsyms * sizeof(Elf64_Sym));
    /* [0] NULL: all zero */
    /* [1] FILE (STT_FILE = 4) */
    symtab[1].st_name = (uint32_t)sym_name[1];
    symtab[1].st_info = (0 << 4) | 4;      /* LOCAL, FILE */
    symtab[1].st_shndx = SHN_ABS;
    /* SECTION syms (STT_SECTION = 3) */
    for (int i = 0; i < g_nsecs; i++) {
        Elf64_Sym *s = &symtab[2 + i];
        s->st_name = (uint32_t)sym_name[2 + i];
        s->st_info = (0 << 4) | 3;         /* LOCAL, SECTION */
        s->st_shndx = (uint16_t)g_secs[i].secidx;
    }
    /* locals */
    for (int i = 0; i < g_nlocals; i++) {
        Sym *sy = g_locals[i];
        Elf64_Sym *s = &symtab[local_base + i];
        s->st_name = (uint32_t)sym_name[local_base + i];
        s->st_info = (0 << 4) | 0;         /* LOCAL, NOTYPE */
        s->st_value = (uint64_t)sy->val;
        if (sy->sec >= 0) s->st_shndx = (uint16_t)g_secs[sy->sec].secidx;
        else if (sy->is_equ) s->st_shndx = SHN_ABS;
        else s->st_shndx = SHN_UNDEF;
    }
    /* globals */
    for (int i = 0; i < ngeff; i++) {
        Sym *sy = g_eff[i];
        Elf64_Sym *s = &symtab[global_base + i];
        s->st_name = (uint32_t)sym_name[global_base + i];
        s->st_info = (1 << 4) | 0;         /* GLOBAL, NOTYPE */
        s->st_value = (uint64_t)(sy->defined ? sy->val : 0);
        if (sy->is_extern) s->st_shndx = SHN_UNDEF;
        else if (sy->sec >= 0) s->st_shndx = (uint16_t)g_secs[sy->sec].secidx;
        else s->st_shndx = SHN_UNDEF;
    }
    int first_global = global_base;

    /* --- build .rela sections (one per section with relocs) --- */
    Elf64_Rela *rela[16]; size_t rela_n[16]; int rela_sec[16];
    memset(rela, 0, sizeof rela); memset(rela_n, 0, sizeof rela_n);
    for (int i = 0; i < g_nsecs; i++) rela_sec[i] = -1;
    int nrela = 0;
    for (int i = 0; i < g_nsecs; i++) {
        for (int j = 0; j < g_nrelocs; j++)
            if (g_relocs[j].sec == i) { rela_sec[i] = nrela++; break; }
    }
    for (int i = 0; i < g_nsecs; i++) {
        if (rela_sec[i] < 0) continue;
        int cnt = 0;
        for (int j = 0; j < g_nrelocs; j++) if (g_relocs[j].sec == i) cnt++;
        rela[rela_sec[i]] = xmalloc((size_t)cnt * sizeof(Elf64_Rela));
        memset(rela[rela_sec[i]], 0, (size_t)cnt * sizeof(Elf64_Rela));
    }
    for (int i = 0; i < g_nrelocs; i++) {
        Reloc *r = &g_relocs[i];
        Elf64_Rela *e = &rela[rela_sec[r->sec]][rela_n[rela_sec[r->sec]]++];
        e->r_offset = r->off;
        int symidx;
        if (r->tsec >= 0) symidx = 2 + r->tsec;         /* SECTION symbol */
        else symidx = -1;                                /* extern: resolve below */
        if (symidx < 0) {
            /* find the extern in the effective global list */
            for (int k = 0; k < ngeff; k++)
                if (g_eff[k] == r->tsym) { symidx = global_base + k; break; }
            if (symidx < 0) die("relocation target '%s' never declared", r->tsym ? r->tsym->name : "?");
        }
        e->r_info = ((uint64_t)symidx << 32) | (uint32_t)r->type;
        e->r_addend = r->addend;
    }

    /* --- lay out the file --- */
    obj = NULL; obj_len = 0; obj_cap = 0;
    obj_zero(64);   /* ELF header placeholder */
    uint64_t sh_offset[16]; uint64_t sh_size[16];
    for (int i = 0; i < g_nsecs; i++) {
        obj_align((size_t)(g_secs[i].align ? g_secs[i].align : 1));
        sh_offset[i] = obj_len;
        sh_size[i] = (uint64_t)g_secs[i].size;
        if (g_secs[i].type != SHT_NOBITS)
            obj_bytes(g_secs[i].data ? g_secs[i].data : (uint8_t*)"", g_secs[i].len);
    }
    uint64_t shstr_off = obj_len; obj_align(1); obj_bytes(shstr, shstr_len);
    uint64_t strtab_off = obj_len; obj_align(1); obj_bytes(strtab, strtab_len);
    obj_align(8);
    uint64_t symtab_off = obj_len;
    for (int i = 0; i < nsyms; i++) {
        uint8_t b[24];
        le32(b, symtab[i].st_name); b[4] = symtab[i].st_info; b[5] = symtab[i].st_other;
        le16(b+6, symtab[i].st_shndx); le64(b+8, symtab[i].st_value); le64(b+16, symtab[i].st_size);
        obj_bytes(b, 24);
    }
    uint64_t rela_off[16]; uint64_t rela_size[16];
    for (int i = 0; i < g_nsecs; i++) {
        if (rela_sec[i] < 0) continue;
        obj_align(8);
        rela_off[rela_sec[i]] = obj_len;
        rela_size[rela_sec[i]] = rela_n[rela_sec[i]] * 24;
        for (size_t j = 0; j < rela_n[rela_sec[i]]; j++) {
            uint8_t b[24];
            le64(b, rela[rela_sec[i]][j].r_offset);
            le64(b+8, rela[rela_sec[i]][j].r_info);
            le64(b+16, (uint64_t)rela[rela_sec[i]][j].r_addend);
            obj_bytes(b, 24);
        }
    }
    obj_align(8);
    uint64_t e_shoff = obj_len;
    int e_shnum = 1 + g_nsecs + 3 + nrela_secs;
    int e_shstrndx = shstr_idx;

    /* --- ELF header --- */
    uint8_t eh[64]; memset(eh, 0, sizeof eh);
    eh[0]=0x7F; eh[1]='E'; eh[2]='L'; eh[3]='F';
    eh[4]=2;    /* ELFCLASS64 */
    eh[5]=1;    /* ELFDATA2LSB */
    eh[6]=1;    /* EV_CURRENT */
    le16(eh+16, 1);      /* ET_REL */
    le16(eh+18, 62);     /* EM_X86_64 */
    le32(eh+20, 1);      /* e_version */
    le64(eh+40, e_shoff);
    le16(eh+52, 64);     /* e_ehsize */
    le16(eh+54, 0);      /* e_phentsize */
    le16(eh+56, 0);      /* e_phnum */
    le16(eh+58, 64);     /* e_shentsize */
    le16(eh+60, (uint16_t)e_shnum);
    le16(eh+62, (uint16_t)e_shstrndx);
    memcpy(obj, eh, 64);

    /* --- section headers --- */
    Elf64_Shdr *sh = xmalloc((size_t)e_shnum * sizeof(Elf64_Shdr));
    memset(sh, 0, (size_t)e_shnum * sizeof(Elf64_Shdr));
    for (int i = 0; i < g_nsecs; i++) {
        sh[i+1].sh_name = (uint32_t)sh_name[i];
        sh[i+1].sh_type = (uint32_t)g_secs[i].type;
        sh[i+1].sh_flags = (uint64_t)g_secs[i].flags;
        sh[i+1].sh_offset = sh_offset[i];
        sh[i+1].sh_size = sh_size[i];
        sh[i+1].sh_addralign = (uint64_t)g_secs[i].align;
    }
    {
        /* shstrtab layout: user section names, then ".shstrtab", ".symtab",
         * ".strtab" and ".rela.<sec>" names, in that append order. */
        int off = 1;
        for (int i = 0; i < g_nsecs; i++) off += (int)strlen(g_secs[i].name) + 1;
        int o_shstrtab = off;
        int o_symtab   = o_shstrtab + (int)strlen(".shstrtab") + 1;
        int o_strtab   = o_symtab   + (int)strlen(".symtab")   + 1;
        sh[shstr_idx].sh_name = (uint32_t)o_shstrtab;
        sh[symtab_idx].sh_name = (uint32_t)o_symtab;
        sh[strtab_idx].sh_name = (uint32_t)o_strtab;
    }
    sh[shstr_idx].sh_type = SHT_STRTAB;
    sh[shstr_idx].sh_offset = shstr_off;
    sh[shstr_idx].sh_size = shstr_len;
    sh[shstr_idx].sh_addralign = 1;
    sh[symtab_idx].sh_type = SHT_SYMTAB;
    sh[symtab_idx].sh_offset = symtab_off;
    sh[symtab_idx].sh_size = (uint64_t)nsyms * 24;
    sh[symtab_idx].sh_link = (uint32_t)strtab_idx;
    sh[symtab_idx].sh_info = (uint32_t)first_global;
    sh[symtab_idx].sh_addralign = 8;
    sh[symtab_idx].sh_entsize = 24;
    sh[strtab_idx].sh_type = SHT_STRTAB;
    sh[strtab_idx].sh_offset = strtab_off;
    sh[strtab_idx].sh_size = strtab_len;
    sh[strtab_idx].sh_addralign = 1;
    for (int i = 0; i < g_nsecs; i++) {
        int ri = rela_sec[i];
        if (ri < 0) continue;
        int idx = strtab_idx + 1 + ri;
        sh[idx].sh_name = (uint32_t)rela_name[i];
        sh[idx].sh_type = SHT_RELA;
        sh[idx].sh_offset = rela_off[ri];
        sh[idx].sh_size = rela_size[ri];
        sh[idx].sh_link = (uint32_t)symtab_idx;
        sh[idx].sh_info = (uint32_t)g_secs[i].secidx;
        sh[idx].sh_addralign = 8;
        sh[idx].sh_entsize = 24;
    }
    for (int i = 0; i < e_shnum; i++) {
        uint8_t b[64]; memset(b, 0, 64);
        le32(b, sh[i].sh_name); le32(b+4, sh[i].sh_type);
        le64(b+8, sh[i].sh_flags); le64(b+16, sh[i].sh_addr);
        le64(b+24, sh[i].sh_offset); le64(b+32, sh[i].sh_size);
        le32(b+40, sh[i].sh_link); le32(b+44, sh[i].sh_info);
        le64(b+48, sh[i].sh_addralign); le64(b+56, sh[i].sh_entsize);
        obj_bytes(b, 64);
    }

    FILE *f = fopen(outpath, "wb");
    if (!f) { fprintf(stderr, "mini-asm: cannot write '%s'\n", outpath); exit(2); }
    fwrite(obj, 1, obj_len, f);
    fclose(f);
}


/* ------------------------------------------------------------------ */
/* ELF32 writer (SH4e).  Same symtab/strtab ordering as ELF64 (the SH4d   */
/* rules carry over unchanged: FILE, SECTION, LOCALs incl. equ ABS,       */
/* GLOBAL with unused externs dropped); only the record sizes and the     */
/* relocation format differ -- ELF32 uses SHT_REL (addend in the field,   */
/* which the emit paths already patched), ELF64 uses SHT_RELA.            */
/* ------------------------------------------------------------------ */

static void write_elf32(const char *inpath, const char *outpath) {
    int shstr_idx = 1 + g_nsecs;
    int symtab_idx = shstr_idx + 1;
    int strtab_idx = symtab_idx + 1;
    int nrela_secs = 0;
    for (int i = 0; i < g_nsecs; i++)
        for (int j = 0; j < g_nrelocs; j++)
            if (g_relocs[j].sec == i) { nrela_secs++; break; }

    uint8_t shstr[4096]; size_t shstr_len = 1; shstr[0] = 0;
    int sh_name[16];
    for (int i = 0; i < g_nsecs; i++) {
        sh_name[i] = (int)shstr_len;
        size_t L = strlen(g_secs[i].name);
        memcpy(shstr + shstr_len, g_secs[i].name, L);
        shstr_len += L; shstr[shstr_len++] = 0;
    }
    { const char *n = ".shstrtab"; memcpy(shstr+shstr_len, n, strlen(n)); shstr_len += strlen(n); shstr[shstr_len++]=0; }
    { const char *n = ".symtab";   memcpy(shstr+shstr_len, n, strlen(n)); shstr_len += strlen(n); shstr[shstr_len++]=0; }
    { const char *n = ".strtab";   memcpy(shstr+shstr_len, n, strlen(n)); shstr_len += strlen(n); shstr[shstr_len++]=0; }
    int rela_name[16];
    for (int i = 0; i < g_nsecs; i++) {
        int has = 0;
        for (int j = 0; j < g_nrelocs; j++) if (g_relocs[j].sec == i) { has = 1; break; }
        if (!has) continue;
        rela_name[i] = (int)shstr_len;
        char nm[64]; snprintf(nm, sizeof nm, ".rel%s", g_secs[i].name);
        size_t L = strlen(nm);
        memcpy(shstr + shstr_len, nm, L); shstr_len += L; shstr[shstr_len++] = 0;
    }

    uint8_t strtab[16384]; size_t strtab_len = 1; strtab[0] = 0;
    int sym_name[2048];
    sym_name[0] = 0;
    sym_name[1] = (int)strtab_len;
    { const char *fs = g_file_sym ? g_file_sym : inpath;
      size_t L = strlen(fs); memcpy(strtab+strtab_len, fs, L); strtab_len += L; strtab[strtab_len++]=0; }
    for (int i = 0; i < g_nsecs; i++) sym_name[2 + i] = 0;
    int local_base = 2 + g_nsecs;
    for (int i = 0; i < g_nlocals; i++) {
        sym_name[local_base + i] = (int)strtab_len;
        size_t L = strlen(g_locals[i]->name);
        memcpy(strtab + strtab_len, g_locals[i]->name, L);
        strtab_len += L; strtab[strtab_len++] = 0;
    }
    static Sym *g_eff[2048];
    int ngeff = 0;
    for (int i = 0; i < g_nglobals; i++) {
        Sym *sy = g_globals[i];
        if (sy->is_extern && !sy->defined) {
            int used = 0;
            for (int j = 0; j < g_nrelocs; j++)
                if (g_relocs[j].tsec < 0 && g_relocs[j].tsym == sy) { used = 1; break; }
            if (!used) continue;
        }
        g_eff[ngeff++] = sy;
    }
    int global_base = local_base + g_nlocals;
    for (int i = 0; i < ngeff; i++) {
        sym_name[global_base + i] = (int)strtab_len;
        size_t L = strlen(g_eff[i]->name);
        memcpy(strtab + strtab_len, g_eff[i]->name, L);
        strtab_len += L; strtab[strtab_len++] = 0;
    }
    int nsyms = global_base + ngeff;

    /* Elf32_Sym: name(4) value(4) size(4) info(1) other(1) shndx(2) */
    uint8_t symtab[2048 * 16];
    memset(symtab, 0, sizeof symtab);
    {   uint8_t *s = symtab + 16;
        le32(s, (uint32_t)sym_name[1]);
        s[12] = (0 << 4) | 4;   /* LOCAL, FILE */
        le16(s+14, SHN_ABS);
    }
    for (int i = 0; i < g_nsecs; i++) {
        uint8_t *s = symtab + (2 + i) * 16;
        le32(s, 0);            /* SECTION syms: no name */
        s[12] = (0 << 4) | 3;  /* LOCAL, SECTION */
        le16(s+14, (uint16_t)g_secs[i].secidx);
    }
    for (int i = 0; i < g_nlocals; i++) {
        Sym *sy = g_locals[i];
        uint8_t *s = symtab + (local_base + i) * 16;
        le32(s, (uint32_t)sym_name[local_base + i]);
        le32(s+4, (uint32_t)sy->val);
        s[12] = (0 << 4) | 0;
        if (sy->sec >= 0) le16(s+14, (uint16_t)g_secs[sy->sec].secidx);
        else if (sy->is_equ) le16(s+14, SHN_ABS);
        else le16(s+14, SHN_UNDEF);
    }
    for (int i = 0; i < ngeff; i++) {
        Sym *sy = g_eff[i];
        uint8_t *s = symtab + (global_base + i) * 16;
        le32(s, (uint32_t)sym_name[global_base + i]);
        le32(s+4, (uint32_t)(sy->defined ? sy->val : 0));
        s[12] = (1 << 4) | 0;
        if (sy->is_extern) le16(s+14, SHN_UNDEF);
        else if (sy->sec >= 0) le16(s+14, (uint16_t)g_secs[sy->sec].secidx);
        else le16(s+14, SHN_UNDEF);
    }
    int first_global = global_base;

    /* Elf32_Rel: offset(4) info(4); addends already live in the fields.
     * (heap: 16x64 KiB on the stack would hurt the 4 MiB guest stack) */
    uint8_t (*relbuf)[65536] = xmalloc(16 * 65536);
    size_t reln[16];
    int rela_sec[16];
    memset(reln, 0, sizeof reln);
    for (int i = 0; i < g_nsecs; i++) rela_sec[i] = -1;
    int nrela = 0;
    for (int i = 0; i < g_nsecs; i++)
        for (int j = 0; j < g_nrelocs; j++)
            if (g_relocs[j].sec == i) { rela_sec[i] = nrela++; break; }
    for (int i = 0; i < g_nrelocs; i++) {
        Reloc *r = &g_relocs[i];
        int symidx;
        if (r->tsec >= 0) symidx = 2 + r->tsec;
        else {
            symidx = -1;
            for (int k = 0; k < ngeff; k++)
                if (g_eff[k] == r->tsym) { symidx = global_base + k; break; }
            if (symidx < 0) die("relocation target '%s' never declared", r->tsym ? r->tsym->name : "?");
        }
        uint8_t *e = relbuf[rela_sec[r->sec]] + reln[rela_sec[r->sec]] * 8;
        le32(e, (uint32_t)r->off);
        le32(e+4, ((uint32_t)symidx << 8) | (uint32_t)r->type);
        reln[rela_sec[r->sec]]++;
    }

    /* layout */
    obj = NULL; obj_len = 0; obj_cap = 0;
    obj_zero(52);
    uint32_t sh_offset[16]; uint32_t sh_size[16];
    for (int i = 0; i < g_nsecs; i++) {
        obj_align((size_t)(g_secs[i].align ? g_secs[i].align : 1));
        sh_offset[i] = (uint32_t)obj_len;
        sh_size[i] = (uint32_t)g_secs[i].size;
        if (g_secs[i].type != SHT_NOBITS)
            obj_bytes(g_secs[i].data ? g_secs[i].data : (uint8_t*)"", g_secs[i].len);
    }
    uint32_t shstr_off = (uint32_t)obj_len; obj_bytes(shstr, shstr_len);
    uint32_t strtab_off = (uint32_t)obj_len; obj_bytes(strtab, strtab_len);
    obj_align(4);
    uint32_t symtab_off = (uint32_t)obj_len;
    obj_bytes(symtab, (size_t)nsyms * 16);
    uint32_t rela_off[16];
    for (int i = 0; i < g_nsecs; i++) {
        int ri = rela_sec[i];
        if (ri < 0) continue;
        obj_align(4);
        rela_off[ri] = (uint32_t)obj_len;
        obj_bytes(relbuf[ri], reln[ri] * 8);
    }
    obj_align(4);
    uint32_t e_shoff = (uint32_t)obj_len;
    int e_shnum = 1 + g_nsecs + 3 + nrela_secs;
    int e_shstrndx = shstr_idx;

    /* ELF32 header (52 bytes) */
    uint8_t eh[52]; memset(eh, 0, sizeof eh);
    eh[0]=0x7F; eh[1]='E'; eh[2]='L'; eh[3]='F';
    eh[4]=1; eh[5]=1; eh[6]=1;
    le16(eh+16, 1);      /* ET_REL */
    le16(eh+18, 3);      /* EM_386 */
    le32(eh+20, 1);
    le32(eh+32, e_shoff);
    le16(eh+40, 52);     /* e_ehsize */
    le16(eh+46, 40);     /* e_shentsize */
    le16(eh+48, (uint16_t)e_shnum);
    le16(eh+50, (uint16_t)e_shstrndx);
    memcpy(obj, eh, 52);

    /* section headers (40 bytes each) */
    for (int i = 0; i < e_shnum; i++) {
        uint8_t b[40]; memset(b, 0, 40);
        if (i == 0) { obj_bytes(b, 40); continue; }
        if (i <= g_nsecs) {
            int s = i - 1;
            le32(b, (uint32_t)sh_name[s]);
            le32(b+4, (uint32_t)g_secs[s].type);
            le32(b+8, (uint32_t)g_secs[s].flags);
            le32(b+16, sh_offset[s]);
            le32(b+20, sh_size[s]);
            le32(b+32, (uint32_t)g_secs[s].align);
            obj_bytes(b, 40);
            continue;
        }
        if (i == shstr_idx) {
            int off = 1;
            for (int s = 0; s < g_nsecs; s++) off += (int)strlen(g_secs[s].name) + 1;
            le32(b, (uint32_t)off);
            le32(b+4, 3);              /* SHT_STRTAB */
            le32(b+16, shstr_off);
            le32(b+20, (uint32_t)shstr_len);
            le32(b+32, 1);
            obj_bytes(b, 40); continue;
        }
        if (i == symtab_idx) {
            int off = 1;
            for (int s = 0; s < g_nsecs; s++) off += (int)strlen(g_secs[s].name) + 1;
            le32(b, (uint32_t)(off + (int)strlen(".shstrtab") + 1));
            le32(b+4, 2);              /* SHT_SYMTAB */
            le32(b+16, symtab_off);
            le32(b+20, (uint32_t)nsyms * 16);
            le32(b+24, (uint32_t)strtab_idx);
            le32(b+28, (uint32_t)first_global);
            le32(b+32, 4);
            le32(b+36, 16);            /* entsize */
            obj_bytes(b, 40); continue;
        }
        if (i == strtab_idx) {
            int off = 1;
            for (int s = 0; s < g_nsecs; s++) off += (int)strlen(g_secs[s].name) + 1;
            le32(b, (uint32_t)(off + (int)strlen(".shstrtab") + 1 + (int)strlen(".symtab") + 1));
            le32(b+4, 3);              /* SHT_STRTAB */
            le32(b+16, strtab_off);
            le32(b+20, (uint32_t)strtab_len);
            le32(b+32, 1);
            obj_bytes(b, 40); continue;
        }
        /* .rel.<sec> */
        {
            int ri = i - (strtab_idx + 1);
            int sec = -1;
            for (int s = 0; s < g_nsecs; s++) if (rela_sec[s] == ri) { sec = s; break; }
            le32(b, (uint32_t)rela_name[sec]);
            le32(b+4, 9);              /* SHT_REL */
            le32(b+16, rela_off[ri]);
            le32(b+20, (uint32_t)(reln[ri] * 8));
            le32(b+24, (uint32_t)symtab_idx);
            le32(b+28, (uint32_t)g_secs[sec].secidx);
            le32(b+32, 4);
            le32(b+36, 8);             /* entsize */
            obj_bytes(b, 40);
        }
    }

    FILE *f = fopen(outpath, "wb");
    if (!f) { fprintf(stderr, "mini-asm: cannot write '%s'\n", outpath); exit(2); }
    fwrite(obj, 1, obj_len, f);
    fclose(f);
    free(relbuf);
}

/* reset all per-source global state so a second input file starts clean */
static void reset_asm_state(void) {
    g_nlines = 0;
    g_nplines = 0;
    g_nsyms = 0;
    g_nlocals = 0;
    g_nglobals = 0;
    g_nsecs = 0;
    g_nrelocs = 0;
    g_ntmacros = 0;
    g_nmacrodefs = 0;
    g_cur_global = "";
    g_cursec = -1;
    g_bits = 16;
    g_default_rel = 0;
    g_line_no = 0;
}

/* assemble one source to outpath (bin/elf32/elf64) */
static int assemble_source(const char *inpath, const char *outpath) {
    load_file(inpath);

    /* equ constants (pc-independent) first, so forward references resolve.
     * NOTE: sym_local_add() happens later in assemble_line(), at the line's
     * source position -- nasm emits equ constants in definition order
     * interleaved with labels, not all upfront (boot32's STACK_SIZE, defined
     * at line 87, lands after .fill_pde at line 52). */
    for (int i = 0; i < g_nlines; i++) {
        AsmLine *L = &g_lines[i];
        if (L->is_equ) {
            Sym *s = sym_intern(L->mnem);
            s->val = eval_str(L->equ_expr);
            s->defined = 1;
            s->is_equ = 1;
            s->sec = -2;   /* ABS */
        }
    }

    /* fixed-point jump sizing; forward refs tolerated during sizing */
    g_allow_undef = 1;
    int changed = 1, iter = 0;
    while (changed && iter < 50) {
        changed = 0; iter++;
        g_pc = g_org = 0;
        g_cur_global = "";
        g_cursec = -1;
        g_default_rel = 0;
        out_reset();
        sections_reset();
        for (int i = 0; i < g_nlines; i++)
            assemble_line(&g_lines[i], 0, &changed);
    }

    /* final emit: every symbol must now be defined */
    g_allow_undef = 0;
    g_pc = g_org = 0;
    g_cur_global = "";
    g_cursec = -1;
    g_default_rel = 0;
    g_nrelocs = 0;
    out_reset();
    sections_reset();
    for (int i = 0; i < g_nlines; i++)
        assemble_line(&g_lines[i], 1, NULL);

    if (g_fmt_elf == 1) { write_elf(inpath, outpath); return 0; }
    if (g_fmt_elf == 2) { write_elf32(inpath, outpath); return 0; }
    FILE *f = fopen(outpath, "wb");
    if (!f) { fprintf(stderr, "mini-asm: cannot write '%s'\n", outpath); return 2; }
    fwrite(g_out, 1, g_out_len, f);
    fclose(f);
    return 0;
}

/* strip the directory part of a path (for --check-dir output naming) */
static const char *path_basename(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/') b = q + 1;
    return b;
}

int main(int argc, char **argv) {
    const char *inputs[64]; int ninputs = 0;
    const char *outpath = NULL, *checkdir = NULL, *fmt = "bin";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) fmt = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outpath = argv[++i];
        else if (!strcmp(argv[i], "--check-dir") && i + 1 < argc) checkdir = argv[++i];
        else if (!strcmp(argv[i], "--file-sym") && i + 1 < argc) g_file_sym = argv[++i];
        else if (!strcmp(argv[i], "-I") && i + 1 < argc) {
            if (g_nincdirs < 16) g_incdirs[g_nincdirs++] = argv[++i];
            else i++;
        }
        else if (!strncmp(argv[i], "-I", 2) && argv[i][2]) {
            if (g_nincdirs < 16) g_incdirs[g_nincdirs++] = argv[i] + 2;
        }
        else if (argv[i][0] == '-') { fprintf(stderr, "mini-asm: unknown option '%s'\n", argv[i]); return 2; }
        else if (ninputs < 64) inputs[ninputs++] = argv[i];
    }
    if (!ninputs) { fprintf(stderr, "usage: mini-asm -f bin|elf32|elf64 [-I dir] [-o out|--check-dir refdir] input.asm [...]\n"); return 2; }
    g_fmt_elf = !strcmp(fmt, "elf64") ? 1 : (!strcmp(fmt, "elf32") ? 2 : 0);
    if (strcmp(fmt, "bin") && !g_fmt_elf) {
        fprintf(stderr, "mini-asm: supports -f bin, -f elf32 and -f elf64 (SH4e)\n");
        return 2;
    }

    /* --check-dir mode: assemble every input into the directory named by -o
     * (default ".") and byte-compare each result against refdir/<base>.o,
     * printing the SH4e in-guest receipt.  Without --check-dir, a single
     * input writes to -o exactly (backward compatible); multiple inputs with
     * -o write into the directory. */
    int bad = 0, identical = 0, total = 0;
    for (int i = 0; i < ninputs; i++) {
        const char *in = inputs[i];
        const char *base = path_basename(in);
        char out[1024];
        if (checkdir) {
            char b2[512]; snprintf(b2, sizeof b2, "%s", base);
            char *dot = strrchr(b2, '.');
            if (dot) *dot = 0;
            snprintf(out, sizeof out, "%s/%s.o", outpath ? outpath : ".", b2);
        } else if (ninputs == 1) {
            snprintf(out, sizeof out, "%s", outpath ? outpath : (g_fmt_elf ? "a.o" : "a.bin"));
        } else {
            char b2[512]; snprintf(b2, sizeof b2, "%s", base);
            char *dot = strrchr(b2, '.');
            if (dot) *dot = 0;
            snprintf(out, sizeof out, "%s/%s.o", outpath ? outpath : ".", b2);
        }
        if (i > 0) reset_asm_state();
        if (assemble_source(in, out) != 0) { bad = 1; continue; }
        total++;
        if (checkdir) {
            char b2[512]; snprintf(b2, sizeof b2, "%s", base);
            char *dot = strrchr(b2, '.');
            if (dot) *dot = 0;
            char ref[1024];
            snprintf(ref, sizeof ref, "%s/%s.o", checkdir, b2);
            FILE *a = fopen(out, "rb"), *b = fopen(ref, "rb");
            int same = 0;
            if (a && b) {
                int c, d; same = 1;
                for (;;) {
                    c = fgetc(a); d = fgetc(b);
                    if (c != d) { same = 0; break; }
                    if (c == EOF) break;
                }
                if (c == EOF && d != EOF) same = 0;
            } else same = 0;
            if (a) fclose(a);
            if (b) fclose(b);
            if (same) {
                printf("[selfhost] asm %s byte-identical\n", base);
                identical++;
            } else {
                printf("[selfhost] asm %s DIFFERS from reference\n", base);
                bad = 1;
            }
        }
    }
    if (checkdir) {
        if (!bad) {
            printf("[selfhost] asm PASS: %d/%d objects byte-identical\n", identical, total);
            return 0;
        }
        printf("[selfhost] asm FAIL: %d/%d objects byte-identical\n", identical, total);
        return 1;
    }
    return bad ? 1 : 0;
}
