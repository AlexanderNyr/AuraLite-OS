/* userspace/office/libaodoc/aocalc.c — the Sheet calculation engine.
 * See aocalc.h for the contract.  The implementation notes that matter:
 *
 *  - Cells are sparse: one open-addressing hash per sheet, keyed by
 *    (row, col).  Deletion leaves a tombstone; when tombstones crowd the
 *    table it is rebuilt (the old slots stay in the arena until reset —
 *    bounded, counted, and reported by ao_arena_used()).
 *  - A formula is parsed once, at edit time, into an arena AST.  The
 *    parse also collects the cell's dependencies (single refs and
 *    ranges, possibly cross-sheet), which is the whole dependency graph:
 *    an edit marks its cell dirty and a fixpoint scan dirties every
 *    formula that can see a dirty cell.
 *  - Evaluation is a memoising DFS over dirty cells.  A cell that is
 *    being computed and is reached again is a cycle: it and everything
 *    that waits on it read #CYCLE!, and nothing hangs.
 *  - Every node evaluation spends from the book's recalculation budget.
 *    A pathological graph (a 60 000-cell range under a chain of
 *    formulas, say) exhausts it and settles as #NUM! instead of
 *    freezing the machine.
 */

#include "aocalc.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ misc */

static int ao_isnan(double x) { return x != x; }
static char *ao_strdup_n(ao_arena_t *a, const char *s, size_t n) {
    char *p = ao_arena_alloc(a, n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}
static char *ao_strdup(ao_arena_t *a, const char *s) {
    return ao_strdup_n(a, s, strlen(s));
}
static int ci_cmp(const char *a, const char *b) {
    while (*a && *b) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d) return d;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
static int ci_ncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int d = tolower((unsigned char)a[i]) - tolower((unsigned char)b[i]);
        if (d) return d;
        if (!a[i]) return 0;
    }
    return 0;
}

/* ----------------------------------------------------------------- values */

ao_value_t ao_val_blank(void) {
    ao_value_t v; memset(&v, 0, sizeof v); v.kind = AO_VAL_BLANK; v.sheet = -1;
    return v;
}
ao_value_t ao_val_num(double n) {
    ao_value_t v = ao_val_blank(); v.kind = AO_VAL_NUM; v.num = n; return v;
}
ao_value_t ao_val_bool(int b) {
    ao_value_t v = ao_val_blank(); v.kind = AO_VAL_BOOL; v.num = b ? 1 : 0; return v;
}
ao_value_t ao_val_text(const char *s) {
    ao_value_t v = ao_val_blank(); v.kind = AO_VAL_TEXT; v.text = s; return v;
}
ao_value_t ao_val_err(ao_err_t e) {
    ao_value_t v = ao_val_blank(); v.kind = AO_VAL_ERR; v.err = (uint8_t)e; return v;
}

const char *ao_err_str(ao_err_t e) {
    switch (e) {
        case AO_ERR_DIV0:  return "#DIV/0!";
        case AO_ERR_VALUE: return "#VALUE!";
        case AO_ERR_REF:   return "#REF!";
        case AO_ERR_NAME:  return "#NAME?";
        case AO_ERR_NUM:   return "#NUM!";
        case AO_ERR_CYCLE: return "#CYCLE!";
        case AO_ERR_NA:    return "#N/A";
        default:           return "#ERR";
    }
}

/* General-format number text: integral values print whole, everything
 * else gets up to 12 significant digits (0.1+0.2 prints "0.3", as it
 * should).  Written out by hand: the guest libc's printf has no %g, and
 * the engine must print the same digits in the guest as on the host. */
static void num_general(double n, char *buf, size_t sz) {
    if (n == 0) { snprintf(buf, sz, "0"); return; }
    if (n == floor(n) && fabs(n) < 1e15) { snprintf(buf, sz, "%.0f", n); return; }
    double a = fabs(n);
    if (a >= 1e15 || a < 1e-9) {
        /* Scientific, manually: d.ddddddddd e±X (12 significant digits). */
        int e = 0;
        double m = a;
        while (m >= 10) { m /= 10; e++; }
        while (m < 1)   { m *= 10; e--; }
        char core[64];
        snprintf(core, sizeof core, "%.9f", m);
        /* %.9f of 9.9999999996 rounds to 10.000000000: renormalise. */
        if (core[1] != '.') {
            snprintf(core, sizeof core, "%.9f", m / 10);
            e++;
        }
        char *dot = strchr(core, '.');
        if (dot) {
            char *end = core + strlen(core);
            while (end > dot + 1 && end[-1] == '0') --end;
            if (end == dot + 1) end = dot;
            *end = 0;
        }
        snprintf(buf, sz, "%s%se%+d", n < 0 ? "-" : "", core, e);
        return;
    }
    char core[64];
    snprintf(core, sizeof core, "%.10f", n);
    char *dot = strchr(core, '.');
    if (dot) {
        char *end = core + strlen(core);
        while (end > dot + 1 && end[-1] == '0') --end;
        if (end == dot + 1) end = dot;
        *end = 0;
    }
    snprintf(buf, sz, "%s", core);
}

int ao_val_to_num(ao_value_t v, double *out) {
    switch (v.kind) {
        case AO_VAL_BLANK: *out = 0; return 0;
        case AO_VAL_NUM:   *out = v.num; return 0;
        case AO_VAL_BOOL:  *out = v.num ? 1 : 0; return 0;
        case AO_VAL_TEXT: {
            if (!v.text) { *out = 0; return AO_ERR_VALUE; }
            char tmp[64];
            size_t k = 0;
            for (const char *p = v.text; *p && k < sizeof tmp - 1; p++)
                if (*p != ',') tmp[k++] = *p;
            tmp[k] = 0;
            char *e = NULL;
            double d = strtod(tmp, &e);
            if (e == tmp) return AO_ERR_VALUE;
            while (*e == ' ') e++;
            if (*e == '%') { d /= 100.0; e++; }
            if (*e) return AO_ERR_VALUE;
            *out = d;
            return 0;
        }
        case AO_VAL_ERR: return v.err ? v.err : AO_ERR_VALUE;
        default: return AO_ERR_VALUE;
    }
}

const char *ao_val_to_text(ao_book_t *b, ao_value_t v, char *buf, size_t n) {
    (void)b;
    if (!buf || n == 0) return "";
    switch (v.kind) {
        case AO_VAL_BLANK: buf[0] = 0; break;
        case AO_VAL_NUM:   num_general(v.num, buf, n); break;
        case AO_VAL_BOOL:  snprintf(buf, n, "%s", v.num ? "TRUE" : "FALSE"); break;
        case AO_VAL_TEXT:  snprintf(buf, n, "%s", v.text ? v.text : ""); break;
        case AO_VAL_ERR:   snprintf(buf, n, "%s", ao_err_str((ao_err_t)v.err)); break;
        default:           snprintf(buf, n, "%s", ao_err_str(AO_ERR_VALUE)); break;
    }
    return buf;
}

/* Spreadsheet comparison: blank adapts to the other side's type; then
 * number < text < boolean, text case-insensitive. */
static int ao_cmp(ao_value_t a, ao_value_t b) {
    if (a.kind == AO_VAL_BLANK && b.kind == AO_VAL_BLANK) return 0;
    if (a.kind == AO_VAL_BLANK) a = (b.kind == AO_VAL_TEXT) ? ao_val_text("") : ao_val_num(0);
    if (b.kind == AO_VAL_BLANK) b = (a.kind == AO_VAL_TEXT) ? ao_val_text("") : ao_val_num(0);
    int ra = a.kind == AO_VAL_NUM ? 0 : a.kind == AO_VAL_TEXT ? 1 : 2;
    int rb = b.kind == AO_VAL_NUM ? 0 : b.kind == AO_VAL_TEXT ? 1 : 2;
    if (ra != rb) return ra < rb ? -1 : 1;
    if (a.kind == AO_VAL_NUM || a.kind == AO_VAL_BOOL)
        return a.num < b.num ? -1 : a.num > b.num ? 1 : 0;
    return ci_cmp(a.text ? a.text : "", b.text ? b.text : "");
}

/* -------------------------------------------------------------------- refs */

int ao_ref_parse(const char *s, uint32_t *row, uint32_t *col,
                 int *abs_row, int *abs_col) {
    if (!s) return -1;
    const char *p = s;
    int ac = 0, ar = 0;
    if (*p == '$') { ac = 1; p++; }
    if (!isalpha((unsigned char)*p)) return -1;
    uint32_t c = 0;
    while (isalpha((unsigned char)*p)) {
        c = c * 26 + (uint32_t)(toupper((unsigned char)*p) - 'A' + 1);
        if (c > AO_SHEET_MAX_COLS) return -1;
        p++;
    }
    if (*p == '$') { ar = 1; p++; }
    if (!isdigit((unsigned char)*p)) return -1;
    uint32_t r = 0;
    while (isdigit((unsigned char)*p)) {
        r = r * 10 + (uint32_t)(*p - '0');
        if (r > AO_SHEET_MAX_ROWS) return -1;
        p++;
    }
    if (*p || c == 0 || r == 0) return -1;
    *col = c - 1;
    *row = r - 1;
    if (abs_col) *abs_col = ac;
    if (abs_row) *abs_row = ar;
    return 0;
}

void ao_ref_format(uint32_t row, uint32_t col, int abs_row, int abs_col,
                   char *buf, size_t n) {
    char tmp[32];
    int k = 0;
    char rev[8];
    int rc = 0;
    uint32_t c = col + 1;
    while (c) { rev[rc++] = (char)('A' + (c - 1) % 26); c = (c - 1) / 26; }
    if (abs_col) tmp[k++] = '$';
    while (rc) tmp[k++] = rev[--rc];
    if (abs_row) tmp[k++] = '$';
    k += snprintf(tmp + k, sizeof tmp - (size_t)k, "%u", row + 1);
    tmp[k] = 0;
    snprintf(buf, n, "%s", tmp);
}
void ao_ref_name(uint32_t row, uint32_t col, char *buf, size_t n) {
    ao_ref_format(row, col, 0, 0, buf, n);
}

/* -------------------------------------------------------------- structures */

typedef struct {
    int32_t  sheet;
    uint32_t row, col;
} ao_dep_t;
typedef struct {
    int32_t  sheet;
    uint32_t r1, c1, r2, c2;
} ao_rdep_t;

enum { AO_CELL_EMPTY = 0, AO_CELL_LITERAL = 1, AO_CELL_FORMULA = 2 };
enum { AO_ST_CLEAN = 0, AO_ST_DIRTY = 1, AO_ST_BUSY = 2 };

struct ao_cell {
    uint32_t row, col;
    uint8_t  content;        /* AO_CELL_* */
    uint8_t  state;          /* AO_ST_* */
    const char *input;       /* as typed (arena) */
    ao_value_t literal;      /* content == LITERAL */
    struct ao_node *ast;     /* content == FORMULA (NULL: parse failed) */
    ao_dep_t  *deps;   uint32_t ndeps;
    ao_rdep_t *rdeps;  uint32_t nrdeps;
    ao_value_t value;        /* cached computed value */
};

struct ao_sheet {
    char *name;
    ao_book_t *book;
    ao_cell_t **slots;
    uint32_t slot_cap, count, tombs;
    ao_cell_t **sorted;      /* row/col-order cache */
    int sorted_valid;
};

typedef struct { ao_cell_t *cell; int32_t sheet; } ao_fentry_t;

struct ao_book {
    ao_arena_t *arena;
    ao_fentry_t *fcells;     /* every cell that has been a formula */
    uint32_t nfcells, fcell_cap;
    /* Reused invalidate() scratch: the dirty frontier.  Allocating this
     * from the arena per edit leaked kilobytes per keystroke. */
    struct { int32_t sheet; uint32_t row, col; } *dirty;
    uint32_t dirty_cap;
    ao_sheet_t *sheets[AO_BOOK_MAX_SHEETS];
    int nsheets;
    long budget, steps;
    int budget_blown;
    double now_serial;       /* 0: wall clock */
    uint32_t formula_cells;  /* edits skip the dirty scan while this is 0 */
};

