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
    long long  val;
    int        defined;
} Sym;

static Sym  g_syms[1024];
static int  g_nsyms = 0;

static Sym *sym_find(const char *name) {
    for (int i = 0; i < g_nsyms; i++)
        if (strcmp(g_syms[i].name, name) == 0)
            return &g_syms[i];
    return NULL;
}

static Sym *sym_intern(const char *name) {
    Sym *s = sym_find(name);
    if (s) return s;
    if (g_nsyms >= 1024) die("symbol table overflow");
    g_syms[g_nsyms].name = xstrdup(name);
    g_syms[g_nsyms].val = 0;
    g_syms[g_nsyms].defined = 0;
    return &g_syms[g_nsyms++];
}

/* ------------------------------------------------------------------ */
/* output buffer                                                       */
/* ------------------------------------------------------------------ */

static uint8_t *g_out = NULL;
static size_t   g_out_len = 0;
static size_t   g_out_cap = 0;

static void out_reset(void) { g_out_len = 0; }

static void out_byte(uint8_t b) {
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
        if (c == '|') { e->p++; v |= parse_add(e); }
        else if (c == '^') { e->p++; v ^= parse_add(e); }
        else break;
    }
    return v;
}
static long long parse_bitand(Expr *e) {
    long long v = parse_bitor(e);
    for (;;) {
        skip_ws(e);
        if (*e->p == '&') { e->p++; v &= parse_bitor(e); }
        else break;
    }
    return v;
}
static long long parse_expr(Expr *e) { return parse_bitand(e); }

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

/* ------------------------------------------------------------------ */
/* operands                                                            */
/* ------------------------------------------------------------------ */

enum { OP_NONE, OP_REG, OP_SREG, OP_CR, OP_MEM, OP_IMM, OP_FAR };

