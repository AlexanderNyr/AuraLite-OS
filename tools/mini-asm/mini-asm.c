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
/* relational / equality (for %if).  Shifts (<< >>) are handled deeper in
 * parse_mul, so a lone < or > here is a comparison. */
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

static long long parse_expr(Expr *e) { return parse_rel(e); }

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
    o->aregs = 0; o->has_disp = 0; o->disp[0] = 0;

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
     * the absolute form, so both are accepted and the keyword is dropped. */
    if ((!strncmp(s, "abs", 3) || !strncmp(s, "rel", 3)) &&
        (s[3] == ' ' || s[3] == '\t')) {
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

        /* is the term "reg" or "reg*scale"? */
        char *star = strchr(term, '*');
        char regname[64]; int scale = 1; int isreg = 0; int r = -1;
        if (star) {
            size_t rl = (size_t)(star - term);
            while (rl && (term[rl-1]==' '||term[rl-1]=='\t')) rl--;
            if (rl < sizeof regname) {
                memcpy(regname, term, rl); regname[rl] = 0;
                char *sc = star + 1; while (*sc==' '||*sc=='\t') sc++;
                scale = (int)eval_str(sc);
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
            else isreg = 0;
            if (isreg) {
                if (o->aregs == 0) o->aregs = w;
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
    o->scale = 1; o->text[0] = 0;
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
            int is_far = 0;
            for (const char *q = L->ops[0]; *q; q++) if (*q == ':') { is_far = 1; break; }
            if (!disp_only && is_reg) {
                L->is_jump = 0;   /* indirect jmp/call reg: fixed size, encode_instr handles it */
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
static void preprocess(const char *path, int depth) {
    if (depth > 32) die("%%include nested too deep at '%s'", path);
    char *buf = read_whole(path);
    int line_no = 0;
    int taking[64], taken[64], parent[64], sp = 0;
    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        line_no++;
        g_line_no = line_no;
        char *s = p;
        while (*s == ' ' || *s == '\t') s++;
        int cur = (sp == 0) ? 1 : taking[sp - 1];

        if (!strncmp(s, "%include", 8) && (s[8]==' '||s[8]=='\t'||s[8]=='"')) {
            if (cur) {
                char *q = s + 8; while (*q==' '||*q=='\t') q++;
                if (*q != '"' && *q != '\'') die("malformed %%include");
                char qc = *q++; char *st = q;
                while (*q && *q != qc) q++;
                if (*q != qc) die("unterminated %%include name");
                *q = 0;
                char *full = find_include(st);
                if (!full) die("cannot open include file '%s'", st);
                preprocess(full, depth + 1);
                free(full);
            }
        } else if (!strncmp(s, "%if", 3) && (s[3]==' '||s[3]=='\t')) {
            parent[sp] = cur;
            int cond = 0;
            if (cur) { char *e = s + 3; while (*e==' '||*e=='\t') e++; cond = eval_str(e) != 0; }
            taking[sp] = cur && cond;
            taken[sp]  = cond;
            sp++;
        } else if (!strncmp(s, "%elif", 5)) {
            die("%%elif not supported (SH4c subset)");
        } else if (!strncmp(s, "%else", 5)) {
            if (sp > 0) {
                taking[sp-1] = parent[sp-1] && !taken[sp-1];
                if (taking[sp-1]) taken[sp-1] = 1;
            }
        } else if (!strncmp(s, "%endif", 6)) {
            if (sp > 0) sp--;
        } else if (!strncmp(s, "%error", 6)) {
            if (cur) { char *q = s + 6; while (*q==' '||*q=='\t') q++; die("%%error: %s", q); }
        } else if (!strncmp(s, "%define", 7) || !strncmp(s, "%assign", 7)) {
            if (cur) {
                char *q = s + 7; while (*q==' '||*q=='\t') q++;
                char *nm = q;
                while (*q && *q!=' ' && *q!='\t' && *q!='(') q++;
                if (*q == '(') die("function-like %%define not supported (SH4c subset)");
                int had = (*q != 0);
                if (had) { *q = 0; q++; while (*q==' '||*q=='\t') q++; }
                /* object-like numeric define: store as a symbol (the tree's
                 * only %defines are boot_offsets.inc's integer constants) */
                Sym *sym = sym_intern(nm);
                sym->val = had ? eval_str(q) : 1;   /* bare %define FOO -> 1 */
                sym->defined = 1;
            }
        } else {
            if (cur) pline_add(p, line_no);
        }
        if (!nl) break;
        p = nl + 1;
    }
    if (sp != 0) die("unterminated %%if in '%s'", path);
    free(buf);
}

static void load_file(const char *path) {
    preprocess(path, 0);
    g_cur_global = "";
    for (int i = 0; i < g_nplines; i++)
        load_line(g_plines[i].text, g_plines[i].line_no);
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

/* Emit the ModRM (+SIB +disp) bytes for a memory operand `m` with register
 * field `rf`.  Returns the byte count (ModRM + SIB + disp).  The segment
 * override prefix is emitted by the caller.  Handles 16-bit (special rm)
 * and 32-bit (SIB) effective addresses and nasm's shortest-disp choice. */
static int emit_mem(int rf, Operand *m, int emit) {
    long long disp = m->has_disp ? eval_str(m->disp) : 0;
    int base = m->base, index = m->index;

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
        /* absolute [disp].  In 64-bit mode mod00/rm101 is RIP-relative, so
         * nasm uses SIB-with-no-base for an absolute address; in 32-bit it
         * is the plain mod00/rm101 disp32 form. */
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
            out_le((unsigned long long)disp, 4);
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
    int sib_base = (base < 0) ? 5 : base;
    int sib_index = (index < 0) ? 4 : index;
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
    a.scale = b.scale = 1;
    if (L->nops >= 1) parse_operand(L->ops[0], &a);
    if (L->nops >= 2) parse_operand(L->ops[1], &b);

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
            {"ud2",0x0B},{"rdmsr",0x32},{"wrmsr",0x30},{"cpuid",0xA2},{NULL,0}
        };
        for (int i = 0; z2[i].n; i++) if (!strcmp(m, z2[i].n)) {
            if (emit) { out_byte(0x0F); out_byte((uint8_t)z2[i].op); }
            return 2;
        }
        if (!strcmp(m, "pusha") || !strcmp(m, "pushad") ||
            !strcmp(m, "popa")  || !strcmp(m, "popad")) {
            int ispush = (m[1] == 'u');
            int is32 = (m[strlen(m)-1] == 'd');          /* pushad/popad */
            int implied = is32 ? 32 : 16;
            int defw = (g_bits == 16) ? 16 : 32;
            int p66 = (implied != defw);
            if (emit) { if (p66) out_byte(0x66); out_byte((uint8_t)(ispush ? 0x60 : 0x61)); }
            return (p66?1:0) + 1;
        }
        if (!strcmp(m, "pushf") || !strcmp(m, "pushfd") ||
            !strcmp(m, "popf")  || !strcmp(m, "popfd")) {
            int ispush = (m[1] == 'u');
            int is32 = (m[strlen(m)-1] == 'd');          /* pushfd/popfd */
            int implied = is32 ? 32 : 16;
            int defw = (g_bits == 16) ? 16 : 32;
            int p66 = (implied != defw);
            if (emit) { if (p66) out_byte(0x66); out_byte((uint8_t)(ispush ? 0x9C : 0x9D)); }
            return (p66?1:0) + 1;
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
        if (a.kind == OP_FAR) {                 /* far jmp seg:off -> EA */
            char buf[256]; snprintf(buf, sizeof buf, "%s", a.text);
            char *colon = strchr(buf, ':'); *colon = 0;
            long long seg = eval_str(buf), off = eval_str(colon + 1);
            int offw = (a.memsize == 32) ? 4 : (a.memsize == 16) ? 2 : (g_bits == 16 ? 2 : 4);
            int defw = (g_bits == 16) ? 2 : 4;
            int p66 = (offw != defw);
            if (emit) {
                if (p66) out_byte(0x66);
                out_byte(0xEA);
                out_le((unsigned long long)off, offw);
                out_le((unsigned long long)seg, 2);
            }
            return (p66?1:0) + 1 + offw + 2;
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
            int w = a.width;
            /* nasm: mov r64, imm that fits in unsigned 32 bits uses the 32-bit
             * form (B8+r imm32, no REX.W) -- the 32-bit write zero-extends. */
            if (w == 64 && v >= 0 && v <= 0xFFFFFFFFLL) w = 32;
            int p66 = need_66(w);
            int rex = (g_bits == 64 && (w == 64 || a.reg >= 8))
                      ? (0x40 | (w == 64 ? 8 : 0) | (a.reg >= 8 ? 1 : 0)) : 0;
            int immw = (w == 8) ? 1 : (w == 16) ? 2 : (w == 32) ? 4 : 8;
            if (emit) {
                if (p66) out_byte(0x66);
                if (rex) out_byte((uint8_t)rex);
                out_byte((uint8_t)((w == 8 ? 0xB0 : 0xB8) + (a.reg & 7)));
                out_le((unsigned long long)v, immw);
            }
            return 1 + (p66?1:0) + (rex?1:0) + immw;
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
            long long v = eval_str(b.text);
            int w = a.memsize ? a.memsize : (g_bits == 16 ? 16 : 32);
            int p66 = need_66(w);
            int segpre = a.seg ? 1 : 0;
            int a67 = need_67(a.aregs);
            int memsz = emit_mem(0, &a, 0);
            int immw = (w == 8) ? 1 : (w == 16 ? 2 : 4);
            if (emit) {
                if (a.seg) out_byte((uint8_t)a.seg);
                if (p66) out_byte(0x66);
                if (a67) out_byte(0x67);
                out_byte((uint8_t)(w == 8 ? 0xC6 : 0xC7));
                emit_mem(0, &a, 1);
                out_le((unsigned long long)v, immw);
            }
            return segpre + (p66?1:0) + (a67?1:0) + 1 + memsz + immw;
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
                if (v >= -128 && v <= 127) {   /* 83 /digit imm8 sign-extended */
                    if (emit) {
                        if (segpre) out_byte((uint8_t)dst->seg);
                        if (p66) out_byte(0x66);
                        if (a67) out_byte(0x67);
                        if (rex) out_byte((uint8_t)rex);
                        out_byte(0x83);
                        if (ismem) emit_mem(digit, dst, 1); else out_byte((uint8_t)modrm(3, digit, dst->reg));
                        out_byte((uint8_t)v);
                    }
                    return segpre + (p66?1:0) + (a67?1:0) + (rex?1:0) + 1 + memsz + (ismem?0:1) + 1;
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
        die("unsupported '%s' form (SH4c: reg/sreg only)", m);
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
            int p66 = (st[i].sz != defw);
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
                if (un[i].op0f) {   /* inc/dec reg: 0x40+r / 0x48+r (or FE/FF for 8-bit) */
                    if (w == 8) {
                        if (emit) { out_byte((uint8_t)(un[i].digit?0xFE:0xFE));
                                    out_byte((uint8_t)modrm(3, un[i].digit, a.reg)); }
                        return 2;
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
                int bycl = (b.kind == OP_REG && b.width == 8 && b.reg == 1);  /* cl */
                if (emit) {
                    if (p66) out_byte(0x66);
                    out_byte((uint8_t)(w==8?0xC0:0xC1));
                    out_byte((uint8_t)modrm(3, sh[i].digit, a.reg));
                    if (!bycl) { long long v = eval_str(b.text); out_byte((uint8_t)v); }
                }
                return (p66?1:0) + 2 + (bycl?0:1);
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
    /* ELF symbol/section directives are no-ops in -f bin (no symbol table);
     * the label still gets its address from its own definition. */
    if (!strcmp(m, "global") || !strcmp(m, "extern") || !strcmp(m, "common") ||
        !strcmp(m, "section") || !strcmp(m, "segment") || !strcmp(m, "cpu") ||
        !strcmp(m, "default")) {
        return 0;
    }
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
        else if (!strcmp(argv[i], "-I") && i + 1 < argc) {
            if (g_nincdirs < 16) g_incdirs[g_nincdirs++] = argv[++i];
            else i++;
        }
        else if (!strncmp(argv[i], "-I", 2) && argv[i][2]) {
            if (g_nincdirs < 16) g_incdirs[g_nincdirs++] = argv[i] + 2;
        }
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