/* ---- forward declarations ---- */
typedef struct ao_node ao_node_t;
static ao_value_t eval_cell(ao_book_t *b, int sheet, uint32_t row, uint32_t col);
static ao_value_t eval_node(ao_book_t *b, int ctx_sheet, ao_node_t *nd);
static ao_node_t *parse_formula(ao_book_t *b, int ctx_sheet, const char *text,
                                ao_dep_t **deps, uint32_t *ndeps,
                                ao_rdep_t **rdeps, uint32_t *nrdeps);

/* ------------------------------------------------------------------- arena */

static void *balloc(ao_book_t *b, size_t n) { return ao_arena_alloc(b->arena, n); }

/* -------------------------------------------------------------------- book */

ao_book_t *ao_book_new(ao_arena_t *arena) {
    if (!arena) return NULL;
    ao_book_t *b = ao_arena_calloc(arena, 1, sizeof *b);
    if (!b) return NULL;
    b->arena = arena;
    b->budget = AO_RECALC_BUDGET;
    return b;
}

static int sheet_index(ao_book_t *b, const char *name) {
    for (int i = 0; i < b->nsheets; i++)
        if (ci_cmp(b->sheets[i]->name, name) == 0) return i;
    return -1;
}

ao_sheet_t *ao_book_sheet(ao_book_t *b, const char *name, int create) {
    if (!b || !name || !*name) return NULL;
    int i = sheet_index(b, name);
    if (i >= 0) return b->sheets[i];
    if (!create || b->nsheets >= AO_BOOK_MAX_SHEETS) return NULL;
    ao_sheet_t *s = ao_arena_calloc(b->arena, 1, sizeof *s);
    if (!s) return NULL;
    s->name = ao_strdup(b->arena, name);
    if (!s->name) return NULL;
    s->book = b;
    s->slot_cap = 64;
    s->slots = ao_arena_calloc(b->arena, s->slot_cap, sizeof(ao_cell_t *));
    if (!s->slots) return NULL;
    b->sheets[b->nsheets++] = s;
    return s;
}
ao_sheet_t *ao_book_sheet_at(ao_book_t *b, int idx) {
    if (!b || idx < 0 || idx >= b->nsheets) return NULL;
    return b->sheets[idx];
}
int ao_book_sheet_count(const ao_book_t *b) { return b ? b->nsheets : 0; }
const char *ao_sheet_name(const ao_sheet_t *s) { return s ? s->name : ""; }
uint32_t ao_sheet_cell_count(const ao_sheet_t *s) { return s ? s->count : 0; }
void ao_cell_pos(const ao_cell_t *c, uint32_t *row, uint32_t *col) {
    if (row) *row = c ? c->row : 0;
    if (col) *col = c ? c->col : 0;
}
const char *ao_cell_input(const ao_cell_t *c) { return c ? c->input : NULL; }

/* -------------------------------------------------------------------- hash */

#define AO_TOMB ((ao_cell_t *)(uintptr_t)1)

static uint32_t cell_hash(uint32_t row, uint32_t col) {
    uint32_t h = row * 2654435761u ^ col * 40503u ^ (row >> 3);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

static ao_cell_t **slot_for(ao_sheet_t *s, uint32_t row, uint32_t col, int for_insert) {
    uint32_t mask = s->slot_cap - 1;
    uint32_t i = cell_hash(row, col) & mask;
    ao_cell_t **tomb = NULL;
    for (;;) {
        ao_cell_t **sl = &s->slots[i];
        if (*sl == NULL) return for_insert && tomb ? tomb : sl;
        if (*sl == AO_TOMB) { if (!tomb) tomb = sl; }
        else if ((*sl)->row == row && (*sl)->col == col) return sl;
        i = (i + 1) & mask;
    }
}

static int sheet_grow(ao_sheet_t *s) {
    ao_book_t *b = s->book;
    uint32_t ncap = s->slot_cap * 2;
    ao_cell_t **ns = ao_arena_calloc(b->arena, ncap, sizeof(ao_cell_t *));
    if (!ns) return -1;
    ao_cell_t **old = s->slots;
    uint32_t ocap = s->slot_cap;
    s->slots = ns;
    s->slot_cap = ncap;
    s->tombs = 0;
    for (uint32_t i = 0; i < ocap; i++) {
        ao_cell_t *c = old[i];
        if (c && c != AO_TOMB) *slot_for(s, c->row, c->col, 1) = c;
    }
    return 0;
}

ao_cell_t *ao_sheet_cell(ao_book_t *b, ao_sheet_t *s, uint32_t row, uint32_t col) {
    if (!b || !s) return NULL;
    ao_cell_t *c = *slot_for(s, row, col, 0);
    return c == AO_TOMB ? NULL : c;
}

/* --------------------------------------------------------- dirty machinery */

/* Mark (sheet,row,col) and everything that transitively depends on it
 * dirty.  The fixpoint scan is O(cells x passes); the passes are bounded
 * by the longest dependency chain, and the cell cap bounds the cells. */
static void invalidate(ao_book_t *b, int sh, uint32_t row, uint32_t col) {
    ao_sheet_t *t = ao_book_sheet_at(b, sh);
    if (t) {
        ao_cell_t *c = ao_sheet_cell(b, t, row, col);
        if (c) c->state = AO_ST_DIRTY;
    }
    if (!b->nfcells) return;
    /* The dirty frontier, as coordinates (a deleted cell has no struct
     * left in the map, but its coordinates still dirty its dependents).
     * Kept in a book-owned buffer reused by every edit. */
    typedef struct { int32_t sheet; uint32_t row, col; } coord_t;
    uint32_t cap = b->dirty_cap, nd = 0;
    coord_t *dirty = (coord_t *)b->dirty;
    if (!dirty) {
        cap = 256;
        dirty = balloc(b, cap * sizeof *dirty);
        if (!dirty) return;
        b->dirty = (void *)dirty;
        b->dirty_cap = cap;
    }
    dirty[nd++] = (coord_t){ sh, row, col };
    int changed = 1;
    while (changed) {
        changed = 0;
        for (uint32_t i = 0; i < b->nfcells; i++) {
            ao_cell_t *c = b->fcells[i].cell;
            if (c->content != AO_CELL_FORMULA || c->state == AO_ST_DIRTY)
                continue;
            int hit = 0;
            for (uint32_t d = 0; d < c->ndeps && !hit; d++)
                for (uint32_t k = 0; k < nd; k++)
                    if (dirty[k].sheet == c->deps[d].sheet &&
                        dirty[k].row == c->deps[d].row &&
                        dirty[k].col == c->deps[d].col) { hit = 1; break; }
            for (uint32_t d = 0; d < c->nrdeps && !hit; d++) {
                ao_rdep_t *r = &c->rdeps[d];
                for (uint32_t k = 0; k < nd; k++)
                    if (dirty[k].sheet == r->sheet &&
                        dirty[k].row >= r->r1 && dirty[k].row <= r->r2 &&
                        dirty[k].col >= r->c1 && dirty[k].col <= r->c2) { hit = 1; break; }
            }
            if (hit) {
                c->state = AO_ST_DIRTY;
                changed = 1;
                if (nd == cap) {
                    cap *= 2;
                    coord_t *np = balloc(b, cap * sizeof *np);
                    if (!np) return;
                    memcpy(np, dirty, nd * sizeof *np);
                    dirty = np;
                    b->dirty = (void *)dirty;
                    b->dirty_cap = cap;
                }
                dirty[nd++] = (coord_t){ b->fcells[i].sheet, c->row, c->col };
            }
        }
    }
}

/* --------------------------------------------------------------- mutation */

int ao_sheet_set(ao_book_t *b, ao_sheet_t *s, uint32_t row, uint32_t col,
                 const char *input) {
    if (!b || !s || row >= AO_SHEET_MAX_ROWS || col >= AO_SHEET_MAX_COLS)
        return AO_CALC_ERR_ARGS;
    int si = sheet_index(b, s->name);
    if (!input) input = "";
    /* Trim surrounding whitespace for the emptiness test. */
    const char *p = input;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 0) {   /* clear */
        ao_cell_t **sl = slot_for(s, row, col, 0);
        if (*sl && *sl != AO_TOMB) {
            if ((*sl)->content == AO_CELL_FORMULA && b->formula_cells)
                b->formula_cells--;
            (*sl)->content = AO_CELL_EMPTY;
            *sl = AO_TOMB;
            s->count--;
            s->tombs++;
            s->sorted_valid = 0;
            invalidate(b, si, row, col);
        }
        return AO_CALC_OK;
    }
    if ((s->count + s->tombs + 1) * 10 > s->slot_cap * 7) {
        if (sheet_grow(s) != 0) return AO_CALC_ERR_NOMEM;
    }
    ao_cell_t *c = ao_sheet_cell(b, s, row, col);
    if (!c) {
        if (s->count >= AO_SHEET_MAX_CELLS) return AO_CALC_ERR_CAP;
        c = ao_arena_calloc(b->arena, 1, sizeof *c);
        if (!c) return AO_CALC_ERR_NOMEM;
        c->row = row;
        c->col = col;
        *slot_for(s, row, col, 1) = c;
        s->count++;
        s->sorted_valid = 0;
    }
    if (c->content == AO_CELL_FORMULA && b->formula_cells) b->formula_cells--;
    c->input = ao_strdup(b->arena, input);
    if (!c->input) return AO_CALC_ERR_NOMEM;
    c->ast = NULL;
    c->deps = NULL; c->ndeps = 0;
    c->rdeps = NULL; c->nrdeps = 0;
    c->state = AO_ST_DIRTY;
    if (input[0] == '=') {
        if (c->content != AO_CELL_FORMULA) {
            if (b->nfcells == b->fcell_cap) {
                uint32_t nc = b->fcell_cap ? b->fcell_cap * 2 : 64;
                ao_fentry_t *np = balloc(b, nc * sizeof *np);
                if (!np) return AO_CALC_ERR_NOMEM;
                if (b->fcells) memcpy(np, b->fcells, b->nfcells * sizeof *np);
                b->fcells = np; b->fcell_cap = nc;
            }
            b->fcells[b->nfcells].cell = c;
            b->fcells[b->nfcells].sheet = si;
            b->nfcells++;
        }
        c->content = AO_CELL_FORMULA;
        b->formula_cells++;
        c->ast = parse_formula(b, si, input + 1, &c->deps, &c->ndeps,
                               &c->rdeps, &c->nrdeps);
        c->value = ao_val_err(AO_ERR_VALUE);   /* until evaluated */
    } else {
        c->content = AO_CELL_LITERAL;
        /* A double as the whole (trimmed) input?  TRUE/FALSE?  Else text. */
        char *e = NULL;
        double d = strtod(p, &e);
        if (e != p && e && *e == 0) {
            c->literal = ao_val_num(d);
        } else if (ci_cmp(p, "TRUE") == 0) {
            c->literal = ao_val_bool(1);
        } else if (ci_cmp(p, "FALSE") == 0) {
            c->literal = ao_val_bool(0);
        } else {
            const char *txt = input;
            /* One layer of quotes in a file means literal text. */
            size_t L = strlen(txt);
            if (L >= 2 && txt[0] == '"' && txt[L - 1] == '"') {
                char *u = balloc(b, L - 1);
                if (!u) return AO_CALC_ERR_NOMEM;
                size_t k = 0;
                for (size_t i = 1; i + 1 < L; i++) {
                    if (txt[i] == '"' && txt[i + 1] == '"') i++;
                    u[k++] = txt[i];
                }
                u[k] = 0;
                txt = u;
            } else {
                txt = ao_strdup(b->arena, txt);
                if (!txt) return AO_CALC_ERR_NOMEM;
            }
            c->literal = ao_val_text(txt);
        }
        c->value = c->literal;
    }
    invalidate(b, si, row, col);
    c->state = AO_ST_DIRTY;   /* invalidate() may have cleaned via scan order */
    return AO_CALC_OK;
}