typedef struct {
    int kind;
    int reg;        /* register index (0-15) */
    int width;      /* 8/16/32/64 for OP_REG */
    char text[256]; /* IMM/FAR text, or MEM displacement expression */
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

static void parse_operand(const char *s, Operand *o) {
    o->kind = OP_NONE; o->reg = 0; o->width = 0; o->text[0] = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return;
    if (*s == '[') {
        size_t n = strlen(s);
        if (s[n-1] != ']') die("malformed memory operand '%s'", s);
        if (n - 2 >= sizeof o->text) die("memory operand too long");
        memcpy(o->text, s + 1, n - 2);
        o->text[n-2] = 0;
        o->kind = OP_MEM;
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
        if (*q == ':') { o->kind = OP_FAR; snprintf(o->text, sizeof o->text, "%s", s); return; }
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

    if (!strcmp(L->mnem, "jmp") || !strcmp(L->mnem, "call") || jcc_cc(L->mnem) >= 0) {
        if (L->nops == 1) {
            int is_jcc = jcc_cc(L->mnem) >= 0;
            int is_reg = reg8(L->ops[0]) >= 0 || reg16(L->ops[0]) >= 0 ||
                         reg32(L->ops[0]) >= 0 || reg64(L->ops[0]) >= 0;
            int is_far = 0;
            for (const char *q = L->ops[0]; *q; q++) if (*q == ':') { is_far = 1; break; }
            if (!is_jcc && is_reg) {
                L->is_jump = 0;   /* indirect jmp/call reg: fixed size, encode_instr handles it */
            } else {
                L->is_jump = 1;
                L->jump_far = is_far;
                if (!is_far) {
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

static void load_file(const char *path) {
    char *buf = read_whole(path);
    g_cur_global = "";
    int line_no = 0;
    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        line_no++;
        load_line(p, line_no);
        if (!nl) break;
        p = nl + 1;
    }
    free(buf);
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
        long long v = eval_str(s);
        if (emit) out_le((unsigned long long)v, width);
        total += width;
    }
    return total;
}

static int encode_instr(AsmLine *L, int emit, int *changed) {
    const char *m = L->mnem;
    Operand a = {OP_NONE,0,0,{0}}, b = {OP_NONE,0,0,{0}};
    if (L->nops >= 1) parse_operand(L->ops[0], &a);
    if (L->nops >= 2) parse_operand(L->ops[1], &b);

    /* ---- zero-operand ---- */
    if (L->nops == 0) {
        struct { const char *n; int op; } z1[] = {
            {"cli",0xFA},{"sti",0xFB},{"hlt",0xF4},{"ret",0xC3},{"lodsb",0xAC},
            {"cld",0xFC},{"std",0xFD},{"nop",0x90},{NULL,0}
        };
        for (int i = 0; z1[i].n; i++) if (!strcmp(m, z1[i].n)) {
            if (emit) out_byte((uint8_t)z1[i].op);
            return 1;
        }
        struct { const char *n; int op; } z2[] = {   /* 0F xx */
            {"ud2",0x0B},{"rdmsr",0x32},{"wrmsr",0x30},{"cpuid",0xA2},{NULL,0}
        };
        for (int i = 0; z2[i].n; i++) if (!strcmp(m, z2[i].n)) {
            if (emit) { out_byte(0x0F); out_byte((uint8_t)z2[i].op); }
            return 2;
        }
        if (!strcmp(m, "pusha") || !strcmp(m, "pushad")) { if (emit) out_byte(0x60); return 1; }
        if (!strcmp(m, "popa")  || !strcmp(m, "popad"))  { if (emit) out_byte(0x61); return 1; }
        if (!strcmp(m, "pushf") || !strcmp(m, "pushfd")) { if (emit) out_byte(0x9C); return 1; }
        if (!strcmp(m, "popf")  || !strcmp(m, "popfd"))  { if (emit) out_byte(0x9D); return 1; }
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
        if (a.kind == OP_FAR) {                 /* far jmp seg:off -> EA */
            char buf[256]; snprintf(buf, sizeof buf, "%s", a.text);
            char *colon = strchr(buf, ':'); *colon = 0;
            long long seg = eval_str(buf), off = eval_str(colon + 1);
            if (emit) { out_byte(0xEA); out_le((unsigned long long)off, 2);
                        out_le((unsigned long long)seg, 2); }
            return 5;
        }
        /* near displacement jump/call (jump_target resolved at load time) */
        Sym *t = sym_find(L->jump_target);
        long long target = (t && t->defined) ? t->val : 0;
        int cc = jcc_cc(m);
        if (is_call) {
            long long rel = target - (g_pc + 3);
            if (emit) { out_byte(0xE8); out_le((unsigned long long)rel, 2); }
            return 3;
        }
        if (!emit) {
            if (!L->jump_long) {
                long long rel = target - (g_pc + 2);
                if (rel < -128 || rel > 127) { L->jump_long = 1; if (changed) *changed = 1; }
            } else {
                long long rel = target - (g_pc + 3);
                if (rel >= -128 && rel <= 127) { L->jump_long = 0; if (changed) *changed = 1; }
            }
            return L->jump_long ? 3 : 2;
        }
        if (!L->jump_long) {
            long long rel = target - (g_pc + 2);
            if (cc >= 0) out_byte((uint8_t)(0x70 + cc)); else out_byte(0xEB);
            out_byte((uint8_t)rel);
            return 2;
        } else {
            long long rel = target - (g_pc + 3);
            out_byte(0xE9); out_le((unsigned long long)rel, 2);
            return 3;
        }
    }

    /* ---- conditional jumps ---- */
    if (jcc_cc(m) >= 0) {
        int cc = jcc_cc(m);
        Sym *t = sym_find(L->jump_target);
        long long target = (t && t->defined) ? t->val : 0;
        if (!emit) {
            if (!L->jump_long) {
                long long rel = target - (g_pc + 2);
                if (rel < -128 || rel > 127) { L->jump_long = 1; if (changed) *changed = 1; }
            } else {
                long long rel = target - (g_pc + 4);
                if (rel >= -128 && rel <= 127) { L->jump_long = 0; if (changed) *changed = 1; }
            }
            return L->jump_long ? 4 : 2;
        }
        if (!L->jump_long) {
            long long rel = target - (g_pc + 2);
            out_byte((uint8_t)(0x70 + cc)); out_byte((uint8_t)rel);
            return 2;
        } else {
            long long rel = target - (g_pc + 4);
            out_byte(0x0F); out_byte((uint8_t)(0x80 + cc)); out_le((unsigned long long)rel, 2);
            return 4;
        }
    }

    /* ---- lgdt / lidt [mem] ---- */
    if ((!strcmp(m, "lgdt") || !strcmp(m, "lidt")) && a.kind == OP_MEM) {
        int rf = m[2] == 'd' ? 2 : 3;
        long long d = eval_str(a.text);
        if (emit) {
            out_byte(0x0F); out_byte(0x01);
            if (g_bits == 16)      { out_byte((uint8_t)modrm(0, rf, 6)); out_le((unsigned long long)d, 2); }
            else if (g_bits == 32) { out_byte((uint8_t)modrm(0, rf, 5)); out_le((unsigned long long)d, 4); }
            else                   { out_byte((uint8_t)modrm(0, rf, 4)); out_byte(0x25); out_le((unsigned long long)d, 4); }
        }
        return (g_bits == 16) ? 5 : 6;
    }

    /* ---- mov ---- */
    if (!strcmp(m, "mov")) {
        if (a.kind == OP_SREG && b.kind == OP_REG) {
            if (emit) { out_byte(0x8E); out_byte((uint8_t)modrm(3, a.reg, b.reg)); }
            return 2;
        }
        if (a.kind == OP_REG && b.kind == OP_SREG) {
            if (emit) { out_byte(0x8C); out_byte((uint8_t)modrm(3, b.reg, a.reg)); }
            return 2;
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
            long long v = eval_str(b.text);
            int w = a.width, p66 = need_66(w);
            int rex = (g_bits == 64 && (w == 64 || a.reg >= 8))
                      ? (0x40 | (w == 64 ? 8 : 0) | (a.reg >= 8 ? 1 : 0)) : 0;
            int immw = (w == 8) ? 1 : (w == 16 ? 2 : 4);
            if (emit) {
                if (p66) out_byte(0x66);
                if (rex) out_byte((uint8_t)rex);
                out_byte((uint8_t)((w == 8 ? 0xB0 : 0xB8) + (a.reg & 7)));
                out_le((unsigned long long)v, immw);
            }
            return 1 + (p66?1:0) + (rex?1:0) + immw;
        }
        /* mov acc,[abs] / mov [abs],acc -> moffs (bits16/32, shorter than /r) */
        if (g_bits != 64 && a.kind == OP_REG && a.reg == 0 && b.kind == OP_MEM) {
            long long d = eval_str(b.text);
            int w = a.width, p66 = need_66(w), addrw = (g_bits == 16) ? 2 : 4;
            int opc = (w == 8) ? 0xA0 : 0xA1;
            if (emit) { if (p66) out_byte(0x66); out_byte((uint8_t)opc);
                        out_le((unsigned long long)d, addrw); }
            return 1 + (p66?1:0) + addrw;
        }
        if (g_bits != 64 && a.kind == OP_MEM && b.kind == OP_REG && b.reg == 0) {
            long long d = eval_str(a.text);
            int w = b.width, p66 = need_66(w), addrw = (g_bits == 16) ? 2 : 4;
            int opc = (w == 8) ? 0xA2 : 0xA3;
            if (emit) { if (p66) out_byte(0x66); out_byte((uint8_t)opc);
                        out_le((unsigned long long)d, addrw); }
            return 1 + (p66?1:0) + addrw;
        }
        /* mov mem,reg (88/89) ; mov reg,mem (8A/8B) -- absolute disp */
        if ((a.kind == OP_MEM && b.kind == OP_REG) || (a.kind == OP_REG && b.kind == OP_MEM)) {
            int memfirst = (a.kind == OP_MEM);
            Operand *rg = memfirst ? &b : &a;
            Operand *me = memfirst ? &a : &b;
            long long d = eval_str(me->text);
            int w = rg->width, p66 = need_66(w);
            int rex = (g_bits == 64 && (w == 64 || rg->reg >= 8))
                      ? (0x40 | (w == 64 ? 8 : 0) | (rg->reg >= 8 ? 4 : 0)) : 0;
            int opc = memfirst ? (w == 8 ? 0x88 : 0x89) : (w == 8 ? 0x8A : 0x8B);
            int dsz = (g_bits == 16) ? 2 : 4;
            int sib = (g_bits == 64) ? 1 : 0;
            if (emit) {
                if (p66) out_byte(0x66);
                if (rex) out_byte((uint8_t)rex);
                out_byte((uint8_t)opc);
                if (g_bits == 16)      { out_byte((uint8_t)modrm(0, rg->reg, 6)); out_le((unsigned long long)d, 2); }
                else if (g_bits == 32) { out_byte((uint8_t)modrm(0, rg->reg, 5)); out_le((unsigned long long)d, 4); }
                else                   { out_byte((uint8_t)modrm(0, rg->reg, 4)); out_byte(0x25); out_le((unsigned long long)d, 4); }
            }
            return 1 + (p66?1:0) + (rex?1:0) + 1 + sib + dsz;   /* opc + modrm + sib? + disp */
        }
        die("unsupported 'mov' form");
    }

    /* ---- xor / test reg, reg ---- */
    if (!strcmp(m, "xor") || !strcmp(m, "test")) {
        if (a.kind == OP_REG && b.kind == OP_REG && a.width == b.width) {
            int w = a.width, p66 = need_66(w);
            int rex = (g_bits == 64 && (w == 64 || a.reg >= 8 || b.reg >= 8))
                      ? (0x40 | (w == 64 ? 8 : 0) | (b.reg >= 8 ? 4 : 0) | (a.reg >= 8 ? 1 : 0)) : 0;
            int opc = (!strcmp(m, "xor") ? 0x30 : 0x84) + (w == 8 ? 0 : 1);
            if (emit) {
                if (p66) out_byte(0x66);
                if (rex) out_byte((uint8_t)rex);
                out_byte((uint8_t)opc);
                out_byte((uint8_t)modrm(3, b.reg, a.reg));
            }
            return 1 + (p66?1:0) + (rex?1:0) + 1;
        }
        die("unsupported '%s' form", m);
    }

    /* ---- add/or/adc/sbb/and/sub/xor/cmp reg, imm ---- */
    {
        struct { const char *n; int digit; int acc32; } ar[] = {
            {"add",0,0x05},{"or",1,0x0D},{"adc",2,0x15},{"sbb",3,0x1D},
            {"and",4,0x25},{"sub",5,0x2D},{"xor",6,0x35},{"cmp",7,0x3D},{NULL,0,0}
        };
        for (int i = 0; ar[i].n; i++) {
            if (strcmp(m, ar[i].n)) continue;
            if (a.kind == OP_REG && b.kind == OP_IMM) {
                long long v = eval_str(b.text);
                int w = a.width, p66 = need_66(w);
                int rex = (g_bits == 64 && (w == 64 || a.reg >= 8))
                          ? (0x40 | (w == 64 ? 8 : 0) | (a.reg >= 8 ? 1 : 0)) : 0;
                int is_acc = (a.reg == 0) && (w != 8);
                if (v >= -128 && v <= 127) {           /* 83 /digit imm8 */
                    if (emit) {
                        if (p66) out_byte(0x66);
                        if (rex) out_byte((uint8_t)rex);
                        out_byte(0x83);
                        out_byte((uint8_t)modrm(3, ar[i].digit, a.reg));
                        out_byte((uint8_t)v);
                    }
                    return 1 + (p66?1:0) + (rex?1:0) + 1 + 1;
                }
                int immw = (w == 16) ? 2 : 4;
                if (is_acc) {                          /* 05/0D/... imm */
                    if (emit) {
                        if (p66) out_byte(0x66);
                        if (rex) out_byte((uint8_t)rex);
                        out_byte((uint8_t)ar[i].acc32);
                        out_le((unsigned long long)v, immw);
                    }
                    return 1 + (p66?1:0) + (rex?1:0) + immw;
                }
                if (emit) {                            /* 81 /digit imm */
                    if (p66) out_byte(0x66);
                    if (rex) out_byte((uint8_t)rex);
                    out_byte(0x81);
                    out_byte((uint8_t)modrm(3, ar[i].digit, a.reg));
                    out_le((unsigned long long)v, immw);
                }
                return 1 + (p66?1:0) + (rex?1:0) + 1 + immw;
            }
            die("unsupported '%s' form", m);
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
    }
    if (!L->mnem) return 0;
    if (L->is_equ) return 0;   /* constants resolved before assembly */

    const char *m = L->mnem;

    if (!strcmp(m, "bits")) {
        long long v = eval_str(L->ops[0]);
        if (v != 16 && v != 32 && v != 64) die("unsupported 'bits %lld' (16/32/64)", v);
        g_bits = (int)v;
        return 0;
    }
    if (!strcmp(m, "org")) { g_org = eval_str(L->ops[0]); g_pc = g_org; return 0; }
    if (!strcmp(m, "db") || !strcmp(m, "dw") || !strcmp(m, "dd") || !strcmp(m, "dq")) {
        int n = emit_data(m, L, emit); g_pc += n; return n;
    }
    if (!strcmp(m, "resb") || !strcmp(m, "resw") || !strcmp(m, "resd") || !strcmp(m, "resq")) {
        int width = m[3] == 'b' ? 1 : m[3] == 'w' ? 2 : m[3] == 'd' ? 4 : 8;
        long long n = eval_str(L->ops[0]) * width;
        if (emit) for (long long i = 0; i < n; i++) out_byte(0);
        g_pc += n; return (int)n;
    }
    if (!strcmp(m, "align") || !strcmp(m, "alignb")) {
        long long a = eval_str(L->ops[0]);
        long long off = g_pc - g_org;
        long long rem = off % a;
        long long pad = rem ? (a - rem) : 0;
        uint8_t fill = !strcmp(m, "alignb") ? 0x00 : 0x90;
        if (emit) for (long long i = 0; i < pad; i++) out_byte(fill);
        g_pc += pad; return (int)pad;
    }
    if (!strcmp(m, "times")) return do_times(L, emit, changed);

    int n = encode_instr(L, emit, changed);
    g_pc += n;
    return n;
}

/* ------------------------------------------------------------------ */
/* driver                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    const char *inpath = NULL, *outpath = NULL, *fmt = "bin";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) fmt = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outpath = argv[++i];
        else if (!strcmp(argv[i], "-I")) i++;   /* include path: SH4b */
        else if (argv[i][0] == '-') { fprintf(stderr, "mini-asm: unknown option '%s'\n", argv[i]); return 2; }
        else inpath = argv[i];
    }
    if (!inpath) { fprintf(stderr, "usage: mini-asm -f bin input.asm -o out.bin\n"); return 2; }
    if (strcmp(fmt, "bin")) { fprintf(stderr, "mini-asm: SH4a supports -f bin only\n"); return 2; }

    load_file(inpath);

    /* equ constants (pc-independent) first */
    for (int i = 0; i < g_nlines; i++) {
        AsmLine *L = &g_lines[i];
        if (L->is_equ) {
            Sym *s = sym_intern(L->mnem);
            s->val = eval_str(L->equ_expr);
            s->defined = 1;
        }
    }

    /* fixed-point jump sizing; forward refs tolerated during sizing */
    g_allow_undef = 1;
    int changed = 1, iter = 0;
    while (changed && iter < 50) {
        changed = 0; iter++;
        g_pc = g_org = 0;
        g_cur_global = "";
        out_reset();
        for (int i = 0; i < g_nlines; i++)
            assemble_line(&g_lines[i], 0, &changed);
    }

    /* final emit: every symbol must now be defined */
    g_allow_undef = 0;
    g_pc = g_org = 0;
    g_cur_global = "";
    out_reset();
    for (int i = 0; i < g_nlines; i++)
        assemble_line(&g_lines[i], 1, NULL);

    if (!outpath) outpath = "a.bin";
    FILE *f = fopen(outpath, "wb");
    if (!f) { fprintf(stderr, "mini-asm: cannot write '%s'\n", outpath); return 2; }
    fwrite(g_out, 1, g_out_len, f);
    fclose(f);
    return 0;
}