/* Sorted iteration cache. */
static int cell_ptr_cmp(const void *a, const void *c2) {
    const ao_cell_t *x = *(ao_cell_t * const *)a;
    const ao_cell_t *y = *(ao_cell_t * const *)c2;
    if (x->row != y->row) return x->row < y->row ? -1 : 1;
    return x->col < y->col ? -1 : x->col > y->col ? 1 : 0;
}
ao_cell_t *ao_sheet_cell_sorted(ao_book_t *b, ao_sheet_t *s, uint32_t idx) {
    if (!b || !s || idx >= s->count) return NULL;
    if (!s->sorted_valid) {
        s->sorted = balloc(b, (s->count ? s->count : 1) * sizeof(ao_cell_t *));
        if (!s->sorted) return NULL;
        uint32_t k = 0;
        for (uint32_t i = 0; i < s->slot_cap; i++) {
            ao_cell_t *c = s->slots[i];
            if (c && c != AO_TOMB) s->sorted[k++] = c;
        }
        qsort(s->sorted, s->count, sizeof(ao_cell_t *), cell_ptr_cmp);
        s->sorted_valid = 1;
    }
    return s->sorted[idx];
}

int ao_book_sheet_copy(ao_book_t *b, const char *from, const char *to) {
    if (!b) return AO_CALC_ERR_ARGS;
    ao_sheet_t *src = ao_book_sheet(b, from, 0);
    if (!src) return AO_CALC_ERR_NOTFOUND;
    if (sheet_index(b, to) >= 0) return AO_CALC_ERR_ARGS;
    ao_sheet_t *dst = ao_book_sheet(b, to, 1);
    if (!dst) return AO_CALC_ERR_NOMEM;
    for (uint32_t i = 0; i < src->count; i++) {
        ao_cell_t *c = ao_sheet_cell_sorted(b, src, i);
        int rc = ao_sheet_set(b, dst, c->row, c->col, c->input);
        if (rc != AO_CALC_OK) return rc;
    }
    return AO_CALC_OK;
}

int ao_book_sheet_rename(ao_book_t *b, const char *from, const char *to) {
    if (!b || !to || !*to) return AO_CALC_ERR_ARGS;
    ao_sheet_t *s = ao_book_sheet(b, from, 0);
    if (!s) return AO_CALC_ERR_NOTFOUND;
    if (sheet_index(b, to) >= 0) return AO_CALC_ERR_ARGS;
    char *nm = ao_strdup(b->arena, to);
    if (!nm) return AO_CALC_ERR_NOMEM;
    /* Formulas that named the old sheet keep the index (deps and ASTs
     * store sheet *indices*), so a rename needs no rewrite — the name is
     * display state.  New parses resolve the new name. */
    s->name = nm;
    return AO_CALC_OK;
}

void ao_book_set_budget(ao_book_t *b, long budget) { if (b) b->budget = budget; }
long ao_book_last_steps(const ao_book_t *b) { return b ? b->steps : 0; }
void ao_book_set_now(ao_book_t *b, double serial) { if (b) b->now_serial = serial; }

static double book_now(ao_book_t *b) {
    if (b->now_serial > 0) return b->now_serial;
    /* Excel serial 0 = 1899-12-30; the Unix epoch is serial 25568. */
    return (double)time(NULL) / 86400.0 + 25569.0;
}

/* ------------------------------------------------------------ formula AST */

enum {
    ND_NUM, ND_TEXT, ND_BOOL, ND_REF, ND_RANGE, ND_FUNC, ND_OP, ND_BAD
};
enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_POW, OP_CAT,
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_NEG, OP_PCT
};

struct ao_node {
    uint8_t kind, op;
    uint8_t qualified;        /* ND_REF: a Sheet! prefix was written */
    double num;
    const char *text;         /* ND_TEXT */
    int func;                 /* ND_FUNC: ao_fn id or -1 (unknown) */
    int32_t sheet;            /* ND_REF/ND_RANGE: -2 = unresolvable */
    uint32_t r1, c1, r2, c2;  /* ND_REF: r1/c1 */
    ao_node_t *a, *b;         /* ND_OP */
    ao_node_t **args;         /* ND_FUNC */
    int nargs;
};

typedef struct {
    ao_book_t *book;
    int ctx_sheet;
    const char *p;
    int failed;
    ao_dep_t *deps;  uint32_t ndeps, dep_capv;
    ao_rdep_t *rdeps; uint32_t nrdeps, rdep_capv;
} parser_t;

static ao_node_t *nd_new(parser_t *ps, int kind) {
    ao_node_t *nd = ao_arena_calloc(ps->book->arena, 1, sizeof *nd);
    if (!nd) { ps->failed = 1; return NULL; }
    nd->kind = (uint8_t)kind;
    nd->sheet = -1;
    return nd;
}
static void skip_ws(parser_t *ps) {
    while (*ps->p == ' ' || *ps->p == '\t') ps->p++;
}
static void dep_add(parser_t *ps, int sheet, uint32_t r, uint32_t c) {
    if (ps->ndeps == ps->dep_capv) {
        uint32_t nc = ps->dep_capv ? ps->dep_capv * 2 : 8;
        ao_dep_t *np = balloc(ps->book, nc * sizeof *np);
        if (!np) { ps->failed = 1; return; }
        if (ps->deps) memcpy(np, ps->deps, ps->ndeps * sizeof *np);
        ps->deps = np; ps->dep_capv = nc;
    }
    ps->deps[ps->ndeps].sheet = sheet;
    ps->deps[ps->ndeps].row = r;
    ps->deps[ps->ndeps].col = c;
    ps->ndeps++;
}
static void rdep_add(parser_t *ps, int sheet, uint32_t r1, uint32_t c1,
                     uint32_t r2, uint32_t c2) {
    if (ps->nrdeps == ps->rdep_capv) {
        uint32_t nc = ps->rdep_capv ? ps->rdep_capv * 2 : 4;
        ao_rdep_t *np = balloc(ps->book, nc * sizeof *np);
        if (!np) { ps->failed = 1; return; }
        if (ps->rdeps) memcpy(np, ps->rdeps, ps->nrdeps * sizeof *np);
        ps->rdeps = np; ps->rdep_capv = nc;
    }
    ao_rdep_t *d = &ps->rdeps[ps->nrdeps++];
    d->sheet = sheet; d->r1 = r1; d->c1 = c1; d->r2 = r2; d->c2 = c2;
}

/* The 49 functions of the plan, in one table so the parser and the
 * evaluator cannot disagree about what exists. */
enum {
    FN_SUM, FN_AVERAGE, FN_MIN, FN_MAX, FN_COUNT, FN_COUNTA, FN_COUNTIF,
    FN_SUMIF, FN_IF, FN_AND, FN_OR, FN_NOT, FN_ABS, FN_ROUND, FN_ROUNDUP,
    FN_ROUNDDOWN, FN_INT, FN_MOD, FN_POWER, FN_SQRT, FN_EXP, FN_LN, FN_LOG,
    FN_SIN, FN_COS, FN_TAN, FN_PI, FN_CONCATENATE, FN_LEFT, FN_RIGHT,
    FN_MID, FN_LEN, FN_UPPER, FN_LOWER, FN_TRIM, FN_VALUE, FN_TEXT,
    FN_TODAY, FN_NOW, FN_VLOOKUP, FN_HLOOKUP, FN_INDEX, FN_MATCH, FN_RANK,
    FN_MEDIAN, FN_STDEV, FN_PRODUCT, FN_AVERAGEIF, FN_SUMPRODUCT, FN_N
};
static const char *const FN_NAMES[FN_N] = {
    "SUM", "AVERAGE", "MIN", "MAX", "COUNT", "COUNTA", "COUNTIF",
    "SUMIF", "IF", "AND", "OR", "NOT", "ABS", "ROUND", "ROUNDUP",
    "ROUNDDOWN", "INT", "MOD", "POWER", "SQRT", "EXP", "LN", "LOG",
    "SIN", "COS", "TAN", "PI", "CONCATENATE", "LEFT", "RIGHT",
    "MID", "LEN", "UPPER", "LOWER", "TRIM", "VALUE", "TEXT",
    "TODAY", "NOW", "VLOOKUP", "HLOOKUP", "INDEX", "MATCH", "RANK",
    "MEDIAN", "STDEV", "PRODUCT", "AVERAGEIF", "SUMPRODUCT"
};
static int fn_lookup(const char *name, size_t len) {
    for (int i = 0; i < FN_N; i++)
        if (strlen(FN_NAMES[i]) == len && ci_ncmp(FN_NAMES[i], name, len) == 0)
            return i;
    return -1;
}

static ao_node_t *parse_expr(parser_t *ps);

/* A cell reference at ps->p, possibly sheet-qualified.  On success
 * consumes it and sets the ND_REF node fields; returns 0. */
static int try_ref(parser_t *ps, ao_node_t *nd) {
    const char *p = ps->p;
    int sheet = ps->ctx_sheet;
    /* Sheet prefix: name! or 'quoted name'! */
    const char *q = p;
    char sname[64];
    size_t sn = 0;
    if (*q == '\'') {
        q++;
        while (*q && *q != '\'' && sn < sizeof sname - 1) sname[sn++] = *q++;
        if (*q != '\'') return -1;
        q++;
    } else {
        while (isalnum((unsigned char)*q) || *q == '_' || *q == '.') {
            if (sn < sizeof sname - 1) sname[sn++] = *q;
            q++;
        }
    }
    if (sn > 0 && *q == '!') {
        sname[sn] = 0;
        ao_sheet_t *sh = ao_book_sheet(ps->book, sname, 0);
        sheet = sh ? sheet_index(ps->book, sname) : -2;
        nd->qualified = 1;
        p = q + 1;
    }
    uint32_t r, c;
    int ar, ac;
    /* Parse the ref text manually so we know where it ends. */
    const char *e = p;
    if (*e == '$') e++;
    if (!isalpha((unsigned char)*e)) return -1;
    while (isalpha((unsigned char)*e)) e++;
    if (*e == '$') e++;
    if (!isdigit((unsigned char)*e)) return -1;
    while (isdigit((unsigned char)*e)) e++;
    char rbuf[24];
    size_t rl = (size_t)(e - p);
    if (rl >= sizeof rbuf) return -1;
    memcpy(rbuf, p, rl);
    rbuf[rl] = 0;
    if (ao_ref_parse(rbuf, &r, &c, &ar, &ac) != 0) return -1;
    nd->sheet = sheet;
    nd->r1 = r; nd->c1 = c;
    ps->p = e;
    return 0;
}

static ao_node_t *parse_primary(parser_t *ps) {
    skip_ws(ps);
    char ch = *ps->p;
    if (ch == '(') {
        ps->p++;
        ao_node_t *in = parse_expr(ps);
        skip_ws(ps);
        if (*ps->p != ')') { ps->failed = 1; return nd_new(ps, ND_BAD); }
        ps->p++;
        return in;
    }
    if (ch == '"') {
        ps->p++;
        const char *start = ps->p;
        /* Find the closing quote ("" is an escaped quote). */
        const char *q = start;
        size_t len = 0;
        for (;;) {
            if (!*q) { ps->failed = 1; return nd_new(ps, ND_BAD); }
            if (*q == '"') {
                if (q[1] == '"') { len += 1; q += 2; continue; }
                break;
            }
            len++; q++;
        }
        char *txt = balloc(ps->book, len + 1);
        if (!txt) { ps->failed = 1; return nd_new(ps, ND_BAD); }
        size_t k = 0;
        for (const char *t = start; t < q; t++) {
            if (*t == '"' && t[1] == '"') t++;
            txt[k++] = *t;
        }
        txt[k] = 0;
        ps->p = q + 1;
        ao_node_t *nd = nd_new(ps, ND_TEXT);
        if (nd) nd->text = txt;
        return nd;
    }
    if (isdigit((unsigned char)ch) || (ch == '.' && isdigit((unsigned char)ps->p[1]))) {
        char *e = NULL;
        double d = strtod(ps->p, &e);
        ps->p = e;
        ao_node_t *nd = nd_new(ps, ND_NUM);
        if (nd) nd->num = d;
        return nd;
    }
    if (isalpha((unsigned char)ch) || ch == '$' || ch == '\'') {
        /* Function call?  IDENT '(' — but a ref like LOG10( is a name. */
        const char *save = ps->p;
        const char *q = ps->p;
        while (isalnum((unsigned char)*q) || *q == '_' || *q == '.') q++;
        if (*q == '(' && q > save) {
            int fn = fn_lookup(save, (size_t)(q - save));
            ps->p = q + 1;
            ao_node_t *nd = nd_new(ps, ND_FUNC);
            if (!nd) return NULL;
            nd->func = fn;
            ao_node_t **args = NULL;
            int na = 0, cap = 0;
            skip_ws(ps);
            if (*ps->p != ')') {
                for (;;) {
                    ao_node_t *a = parse_expr(ps);
                    if (na == cap) {
                        int nc = cap ? cap * 2 : 4;
                        ao_node_t **np = balloc(ps->book, (size_t)nc * sizeof *np);
                        if (!np) { ps->failed = 1; return nd_new(ps, ND_BAD); }
                        if (args) memcpy(np, args, (size_t)na * sizeof *np);
                        args = np; cap = nc;
                    }
                    args[na++] = a;
                    skip_ws(ps);
                    if (*ps->p == ';' || *ps->p == ',') { ps->p++; continue; }
                    break;
                }
            }
            skip_ws(ps);
            if (*ps->p != ')') { ps->failed = 1; return nd_new(ps, ND_BAD); }
            ps->p++;
            nd->args = args;
            nd->nargs = na;
            return nd;
        }
        /* TRUE / FALSE literals. */
        if ((ci_ncmp(save, "TRUE", 4) == 0 && !isalnum((unsigned char)save[4]))) {
            ps->p = save + 4;
            ao_node_t *nd = nd_new(ps, ND_BOOL);
            if (nd) nd->num = 1;
            return nd;
        }
        if ((ci_ncmp(save, "FALSE", 5) == 0 && !isalnum((unsigned char)save[5]))) {
            ps->p = save + 5;
            ao_node_t *nd = nd_new(ps, ND_BOOL);
            if (nd) nd->num = 0;
            return nd;
        }
        /* A reference (maybe a range). */
        ao_node_t *nd = nd_new(ps, ND_REF);
        if (!nd) return NULL;
        if (try_ref(ps, nd) != 0) {
            /* A bare unknown name: parses, evaluates to #NAME?. */
            ps->p = q;
            ao_node_t *bad = nd_new(ps, ND_BAD);
            return bad;
        }
        if (nd->sheet >= 0) dep_add(ps, nd->sheet, nd->r1, nd->c1);
        skip_ws(ps);
        if (*ps->p == ':') {
            ps->p++;
            ao_node_t *rhs = nd_new(ps, ND_REF);
            if (!rhs || try_ref(ps, rhs) != 0) { ps->failed = 1; return nd_new(ps, ND_BAD); }
            if (rhs->qualified && rhs->sheet != nd->sheet) {
                /* A 3-D range across sheets: not supported, reads #REF!. */
                nd->kind = ND_BAD;
                return nd;
            }
            uint32_t r1 = nd->r1 < rhs->r1 ? nd->r1 : rhs->r1;
            uint32_t r2 = nd->r1 < rhs->r1 ? rhs->r1 : nd->r1;
            uint32_t c1 = nd->c1 < rhs->c1 ? nd->c1 : rhs->c1;
            uint32_t c2 = nd->c1 < rhs->c1 ? rhs->c1 : nd->c1;
            if ((uint64_t)(r2 - r1 + 1) * (c2 - c1 + 1) > AO_RANGE_MAX_CELLS) {
                nd->kind = ND_BAD;   /* becomes #VALUE! at evaluation */
                return nd;
            }
            /* Replace the single-ref dep with the range dep. */
            if (ps->ndeps) ps->ndeps--;
            rdep_add(ps, nd->sheet, r1, c1, r2, c2);
            nd->kind = ND_RANGE;
            nd->r1 = r1; nd->c1 = c1; nd->r2 = r2; nd->c2 = c2;
            /* The range lives on the left ref's sheet: the right ref is
             * unqualified in every sane sheet (Sheet1!A1:B9), and letting
             * its default (the formula's own sheet) override the left
             * qualification pointed cross-sheet ranges at the wrong page. */
            return nd;
        }
        /* Single ref: fix the dep sheet if qualification resolved late. */
        return nd;
    }
    ps->failed = 1;
    return nd_new(ps, ND_BAD);
}

static ao_node_t *parse_postfix(parser_t *ps) {
    ao_node_t *nd = parse_primary(ps);
    for (;;) {
        skip_ws(ps);
        if (*ps->p != '%') return nd;
        ps->p++;
        ao_node_t *w = nd_new(ps, ND_OP);
        if (!w) return nd;
        w->op = OP_PCT;
        w->a = nd;
        nd = w;
    }
}
static ao_node_t *parse_unary(parser_t *ps) {
    skip_ws(ps);
    if (*ps->p == '-' || *ps->p == '+') {
        int neg = *ps->p == '-';
        ps->p++;
        ao_node_t *in = parse_unary(ps);
        if (!neg) return in;
        ao_node_t *w = nd_new(ps, ND_OP);
        if (!w) return in;
        w->op = OP_NEG;
        w->a = in;
        return w;
    }
    return parse_postfix(ps);
}
static ao_node_t *parse_pow(parser_t *ps) {
    ao_node_t *l = parse_unary(ps);
    skip_ws(ps);
    if (*ps->p == '^') {
        ps->p++;
        ao_node_t *r = parse_pow(ps);
        ao_node_t *w = nd_new(ps, ND_OP);
        if (!w) return l;
        w->op = OP_POW; w->a = l; w->b = r;
        return w;
    }
    return l;
}
static ao_node_t *parse_mul(parser_t *ps) {
    ao_node_t *l = parse_pow(ps);
    for (;;) {
        skip_ws(ps);
        char ch = *ps->p;
        if (ch != '*' && ch != '/') return l;
        ps->p++;
        ao_node_t *r = parse_pow(ps);
        ao_node_t *w = nd_new(ps, ND_OP);
        if (!w) return l;
        w->op = ch == '*' ? OP_MUL : OP_DIV;
        w->a = l; w->b = r;
        l = w;
    }
}
static ao_node_t *parse_add(parser_t *ps) {
    ao_node_t *l = parse_mul(ps);
    for (;;) {
        skip_ws(ps);
        char ch = *ps->p;
        if (ch != '+' && ch != '-') return l;
        ps->p++;
        ao_node_t *r = parse_mul(ps);
        ao_node_t *w = nd_new(ps, ND_OP);
        if (!w) return l;
        w->op = ch == '+' ? OP_ADD : OP_SUB;
        w->a = l; w->b = r;
        l = w;
    }
}
static ao_node_t *parse_concat(parser_t *ps) {
    ao_node_t *l = parse_add(ps);
    for (;;) {
        skip_ws(ps);
        if (*ps->p != '&') return l;
        ps->p++;
        ao_node_t *r = parse_add(ps);
        ao_node_t *w = nd_new(ps, ND_OP);
        if (!w) return l;
        w->op = OP_CAT; w->a = l; w->b = r;
        l = w;
    }
}
static ao_node_t *parse_expr(parser_t *ps) {
    ao_node_t *l = parse_concat(ps);
    for (;;) {
        skip_ws(ps);
        int op = -1;
        if (ps->p[0] == '<' && ps->p[1] == '=') { op = OP_LE; ps->p += 2; }
        else if (ps->p[0] == '>' && ps->p[1] == '=') { op = OP_GE; ps->p += 2; }
        else if (ps->p[0] == '<' && ps->p[1] == '>') { op = OP_NE; ps->p += 2; }
        else if (*ps->p == '=') { op = OP_EQ; ps->p++; }
        else if (*ps->p == '<') { op = OP_LT; ps->p++; }
        else if (*ps->p == '>') { op = OP_GT; ps->p++; }
        if (op < 0) return l;
        ao_node_t *r = parse_concat(ps);
        ao_node_t *w = nd_new(ps, ND_OP);
        if (!w) return l;
        w->op = (uint8_t)op; w->a = l; w->b = r;
        l = w;
    }
}

static ao_node_t *parse_formula(ao_book_t *b, int ctx_sheet, const char *text,
                                ao_dep_t **deps, uint32_t *ndeps,
                                ao_rdep_t **rdeps, uint32_t *nrdeps) {
    parser_t ps;
    memset(&ps, 0, sizeof ps);
    ps.book = b;
    ps.ctx_sheet = ctx_sheet;
    ps.p = text;
    ao_node_t *nd = parse_expr(&ps);
    skip_ws(&ps);
    if (*ps.p) ps.failed = 1;
    *deps = ps.deps; *ndeps = ps.ndeps;
    *rdeps = ps.rdeps; *nrdeps = ps.nrdeps;
    if (ps.failed || !nd) return NULL;
    return nd;
}

/* -------------------------------------------------------------- evaluation */

static int spend(ao_book_t *b) {
    if (++b->steps > b->budget) { b->budget_blown = 1; return -1; }
    return 0;
}

static ao_value_t eval_cell(ao_book_t *b, int sheet, uint32_t row, uint32_t col) {
    ao_sheet_t *s = ao_book_sheet_at(b, sheet);
    if (!s) return ao_val_err(AO_ERR_REF);
    ao_cell_t *c = ao_sheet_cell(b, s, row, col);
    if (!c) return ao_val_blank();
    if (c->content == AO_CELL_LITERAL) { c->state = AO_ST_CLEAN; return c->literal; }
    if (c->state == AO_ST_CLEAN) return c->value;
    if (c->state == AO_ST_BUSY) return ao_val_err(AO_ERR_CYCLE);
    if (!c->ast) { c->value = ao_val_err(AO_ERR_VALUE); c->state = AO_ST_CLEAN; return c->value; }
    c->state = AO_ST_BUSY;
    ao_value_t v = eval_node(b, sheet, c->ast);
    if (v.kind == AO_VAL_RANGE) v = ao_val_err(AO_ERR_VALUE);
    c->value = v;
    c->state = AO_ST_CLEAN;
    return v;
}

ao_value_t ao_sheet_get(ao_book_t *b, ao_sheet_t *s, uint32_t row, uint32_t col) {
    if (!b || !s) return ao_val_blank();
    return eval_cell(b, sheet_index(b, s->name), row, col);
}

int ao_book_recalc(ao_book_t *b) {
    if (!b) return AO_CALC_ERR_ARGS;
    b->steps = 0;
    b->budget_blown = 0;
    for (int si = 0; si < b->nsheets; si++) {
        ao_sheet_t *s = b->sheets[si];
        for (uint32_t i = 0; i < s->slot_cap; i++) {
            ao_cell_t *c = s->slots[i];
            if (c && c != AO_TOMB && c->state == AO_ST_DIRTY)
                eval_cell(b, si, c->row, c->col);
        }
    }
    if (b->budget_blown) {
        /* Whatever never settled reads #NUM!: the refusal, made visible. */
        for (int si = 0; si < b->nsheets; si++) {
            ao_sheet_t *s = b->sheets[si];
            for (uint32_t i = 0; i < s->slot_cap; i++) {
                ao_cell_t *c = s->slots[i];
                if (c && c != AO_TOMB && c->state != AO_ST_CLEAN) {
                    c->value = ao_val_err(AO_ERR_NUM);
                    c->state = AO_ST_CLEAN;
                }
            }
        }
        return AO_CALC_ERR_BUDGET;
    }
    return AO_CALC_OK;
}

/* Range iteration helper: visit every stored-or-blank coordinate. */
typedef int (*ao_visit_fn)(ao_book_t *b, ao_value_t v, void *ctx);
static ao_value_t range_value(int sheet, uint32_t r1, uint32_t c1,
                              uint32_t r2, uint32_t c2) {
    ao_value_t v = ao_val_blank();
    v.kind = AO_VAL_RANGE;
    v.sheet = sheet;
    v.r1 = r1; v.c1 = c1; v.r2 = r2; v.c2 = c2;
    return v;
}
/* Walk a value that must be a range; a scalar becomes a 1x1 range on the
 * context sheet is NOT attempted (callers check kind first). */
static int each_in_range(ao_book_t *b, ao_value_t rv, ao_visit_fn fn, void *ctx) {
    for (uint32_t r = rv.r1; r <= rv.r2; r++) {
        for (uint32_t c = rv.c1; c <= rv.c2; c++) {
            ao_value_t v = eval_cell(b, rv.sheet, r, c);
            if (fn(b, v, ctx) != 0) return -1;
            if (r == rv.r2 && c == rv.c2) return 0;
        }
    }
    return 0;
}

/* -- aggregate accumulators -- */
typedef struct { double sum, prod, min, max; long n, nall; int have; ao_err_t err; } agg_t;
static int agg_visit(ao_book_t *bk, ao_value_t v, void *ctx) {
    (void)bk;
    agg_t *a = ctx;
    if (v.kind == AO_VAL_ERR) { a->err = (ao_err_t)v.err; return -1; }
    a->nall++;
    if (v.kind == AO_VAL_NUM || v.kind == AO_VAL_BOOL) {
        double d = v.num;
        a->sum += d;
        a->prod *= d;
        if (!a->have || d < a->min) a->min = d;
        if (!a->have || d > a->max) a->max = d;
        a->have = 1;
        a->n++;
    }
    return 0;
}

/* Criteria, Excel-flavoured: ">=5", "<>text", "=x", or a bare value. */
typedef struct { int op; double num; char text[80]; int is_num; } crit_t;
static void crit_parse(ao_book_t *b, ao_value_t cv, crit_t *cr) {
    memset(cr, 0, sizeof *cr);
    cr->op = OP_EQ;
    if (cv.kind == AO_VAL_NUM) { cr->is_num = 1; cr->num = cv.num; return; }
    if (cv.kind == AO_VAL_BOOL) { cr->is_num = 1; cr->num = cv.num; return; }
    char buf[96];
    ao_val_to_text(b, cv, buf, sizeof buf);
    const char *p = buf;
    if (p[0] == '>' && p[1] == '=') { cr->op = OP_GE; p += 2; }
    else if (p[0] == '<' && p[1] == '=') { cr->op = OP_LE; p += 2; }
    else if (p[0] == '<' && p[1] == '>') { cr->op = OP_NE; p += 2; }
    else if (p[0] == '=') { p++; }
    else if (p[0] == '>') { cr->op = OP_GT; p++; }
    else if (p[0] == '<') { cr->op = OP_LT; p++; }
    char *e = NULL;
    double d = strtod(p, &e);
    if (e && e != p && *e == 0) { cr->is_num = 1; cr->num = d; return; }
    snprintf(cr->text, sizeof cr->text, "%.*s", (int)sizeof cr->text - 1, p);
}
static int crit_match(crit_t *cr, ao_value_t v) {
    if (v.kind == AO_VAL_ERR || v.kind == AO_VAL_RANGE) return 0;
    ao_value_t rhs = cr->is_num ? ao_val_num(cr->num) : ao_val_text(cr->text);
    /* A bare text criterion with no operator also matches as a prefix
     * wildcard in Excel; we match exactly (documented simplification
     * only where '*' is absent). */
    if (!cr->is_num && cr->text[0]) {
        size_t L = strlen(cr->text);
        if (L && cr->text[L - 1] == '*' && v.kind == AO_VAL_TEXT && v.text)
            return ci_ncmp(v.text, cr->text, L - 1) == 0;
    }
    int cmp = ao_cmp(v, rhs);
    switch (cr->op) {
        case OP_EQ: return cmp == 0;
        case OP_NE: return cmp != 0;
        case OP_LT: return cmp < 0;
        case OP_GT: return cmp > 0;
        case OP_LE: return cmp <= 0;
        case OP_GE: return cmp >= 0;
    }
    return 0;
}

static ao_value_t as_range(ao_book_t *b, int ctx, ao_node_t *nd, int *ok) {
    ao_value_t v = eval_node(b, ctx, nd);
    *ok = v.kind == AO_VAL_RANGE;
    return v;
}

static int arg_num(ao_book_t *b, int ctx, ao_node_t *nd, double *out) {
    ao_value_t v = eval_node(b, ctx, nd);
    if (v.kind == AO_VAL_ERR) return -(int)v.err;
    if (v.kind == AO_VAL_RANGE) {
        /* A range in scalar context: its top-left cell. */
        v = eval_cell(b, v.sheet, v.r1, v.c1);
        if (v.kind == AO_VAL_ERR) return -(int)v.err;
    }
    int e = ao_val_to_num(v, out);
    return e ? -e : 0;
}
static ao_value_t num_or_err(int rc) {
    return rc <= 0 ? ao_val_num(0) : ao_val_err((ao_err_t)rc);
}

static ao_value_t eval_func(ao_book_t *b, int ctx, ao_node_t *nd) {
    if (nd->func < 0) return ao_val_err(AO_ERR_NAME);
    int fn = nd->func;
    int na = nd->nargs;
    /* IF is lazy: the untaken branch is never evaluated. */
    if (fn == FN_IF) {
        if (na < 2 || na > 3) return ao_val_err(AO_ERR_VALUE);
        double cond;
        int rc = arg_num(b, ctx, nd->args[0], &cond);
        if (rc) return num_or_err(-rc);
        if (cond != 0) return eval_node(b, ctx, nd->args[1]);
        if (na == 3) return eval_node(b, ctx, nd->args[2]);
        return ao_val_bool(0);
    }
    /* Range-capable aggregates. */
    if (fn == FN_SUM || fn == FN_AVERAGE || fn == FN_MIN || fn == FN_MAX ||
        fn == FN_COUNT || fn == FN_COUNTA || fn == FN_PRODUCT ||
        fn == FN_MEDIAN || fn == FN_STDEV) {
        agg_t a;
        memset(&a, 0, sizeof a);
        a.prod = 1;
        /* MEDIAN/STDEV collect; the cap on range size bounds this. */
        double *vals = NULL;
        long vcap = 0;
        if (fn == FN_MEDIAN || fn == FN_STDEV) {
            vcap = 1024;
            vals = balloc(b, (size_t)vcap * sizeof *vals);
            if (!vals) return ao_val_err(AO_ERR_NUM);
        }
        for (int i = 0; i < na; i++) {
            ao_value_t v = eval_node(b, ctx, nd->args[i]);
            if (v.kind == AO_VAL_ERR) { if (vals) {} return ao_val_err((ao_err_t)v.err); }
            if (v.kind == AO_VAL_RANGE) {
                /* Re-walk with collection when needed. */
                if (fn == FN_MEDIAN || fn == FN_STDEV) {
                    for (uint32_t r = v.r1; r <= v.r2; r++) {
                        for (uint32_t c = v.c1; c <= v.c2; c++) {
                            ao_value_t cv = eval_cell(b, v.sheet, r, c);
                            if (cv.kind == AO_VAL_ERR) return ao_val_err((ao_err_t)cv.err);
                            if (cv.kind == AO_VAL_NUM || cv.kind == AO_VAL_BOOL) {
                                if (a.n == vcap) {
                                    long nc = vcap * 2;
                                    double *np = balloc(b, (size_t)nc * sizeof *np);
                                    if (!np) return ao_val_err(AO_ERR_NUM);
                                    memcpy(np, vals, (size_t)a.n * sizeof *np);
                                    vals = np; vcap = nc;
                                }
                                vals[a.n] = cv.num;
                            }
                            agg_visit(b, cv, &a);
                            if (r == v.r2 && c == v.c2) break;
                        }
                        if (r == v.r2) break;
                    }
                } else {
                    long before = a.n;
                    if (each_in_range(b, v, agg_visit, &a) != 0)
                        return ao_val_err(a.err ? a.err : AO_ERR_VALUE);
                    if (fn == FN_PRODUCT) {
                        /* recompute product by a second walk is wasteful;
                         * fold: sum-accumulator cannot multiply, so walk
                         * again accumulating the product only. */
                        (void)before;
                    }
                }
            } else {
                if (fn == FN_MEDIAN || fn == FN_STDEV) {
                    if (v.kind == AO_VAL_NUM || v.kind == AO_VAL_BOOL) {
                        if (a.n == vcap) {
                            long nc = vcap * 2;
                            double *np = balloc(b, (size_t)nc * sizeof *np);
                            if (!np) return ao_val_err(AO_ERR_NUM);
                            memcpy(np, vals, (size_t)a.n * sizeof *np);
                            vals = np; vcap = nc;
                        }
                        vals[a.n] = v.num;
                    }
                }
                agg_visit(b, v, &a);
                if (a.err) return ao_val_err(a.err);
            }
        }
        switch (fn) {
            case FN_SUM: return ao_val_num(a.sum);
            case FN_PRODUCT: return ao_val_num(a.n ? a.prod : 0);
            case FN_AVERAGE:
                if (!a.n) return ao_val_err(AO_ERR_DIV0);
                return ao_val_num(a.sum / (double)a.n);
            case FN_MIN: return ao_val_num(a.have ? a.min : 0);
            case FN_MAX: return ao_val_num(a.have ? a.max : 0);
            case FN_COUNT: return ao_val_num((double)a.n);
            case FN_COUNTA: return ao_val_num((double)a.nall);
            case FN_MEDIAN: {
                if (!a.n) return ao_val_err(AO_ERR_NUM);
                /* insertion sort: fine for gate-sized inputs */
                for (long i = 1; i < a.n; i++) {
                    double x = vals[i];
                    long j = i - 1;
                    while (j >= 0 && vals[j] > x) { vals[j + 1] = vals[j]; j--; }
                    vals[j + 1] = x;
                }
                if (a.n % 2) return ao_val_num(vals[a.n / 2]);
                return ao_val_num((vals[a.n / 2 - 1] + vals[a.n / 2]) / 2);
            }
            case FN_STDEV: {
                if (a.n < 2) return ao_val_err(AO_ERR_DIV0);
                double mean = a.sum / (double)a.n, ss = 0;
                for (long i = 0; i < a.n; i++) ss += (vals[i] - mean) * (vals[i] - mean);
                return ao_val_num(sqrt(ss / (double)(a.n - 1)));
            }
        }
    }
    (void)0;
    switch (fn) {
        case FN_AND: case FN_OR: {
            int all = 1, any = 0, seen = 0;
            for (int i = 0; i < na; i++) {
                ao_value_t v = eval_node(b, ctx, nd->args[i]);
                if (v.kind == AO_VAL_ERR) return ao_val_err((ao_err_t)v.err);
                if (v.kind == AO_VAL_RANGE) {
                    for (uint32_t r = v.r1; r <= v.r2; r++)
                        for (uint32_t c = v.c1; c <= v.c2; c++) {
                            ao_value_t cv = eval_cell(b, v.sheet, r, c);
                            if (cv.kind == AO_VAL_ERR) return ao_val_err((ao_err_t)cv.err);
                            if (cv.kind == AO_VAL_NUM || cv.kind == AO_VAL_BOOL) {
                                seen = 1;
                                if (cv.num) any = 1; else all = 0;
                            }
                            if (r == v.r2 && c == v.c2) break;
                        }
                } else if (v.kind == AO_VAL_NUM || v.kind == AO_VAL_BOOL) {
                    seen = 1;
                    if (v.num) any = 1; else all = 0;
                }
            }
            if (!seen) return ao_val_err(AO_ERR_VALUE);
            return ao_val_bool(fn == FN_AND ? all : any);
        }
        case FN_NOT: {
            double d; int rc = arg_num(b, ctx, nd->args[0], &d);
            if (rc) return num_or_err(-rc);
            return ao_val_bool(d == 0);
        }
        case FN_COUNTIF: case FN_SUMIF: case FN_AVERAGEIF: {
            if (na < 2) return ao_val_err(AO_ERR_VALUE);
            int ok;
            ao_value_t cr_range = as_range(b, ctx, nd->args[0], &ok);
            if (!ok) return ao_val_err(AO_ERR_VALUE);
            ao_value_t cv = eval_node(b, ctx, nd->args[1]);
            if (cv.kind == AO_VAL_ERR) return ao_val_err((ao_err_t)cv.err);
            crit_t cr; crit_parse(b, cv, &cr);
            ao_value_t sr = cr_range;
            if (na >= 3 && fn != FN_COUNTIF) {
                sr = as_range(b, ctx, nd->args[2], &ok);
                if (!ok) return ao_val_err(AO_ERR_VALUE);
            }
            double sum = 0; long cnt = 0;
            uint32_t hh = cr_range.r2 - cr_range.r1, ww = cr_range.c2 - cr_range.c1;
            for (uint32_t r = 0; r <= hh; r++) {
                for (uint32_t c = 0; c <= ww; c++) {
                    ao_value_t v = eval_cell(b, cr_range.sheet, cr_range.r1 + r, cr_range.c1 + c);
                    if (v.kind == AO_VAL_ERR) return ao_val_err((ao_err_t)v.err);
                    if (!crit_match(&cr, v)) continue;
                    if (fn == FN_COUNTIF) { cnt++; continue; }
                    ao_value_t sv = eval_cell(b, sr.sheet, sr.r1 + r, sr.c1 + c);
                    if (sv.kind == AO_VAL_ERR) return ao_val_err((ao_err_t)sv.err);
                    double d;
                    if (ao_val_to_num(sv, &d) == 0) { sum += d; cnt++; }
                }
            }
            if (fn == FN_COUNTIF) return ao_val_num((double)cnt);
            if (fn == FN_SUMIF) return ao_val_num(sum);
            if (!cnt) return ao_val_err(AO_ERR_DIV0);
            return ao_val_num(sum / (double)cnt);
        }
        case FN_SUMPRODUCT: {
            if (na != 2) return ao_val_err(AO_ERR_VALUE);
            int ok1, ok2;
            ao_value_t x = as_range(b, ctx, nd->args[0], &ok1);
            ao_value_t y = as_range(b, ctx, nd->args[1], &ok2);
            if (!ok1 || !ok2) return ao_val_err(AO_ERR_VALUE);
            uint32_t hh = x.r2 - x.r1, ww = x.c2 - x.c1;
            if (y.r2 - y.r1 != hh || y.c2 - y.c1 != ww) return ao_val_err(AO_ERR_VALUE);
            double sum = 0;
            for (uint32_t r = 0; r <= hh; r++)
                for (uint32_t c = 0; c <= ww; c++) {
                    ao_value_t a1 = eval_cell(b, x.sheet, x.r1 + r, x.c1 + c);
                    ao_value_t a2 = eval_cell(b, y.sheet, y.r1 + r, y.c1 + c);
                    if (a1.kind == AO_VAL_ERR) return a1;
                    if (a2.kind == AO_VAL_ERR) return a2;
                    double d1, d2;
                    if (ao_val_to_num(a1, &d1) || ao_val_to_num(a2, &d2))
                        return ao_val_err(AO_ERR_VALUE);
                    sum += d1 * d2;
                }
            return ao_val_num(sum);
        }
        case FN_VLOOKUP: case FN_HLOOKUP: {
            if (na < 3) return ao_val_err(AO_ERR_VALUE);
            ao_value_t key = eval_node(b, ctx, nd->args[0]);
            if (key.kind == AO_VAL_ERR) return key;
            int ok;
            ao_value_t t = as_range(b, ctx, nd->args[1], &ok);
            if (!ok) return ao_val_err(AO_ERR_VALUE);
            double idxd; int rc = arg_num(b, ctx, nd->args[2], &idxd);
            if (rc) return num_or_err(-rc);
            long idx = (long)idxd;
            int approx = 1;
            if (na >= 4) {
                double a4; rc = arg_num(b, ctx, nd->args[3], &a4);
                if (rc) return num_or_err(-rc);
                approx = a4 != 0;
            }
            int vert = fn == FN_VLOOKUP;
            uint32_t span = vert ? t.r2 - t.r1 : t.c2 - t.c1;
            long found = -1;
            ao_value_t best = ao_val_blank();
            for (uint32_t i = 0; i <= span; i++) {
                ao_value_t kv = vert ? eval_cell(b, t.sheet, t.r1 + i, t.c1)
                                     : eval_cell(b, t.sheet, t.r1, t.c1 + i);
                if (kv.kind == AO_VAL_ERR) return kv;
                int cmp = ao_cmp(key, kv);
                if (cmp == 0) { found = (long)i; break; }
                if (approx && cmp > 0) { found = (long)i; best = kv; }
                if (!approx && cmp != 0) continue;
                if (approx && cmp < 0) break;
            }
            (void)best;
            if (found < 0) return ao_val_err(AO_ERR_NA);
            if (vert) {
                if (t.c1 + (uint32_t)idx - 1 > t.c2 || idx < 1) return ao_val_err(AO_ERR_REF);
                return eval_cell(b, t.sheet, t.r1 + (uint32_t)found, t.c1 + (uint32_t)idx - 1);
            }
            if (t.r1 + (uint32_t)idx - 1 > t.r2 || idx < 1) return ao_val_err(AO_ERR_REF);
            return eval_cell(b, t.sheet, t.r1 + (uint32_t)idx - 1, t.c1 + (uint32_t)found);
        }
        case FN_INDEX: {
            if (na < 2) return ao_val_err(AO_ERR_VALUE);
            int ok;
            ao_value_t t = as_range(b, ctx, nd->args[0], &ok);
            if (!ok) return ao_val_err(AO_ERR_VALUE);
            double rd; int rc = arg_num(b, ctx, nd->args[1], &rd);
            if (rc) return num_or_err(-rc);
            long r = (long)rd, c = 1;
            if (na >= 3) {
                double cd; rc = arg_num(b, ctx, nd->args[2], &cd);
                if (rc) return num_or_err(-rc);
                c = (long)cd;
            } else if (t.r1 == t.r2) { c = r; r = 1; }
            if (r < 1 || c < 1 || t.r1 + (uint32_t)r - 1 > t.r2 ||
                t.c1 + (uint32_t)c - 1 > t.c2)
                return ao_val_err(AO_ERR_REF);
            return eval_cell(b, t.sheet, t.r1 + (uint32_t)r - 1, t.c1 + (uint32_t)c - 1);
        }
        case FN_MATCH: {
            if (na < 2) return ao_val_err(AO_ERR_VALUE);
            ao_value_t key = eval_node(b, ctx, nd->args[0]);
            if (key.kind == AO_VAL_ERR) return key;
            int ok;
            ao_value_t t = as_range(b, ctx, nd->args[1], &ok);
            if (!ok) return ao_val_err(AO_ERR_VALUE);
            long mtype = 1;
            if (na >= 3) {
                double md; int rc = arg_num(b, ctx, nd->args[2], &md);
                if (rc) return num_or_err(-rc);
                mtype = (long)md;
            }
            int horiz = t.r1 == t.r2;
            uint32_t span = horiz ? t.c2 - t.c1 : t.r2 - t.r1;
            long found = -1;
            for (uint32_t i = 0; i <= span; i++) {
                ao_value_t kv = horiz ? eval_cell(b, t.sheet, t.r1, t.c1 + i)
                                      : eval_cell(b, t.sheet, t.r1 + i, t.c1);
                if (kv.kind == AO_VAL_ERR) return kv;
                int cmp = ao_cmp(key, kv);
                if (mtype == 0) { if (cmp == 0) { found = (long)i; break; } }
                else if (mtype > 0) { if (cmp >= 0) found = (long)i; if (cmp < 0) break; }
                else { if (cmp <= 0) found = (long)i; if (cmp > 0) break; }
            }
            if (found < 0) return ao_val_err(AO_ERR_NA);
            return ao_val_num((double)found + 1);
        }
        case FN_RANK: {
            if (na < 2) return ao_val_err(AO_ERR_VALUE);
            double x; int rc = arg_num(b, ctx, nd->args[0], &x);
            if (rc) return num_or_err(-rc);
            int ok;
            ao_value_t t = as_range(b, ctx, nd->args[1], &ok);
            if (!ok) return ao_val_err(AO_ERR_VALUE);
            int asc = 0;
            if (na >= 3) {
                double od; rc = arg_num(b, ctx, nd->args[2], &od);
                if (rc) return num_or_err(-rc);
                asc = od != 0;
            }
            long rank = 1;
            for (uint32_t r = t.r1; r <= t.r2; r++)
                for (uint32_t c = t.c1; c <= t.c2; c++) {
                    ao_value_t v = eval_cell(b, t.sheet, r, c);
                    if (v.kind == AO_VAL_ERR) return v;
                    if (v.kind == AO_VAL_NUM || v.kind == AO_VAL_BOOL) {
                        if (!asc && v.num > x) rank++;
                        if (asc && v.num < x) rank++;
                    }
                    if (r == t.r2 && c == t.c2) break;
                }
            return ao_val_num((double)rank);
        }
        case FN_PI: return ao_val_num(3.14159265358979323846);
        case FN_TODAY: return ao_val_num(floor(book_now(b)));
        case FN_NOW: return ao_val_num(book_now(b));
        case FN_TEXT: {
            if (na != 2) return ao_val_err(AO_ERR_VALUE);
            ao_value_t v = eval_node(b, ctx, nd->args[0]);
            if (v.kind == AO_VAL_ERR) return v;
            char fbuf[64], out[160];
            ao_val_to_text(b, eval_node(b, ctx, nd->args[1]), fbuf, sizeof fbuf);
            ao_format_value(b, v, fbuf, out, sizeof out);
            char *dup = ao_strdup(b->arena, out);
            return dup ? ao_val_text(dup) : ao_val_err(AO_ERR_NUM);
        }
        default: break;
    }

    /* Scalar functions: coerce the arguments once. */
    double x, y;
    int rc;
    switch (fn) {
        case FN_ABS: case FN_INT: case FN_SQRT: case FN_EXP: case FN_LN:
        case FN_SIN: case FN_COS: case FN_TAN: case FN_LEN: {
            if (fn == FN_LEN) {
                ao_value_t v = eval_node(b, ctx, nd->args[0]);
                if (v.kind == AO_VAL_ERR) return v;
                char tb[256];
                ao_val_to_text(b, v, tb, sizeof tb);
                return ao_val_num((double)strlen(tb));
            }
            rc = arg_num(b, ctx, nd->args[0], &x);
            if (rc) return num_or_err(-rc);
            switch (fn) {
                case FN_ABS: return ao_val_num(fabs(x));
                case FN_INT: return ao_val_num(floor(x));
                case FN_SQRT:
                    if (x < 0) return ao_val_err(AO_ERR_NUM);
                    return ao_val_num(sqrt(x));
                case FN_EXP: return ao_val_num(exp(x));
                case FN_LN:
                    if (x <= 0) return ao_val_err(AO_ERR_NUM);
                    return ao_val_num(log(x));
                case FN_SIN: return ao_val_num(sin(x));
                case FN_COS: return ao_val_num(cos(x));
                case FN_TAN: return ao_val_num(tan(x));
            }
            return ao_val_err(AO_ERR_VALUE);   /* not reached */
        }
        case FN_MOD: case FN_POWER: case FN_LOG: {
            rc = arg_num(b, ctx, nd->args[0], &x);
            if (rc) return num_or_err(-rc);
            double base = 10;
            if (fn == FN_MOD || fn == FN_POWER || na >= 2) {
                rc = arg_num(b, ctx, nd->args[1], &y);
                if (rc) return num_or_err(-rc);
                base = y;
            }
            if (fn == FN_MOD) {
                if (y == 0) return ao_val_err(AO_ERR_DIV0);
                return ao_val_num(x - y * floor(x / y));
            }
            if (fn == FN_POWER) {
                double r = pow(x, y);
                if (ao_isnan(r)) return ao_val_err(AO_ERR_NUM);
                return ao_val_num(r);
            }
            if (x <= 0 || base <= 0 || base == 1) return ao_val_err(AO_ERR_NUM);
            return ao_val_num(log(x) / log(base));
        }
        case FN_ROUND: case FN_ROUNDUP: case FN_ROUNDDOWN: {
            rc = arg_num(b, ctx, nd->args[0], &x);
            if (rc) return num_or_err(-rc);
            double ndig = 0;
            if (na >= 2) {
                rc = arg_num(b, ctx, nd->args[1], &ndig);
                if (rc) return num_or_err(-rc);
            }
            double f = pow(10.0, ndig);
            double v = x * f;
            double r = fn == FN_ROUND ? floor(v + 0.5)
                       : fn == FN_ROUNDUP ? (v >= 0 ? ceil(v) : floor(v))
                                          : (v >= 0 ? floor(v) : ceil(v));
            if (x < 0 && fn == FN_ROUND) r = -floor(-v + 0.5);
            return ao_val_num(r / f);
        }
        case FN_VALUE: {
            ao_value_t v = eval_node(b, ctx, nd->args[0]);
            if (v.kind == AO_VAL_ERR) return v;
            double d;
            int e = ao_val_to_num(v, &d);
            return e ? ao_val_err((ao_err_t)e) : ao_val_num(d);
        }
        case FN_CONCATENATE: case FN_UPPER: case FN_LOWER: case FN_TRIM:
        case FN_LEFT: case FN_RIGHT: case FN_MID: {
            /* String functions share one arena-built result. */
            char acc[1024];
            acc[0] = 0;
            if (fn == FN_CONCATENATE) {
                for (int i = 0; i < na; i++) {
                    ao_value_t v = eval_node(b, ctx, nd->args[i]);
                    if (v.kind == AO_VAL_ERR) return v;
                    char tb[256];
                    ao_val_to_text(b, v, tb, sizeof tb);
                    size_t cur = strlen(acc);
                    snprintf(acc + cur, sizeof acc - cur, "%s", tb);
                }
                char *dup = ao_strdup(b->arena, acc);
                return dup ? ao_val_text(dup) : ao_val_err(AO_ERR_NUM);
            }
            ao_value_t v = eval_node(b, ctx, nd->args[0]);
            if (v.kind == AO_VAL_ERR) return v;
            char tb[512];
            ao_val_to_text(b, v, tb, sizeof tb);
            if (fn == FN_UPPER || fn == FN_LOWER) {
                for (char *p = tb; *p; p++)
                    *p = (char)(fn == FN_UPPER ? toupper((unsigned char)*p)
                                               : tolower((unsigned char)*p));
                snprintf(acc, sizeof acc, "%s", tb);
            } else if (fn == FN_TRIM) {
                const char *p = tb;
                while (*p == ' ') p++;
                const char *e2 = p + strlen(p);
                while (e2 > p && e2[-1] == ' ') e2--;
                size_t w = 0;
                int space = 0;
                for (const char *t = p; t < e2; t++) {
                    if (*t == ' ') { space = 1; continue; }
                    if (space && w) acc[w++] = ' ';
                    space = 0;
                    if (w + 1 < sizeof acc) acc[w++] = *t;
                }
                acc[w] = 0;
            } else {
                double d1 = 0, d2 = 0;
                long L = (long)strlen(tb);
                if (fn == FN_LEFT || fn == FN_RIGHT) {
                    rc = arg_num(b, ctx, nd->args[1], &d1);
                    if (rc) return num_or_err(-rc);
                    long k = (long)d1;
                    if (k < 0) return ao_val_err(AO_ERR_VALUE);
                    if (k > L) k = L;
                    if (fn == FN_LEFT) snprintf(acc, sizeof acc, "%.*s", (int)k, tb);
                    else snprintf(acc, sizeof acc, "%s", tb + (L - k));
                } else {   /* MID: 1-based start, count */
                    rc = arg_num(b, ctx, nd->args[1], &d1);
                    if (rc) return num_or_err(-rc);
                    rc = arg_num(b, ctx, nd->args[2], &d2);
                    if (rc) return num_or_err(-rc);
                    long st = (long)d1 - 1, k = (long)d2;
                    if (st < 0 || k < 0) return ao_val_err(AO_ERR_VALUE);
                    if (st > L) st = L;
                    if (st + k > L) k = L - st;
                    snprintf(acc, sizeof acc, "%.*s", (int)k, tb + st);
                }
            }
            char *dup = ao_strdup(b->arena, acc);
            return dup ? ao_val_text(dup) : ao_val_err(AO_ERR_NUM);
        }
        default:
            return ao_val_err(AO_ERR_NAME);
    }
}

static ao_value_t eval_node(ao_book_t *b, int ctx, ao_node_t *nd) {
    if (!nd || nd->kind == ND_BAD) return ao_val_err(AO_ERR_VALUE);
    if (spend(b) != 0) return ao_val_err(AO_ERR_NUM);
    switch (nd->kind) {
        case ND_NUM: return ao_val_num(nd->num);
        case ND_TEXT: return ao_val_text(nd->text);
        case ND_BOOL: return ao_val_bool(nd->num != 0);
        case ND_REF:
            if (nd->sheet < 0) return ao_val_err(AO_ERR_REF);
            return eval_cell(b, nd->sheet, nd->r1, nd->c1);
        case ND_RANGE:
            if (nd->sheet < 0) return ao_val_err(AO_ERR_REF);
            return range_value(nd->sheet, nd->r1, nd->c1, nd->r2, nd->c2);
        case ND_FUNC: return eval_func(b, ctx, nd);
        case ND_OP: break;
        default: return ao_val_err(AO_ERR_VALUE);
    }
    if (nd->op == OP_NEG || nd->op == OP_PCT) {
        ao_value_t v = eval_node(b, ctx, nd->a);
        if (v.kind == AO_VAL_ERR) return v;
        double d;
        int e = ao_val_to_num(v, &d);
        if (e) return ao_val_err((ao_err_t)e);
        return ao_val_num(nd->op == OP_NEG ? -d : d / 100.0);
    }
    ao_value_t l = eval_node(b, ctx, nd->a);
    if (l.kind == AO_VAL_ERR) return l;
    ao_value_t r = eval_node(b, ctx, nd->b);
    if (r.kind == AO_VAL_ERR) return r;
    switch (nd->op) {
        case OP_CAT: {
            char lb[256], rb[256], out[520];
            ao_val_to_text(b, l, lb, sizeof lb);
            ao_val_to_text(b, r, rb, sizeof rb);
            snprintf(out, sizeof out, "%s%s", lb, rb);
            char *dup = ao_strdup(b->arena, out);
            return dup ? ao_val_text(dup) : ao_val_err(AO_ERR_NUM);
        }
        case OP_EQ: return ao_val_bool(ao_cmp(l, r) == 0);
        case OP_NE: return ao_val_bool(ao_cmp(l, r) != 0);
        case OP_LT: return ao_val_bool(ao_cmp(l, r) < 0);
        case OP_GT: return ao_val_bool(ao_cmp(l, r) > 0);
        case OP_LE: return ao_val_bool(ao_cmp(l, r) <= 0);
        case OP_GE: return ao_val_bool(ao_cmp(l, r) >= 0);
        default: break;
    }
    double x, y;
    int e = ao_val_to_num(l, &x);
    if (e) return ao_val_err((ao_err_t)e);
    e = ao_val_to_num(r, &y);
    if (e) return ao_val_err((ao_err_t)e);
    switch (nd->op) {
        case OP_ADD: return ao_val_num(x + y);
        case OP_SUB: return ao_val_num(x - y);
        case OP_MUL: return ao_val_num(x * y);
        case OP_DIV:
            if (y == 0) return ao_val_err(AO_ERR_DIV0);
            return ao_val_num(x / y);
        case OP_POW: {
            double v = pow(x, y);
            if (ao_isnan(v)) return ao_val_err(AO_ERR_NUM);
            return ao_val_num(v);
        }
    }
    return ao_val_err(AO_ERR_VALUE);
}

int ao_formula_eval(ao_book_t *b, ao_sheet_t *ctx, const char *formula,
                    ao_value_t *out) {
    if (!b || !ctx || !formula || !out) return AO_CALC_ERR_ARGS;
    if (formula[0] == '=') formula++;
    ao_dep_t *deps; uint32_t ndeps;
    ao_rdep_t *rdeps; uint32_t nrdeps;
    int si = sheet_index(b, ctx->name);
    ao_node_t *ast = parse_formula(b, si, formula, &deps, &ndeps, &rdeps, &nrdeps);
    if (!ast) {
        *out = ao_val_err(AO_ERR_VALUE);
        return AO_CALC_ERR_PARSE;
    }
    b->steps = 0;
    b->budget_blown = 0;
    *out = eval_node(b, si, ast);
    if (out->kind == AO_VAL_RANGE) *out = ao_val_err(AO_ERR_VALUE);
    return AO_CALC_OK;
}


/* -------------------------------------------------------- number formats */

static void civil_from_serial(double serial, int *yy, int *mm, int *dd,
                              int *hh, int *mi) {
    long days = (long)floor(serial);
    double frac = serial - (double)days;
    long z = days - 25569 + 719468;   /* serial 25569 = 1970-01-01 (the fake 1900 leap day absorbed) */
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *dd = (int)(doy - (153 * mp + 2) / 5 + 1);
    *mm = (int)(mp < 10 ? mp + 3 : mp - 9);
    *yy = (int)(y + (*mm <= 2));
    long secs = (long)(frac * 86400.0 + 0.5);
    *hh = (int)(secs / 3600);
    *mi = (int)((secs % 3600) / 60);
}

void ao_format_value(ao_book_t *b, ao_value_t v, const char *fmt,
                     char *buf, size_t n) {
    if (!buf || n == 0) return;
    if (v.kind != AO_VAL_NUM || !fmt || !*fmt ||
        ci_cmp(fmt, "General") == 0) {
        ao_val_to_text(b, v, buf, n);
        return;
    }
    /* Date formats: any of yyyy/dd tokens alongside an m-token. */
    int has_y = strstr(fmt, "yyyy") != NULL || strstr(fmt, "yy") != NULL;
    int has_d = strstr(fmt, "dd") != NULL;
    int has_m = strchr(fmt, 'm') != NULL || strchr(fmt, 'M') != NULL;
    int has_t = strstr(fmt, "hh") != NULL;
    if ((has_y || has_d) && has_m) {
        int yy, mm, dd, hh, mi;
        civil_from_serial(v.num, &yy, &mm, &dd, &hh, &mi);
        /* Walk the format, substituting the tokens we know. */
        size_t w = 0;
        const char *p = fmt;
        while (*p && w + 8 < n) {
            if (strncmp(p, "yyyy", 4) == 0) { w += (size_t)snprintf(buf + w, n - w, "%04d", yy); p += 4; }
            else if (strncmp(p, "yy", 2) == 0) { w += (size_t)snprintf(buf + w, n - w, "%02d", yy % 100); p += 2; }
            else if (strncmp(p, "dd", 2) == 0) { w += (size_t)snprintf(buf + w, n - w, "%02d", dd); p += 2; }
            else if (strncmp(p, "mm", 2) == 0) { w += (size_t)snprintf(buf + w, n - w, "%02d", has_t && (p > fmt && p[-1] == ':') ? mi : mm); p += 2; }
            else if (strncmp(p, "hh", 2) == 0) { w += (size_t)snprintf(buf + w, n - w, "%02d", hh); p += 2; }
            else if (*p == 'd') { w += (size_t)snprintf(buf + w, n - w, "%d", dd); p++; }
            else if (*p == 'm') { w += (size_t)snprintf(buf + w, n - w, "%d", mm); p++; }
            else buf[w++] = *p++;
        }
        buf[w] = 0;
        return;
    }
    /* Numeric formats: [literal] 0/# pattern [. 0/#...] [%] [literal] */
    const char *p = fmt;
    char prefix[24], suffix[24];
    size_t pw = 0, sw = 0;
    while (*p && *p != '0' && *p != '#' && pw + 1 < sizeof prefix) {
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && pw + 1 < sizeof prefix) prefix[pw++] = *p++;
            if (*p == '"') p++;
        } else prefix[pw++] = *p++;
    }
    prefix[pw] = 0;
    int thousands = 0, decimals = 0;
    while (*p == '0' || *p == '#' || *p == ',') {
        if (*p == ',') thousands = 1;
        p++;
    }
    if (*p == '.') {
        p++;
        while (*p == '0' || *p == '#') { decimals++; p++; }
    }
    int percent = 0;
    if (*p == '%') { percent = 1; p++; }
    while (*p && sw + 1 < sizeof suffix) {
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && sw + 1 < sizeof suffix) suffix[sw++] = *p++;
            if (*p == '"') p++;
        } else suffix[sw++] = *p++;
    }
    suffix[sw] = 0;
    double x = percent ? v.num * 100.0 : v.num;
    char core[192];
    snprintf(core, sizeof core, "%.*f", decimals, x);
    if (thousands) {
        /* Insert commas into the integer part. */
        char *dot = strchr(core, '.');
        size_t ilen = dot ? (size_t)(dot - core) : strlen(core);
        size_t start = core[0] == '-' ? 1 : 0;
        char grouped[180];
        size_t w = 0;
        for (size_t i = start; i < ilen; i++) {
            if (i > start && (ilen - i) % 3 == 0 && w + 1 < sizeof grouped)
                grouped[w++] = ',';
            if (w + 1 < sizeof grouped) grouped[w++] = core[i];
        }
        if (dot && w + strlen(dot) + 1 < sizeof grouped) {
            memcpy(grouped + w, dot, strlen(dot));
            w += strlen(dot);
        }
        grouped[w] = 0;
        snprintf(core, sizeof core, "%s%s", start ? "-" : "", grouped);
    }
    snprintf(buf, n, "%s%s%s%s", prefix, core, percent ? "%" : "", suffix);
}

/* ---------------------------------------------------------- file format */

int ao_book_save(ao_book_t *b, ao_emit_fn emit, void *ctx) {
    if (!b || !emit) return AO_CALC_ERR_ARGS;
    char line[2304];
    for (int si = 0; si < b->nsheets; si++) {
        ao_sheet_t *s = b->sheets[si];
        int k = snprintf(line, sizeof line, "[%s]\n", s->name);
        emit(ctx, line, (size_t)k);
        for (uint32_t i = 0; i < s->count; i++) {
            ao_cell_t *c = ao_sheet_cell_sorted(b, s, i);
            char ref[24];
            ao_ref_name(c->row, c->col, ref, sizeof ref);
            const char *in = c->input ? c->input : "";
            /* Text that could be mistaken for a number/formula on load
             * is written quoted: the load side strips one quote layer. */
            int quote = 0;
            if (c->content == AO_CELL_LITERAL && c->literal.kind == AO_VAL_TEXT) {
                const char *t = c->literal.text;
                if (t && (t[0] == '=' || t[0] == '"' ||
                          ci_cmp(t, "TRUE") == 0 || ci_cmp(t, "FALSE") == 0))
                    quote = 1;
                if (t) {
                    char *e = NULL;
                    (void)strtod(t, &e);
                    if (e && e != t && *e == 0) quote = 1;
                }
            }
            if (quote) k = snprintf(line, sizeof line, "%s = \"%s\"\n", ref, c->literal.text);
            else       k = snprintf(line, sizeof line, "%s = %s\n", ref, in);
            if (k > 0) emit(ctx, line, (size_t)(k < (int)sizeof line ? k : (int)sizeof line));
        }
    }
    return AO_CALC_OK;
}

int ao_book_load(ao_book_t *b, const char *text, size_t n) {
    if (!b || !text) return AO_CALC_ERR_ARGS;
    ao_sheet_t *cur = NULL;
    size_t i = 0;
    while (i < n) {
        /* One line. */
        size_t start = i;
        while (i < n && text[i] != '\n') i++;
        size_t len = i - start;
        if (i < n) i++;
        while (len && (text[start + len - 1] == '\r' || text[start + len - 1] == ' '))
            len--;
        const char *p = text + start;
        while (len && (*p == ' ' || *p == '\t')) { p++; len--; }
        if (!len || *p == '#') continue;
        if (*p == '[') {
            if (len < 3 || p[len - 1] != ']') return AO_CALC_ERR_FORMAT;
            char name[64];
            size_t nl = len - 2 < sizeof name - 1 ? len - 2 : sizeof name - 1;
            memcpy(name, p + 1, nl);
            name[nl] = 0;
            cur = ao_book_sheet(b, name, 1);
            if (!cur) return AO_CALC_ERR_NOMEM;
            continue;
        }
        /* REF = value */
        const char *eq = NULL;
        for (size_t k = 0; k < len; k++)
            if (p[k] == '=') { eq = p + k; break; }
        if (!eq) return AO_CALC_ERR_FORMAT;
        if (!cur) {
            cur = ao_book_sheet(b, "Sheet1", 1);
            if (!cur) return AO_CALC_ERR_NOMEM;
        }
        char ref[24];
        size_t rl = (size_t)(eq - p);
        while (rl && (p[rl - 1] == ' ' || p[rl - 1] == '\t')) rl--;
        if (rl == 0 || rl >= sizeof ref) return AO_CALC_ERR_FORMAT;
        memcpy(ref, p, rl);
        ref[rl] = 0;
        uint32_t row, col;
        if (ao_ref_parse(ref, &row, &col, NULL, NULL) != 0)
            return AO_CALC_ERR_FORMAT;
        const char *v = eq + 1;
        size_t vl = len - (size_t)(v - p);
        while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
        char *val = balloc(b, vl + 1);
        if (!val) return AO_CALC_ERR_NOMEM;
        memcpy(val, v, vl);
        val[vl] = 0;
        /* A quoted value is literal text (one layer stripped in set). */
        int rc = ao_sheet_set(b, cur, row, col, val);
        if (rc != AO_CALC_OK) return rc;
    }
    return AO_CALC_OK;
}

/* -------------------------------------------------------------------- CSV */

typedef struct {
    ao_book_t *b;
    ao_sheet_t *s;
    const char *p, *end;
    char delim;
} csv_t;

/* Parse one field into an arena string.  Advances past the delimiter or
 * newline; *term receives ',', '\\n' or 0 (end of input). */
static char *csv_field(csv_t *c, char *term) {
    const char *p = c->p;
    char *out;
    if (p < c->end && *p == '"') {
        /* Quoted: measure, then copy with "" unescaped. */
        const char *q = p + 1;
        size_t len = 0;
        for (;;) {
            if (q >= c->end) break;
            if (*q == '"') {
                if (q + 1 < c->end && q[1] == '"') { len++; q += 2; continue; }
                q++;
                break;
            }
            len++; q++;
        }
        out = balloc(c->b, len + 1);
        if (!out) return NULL;
        size_t w = 0;
        const char *t = p + 1;
        while (t < q) {
            if (*t == '"' && t + 1 < q && t[1] == '"') t++;
            if (w < len) out[w++] = *t;
            t++;
        }
        out[w] = 0;
        p = q;
    } else {
        const char *q = p;
        while (q < c->end && *q != c->delim && *q != '\n' && *q != '\r') q++;
        out = balloc(c->b, (size_t)(q - p) + 1);
        if (!out) return NULL;
        memcpy(out, p, (size_t)(q - p));
        out[q - p] = 0;
        p = q;
    }
    if (p < c->end && *p == c->delim) { *term = ','; p++; }
    else if (p < c->end && *p == '\r') {
        p++;
        if (p < c->end && *p == '\n') p++;
        *term = '\n';
    } else if (p < c->end && *p == '\n') { *term = '\n'; p++; }
    else *term = 0;
    c->p = p;
    return out;
}

int ao_csv_import(ao_book_t *b, ao_sheet_t *s, const char *text, size_t n,
                  int sniff, char delim) {
    if (!b || !s || !text) return AO_CALC_ERR_ARGS;
    if (sniff) {
        delim = ',';
        int tabs = 0, semis = 0, commas = 0;
        for (size_t i = 0; i < n && text[i] != '\n'; i++) {
            if (text[i] == '\t') tabs++;
            else if (text[i] == ';') semis++;
            else if (text[i] == ',') commas++;
        }
        if (tabs && tabs >= commas && tabs >= semis) delim = '\t';
        else if (semis > commas) delim = ';';
    }
    csv_t c = { b, s, text, text + n, delim };
    uint32_t row = 0, col = 0;
    while (c.p < c.end) {
        /* A wholly empty line is a row of blanks: skip it entirely. */
        if (*c.p == '\n') { c.p++; row++; continue; }
        if (*c.p == '\r' && c.p + 1 < c.end && c.p[1] == '\n') { c.p += 2; row++; continue; }
        char term = 0;
        char *f = csv_field(&c, &term);
        if (!f) return AO_CALC_ERR_NOMEM;
        if (*f) {
            int rc = ao_sheet_set(b, s, row, col, f);
            if (rc != AO_CALC_OK) return rc;
        }
        if (term == ',') col++;
        else { row++; col = 0; }
        if (row >= AO_SHEET_MAX_ROWS) break;
    }
    return AO_CALC_OK;
}

int ao_csv_export(ao_book_t *b, ao_sheet_t *s, char delim,
                  ao_emit_fn emit, void *ctx) {
    if (!b || !s || !emit) return AO_CALC_ERR_ARGS;
    if (!s->count) return AO_CALC_OK;
    /* The used rectangle, from the stored cells. */
    uint32_t maxr = 0, maxc = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        ao_cell_t *c = ao_sheet_cell_sorted(b, s, i);
        if (c->row > maxr) maxr = c->row;
        if (c->col > maxc) maxc = c->col;
    }
    char cell[2304];
    for (uint32_t r = 0; r <= maxr; r++) {
        for (uint32_t c = 0; c <= maxc; c++) {
            ao_value_t v = ao_sheet_get(b, s, r, c);
            char tb[1024];
            ao_val_to_text(b, v, tb, sizeof tb);
            int need = 0;
            for (const char *p = tb; *p; p++)
                if (*p == delim || *p == '"' || *p == '\n' || *p == '\r') need = 1;
            if (need) {
                size_t w = 0;
                cell[w++] = '"';
                for (const char *p = tb; *p && w + 3 < sizeof cell; p++) {
                    if (*p == '"') cell[w++] = '"';
                    cell[w++] = *p;
                }
                cell[w++] = '"';
                cell[w] = 0;
            } else {
                snprintf(cell, sizeof cell, "%s", tb);
            }
            emit(ctx, cell, strlen(cell));
            if (c < maxc) emit(ctx, &delim, 1);
        }
        emit(ctx, "\n", 1);
    }
    return AO_CALC_OK;
}
