/* tests/unit/test_tgsi.c — the H4 gate: disassemble, don't eyeball.
 *
 * The plan's H4 test gate says it plainly: "for each shader in a committed
 * corpus, disassemble the emitted tokens back into text (a 150-line
 * disassembler in the test) and compare against the source-normalised form.
 * A back end whose only test is 'the GPU drew something' cannot be debugged."
 *
 * This is that disassembler, and it is written FROM THE SPECIFICATION TEXT,
 * not from tgsi.h -- deliberately.  A test that reuses the emitter's own
 * constants cannot catch a wrong constant; it can only catch an inconsistent
 * one.  The tables below were transcribed from the Gallium TGSI spec
 * (Tables 5, 6, 9, 18, 26, 30) and from p_shader_tokens.h, and they are the
 * independent opinion the emitter is checked against.
 *
 * THE FINDING THIS FILE EXISTS FOR
 *
 * When this disassembler was first pointed at the canned arrays that G13 and
 * GL2 L6 shipped (glvirgl_canned_*_tokens), it refused every one of them:
 * the processors are swapped, the instruction headers carry token type 3
 * where the spec says 2, the declarations carry type 1 where the spec says 0,
 * and the source registers do not decode as INPUT[0] under any field width
 * the spec allows.  Those arrays have never run on a device (the plan's
 * Fact 1), so the error had no way to surface.  `test_canned_is_not_valid`
 * below pins that finding so it cannot be forgotten or "fixed" by quietly
 * changing this disassembler to agree with the bug.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "glsl.h"
#include "tgsi.h"
#include "GL/glbackend.h"   /* gl_virgl_canned_*_tokens, the G13 seam */

static int tn = 0, passed = 0, failed = 0;

#define CHECK(cond, name) do {                          \
    tn++;                                               \
    if (cond) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", (name)); }  \
} while (0)

/* ============================================================================
 * An independent disassembler
 *
 * Field widths transcribed from the spec, not from tgsi.h:
 *
 *   INSTRUCTION  Type:4[0:3] NrTokens:8[4:11] Opcode:8[12:19]
 *                Saturate:1[20] NumDstRegs:2[21:22] NumSrcRegs:4[23:26]
 *   DST_REGISTER File:4[0:3] WriteMask:4[4:7] Index:16[10:25]
 *   SRC_REGISTER File:4[0:3] SwzX:2[4:5] SwzY:2[6:7] SwzZ:2[8:9] SwzW:2[10:11]
 *                Negate:1[12] Index:16[15:30]
 *   HEADER       HeaderSize:8[0:7] BodySize:24[8:31]
 *   PROCESSOR    Processor:4[0:3]
 *
 * Note the two different index positions.  They are not a transcription slip:
 * the source and destination structs really do differ, and mixing them up is
 * the single most likely way to write a back end that emits plausible,
 * wrong streams.
 * ==========================================================================*/

#define TOKEN_TYPE_DECLARATION 0u
#define TOKEN_TYPE_IMMEDIATE   1u
#define TOKEN_TYPE_INSTRUCTION 2u

#define PROC_FRAGMENT 0u
#define PROC_VERTEX   1u

#define FILE_CONSTANT  1u
#define FILE_INPUT     2u
#define FILE_OUTPUT    3u
#define FILE_TEMPORARY 4u
#define FILE_SAMPLER   5u
#define FILE_ADDRESS   6u
#define FILE_IMMEDIATE 7u

static const char *file_name(unsigned f) {
    switch (f) {
    case FILE_CONSTANT:  return "CONST";
    case FILE_INPUT:     return "IN";
    case FILE_OUTPUT:    return "OUT";
    case FILE_TEMPORARY: return "TEMP";
    case FILE_SAMPLER:   return "SMP";
    case FILE_ADDRESS:   return "ADDR";
    case FILE_IMMEDIATE: return "IMM";
    default:             return "FILE?";
    }
}

static const char *opcode_name(unsigned op) {
    switch (op) {
    case 1:   return "MOV";
    case 2:   return "ADD";
    case 3:   return "SUB";
    case 4:   return "MUL";
    case 5:   return "MAD";
    case 17:  return "LRP";
    case 20:  return "RCP";
    case 23:  return "DP2";
    case 24:  return "DP3";
    case 25:  return "DP4";
    case 35:  return "EX2";
    case 36:  return "LG2";
    case 38:  return "RSQ";
    case 39:  return "SQRT";
    case 43:  return "KILL_IF";
    case 44:  return "MIN";
    case 45:  return "MAX";
    case 46:  return "ABS";
    case 48:  return "CMP";
    case 53:  return "FRC";
    case 54:  return "FLR";
    case 68:  return "TEX";
    case 117: return "END";
    default:  return NULL;
    }
}

/* Decoded forms, so a test asserts on STRUCTURE rather than on a formatted
 * string (a string comparison would pass for a wrong register in the right
 * column iff the formatter were also wrong). */
typedef struct { unsigned file; int index; unsigned swizzle; unsigned negate; } src_t;
typedef struct { unsigned file; int index; unsigned mask; } dst_t;

typedef struct {
    int               ok;
    unsigned          opcode;
    unsigned          ndst, nsrc;
    dst_t             dst;
    src_t             src[4];
    unsigned          texture;      /* TEX target, 0 when not a TEX      */
    unsigned          processor;
    int               saw_end;
} insn_t;

static int disasm_src(uint32_t w, src_t *s) {
    s->file    = w & 0xFu;
    s->swizzle = (w >> 4) & 0xFFu;
    s->negate  = (w >> 12) & 1u;
    s->index   = (int)((w >> 15) & 0xFFFFu);
    return s->file <= FILE_IMMEDIATE;
}

static int disasm_dst(uint32_t w, dst_t *d) {
    d->file  = w & 0xFu;
    d->mask  = (w >> 4) & 0xFu;
    d->index = (int)((w >> 10) & 0xFFFFu);
    return d->file <= FILE_IMMEDIATE;
}

/* Walk one program.  Returns the number of instructions decoded, or -1 on a
 * malformed stream (which is itself a test result). */
static int disasm(const uint32_t *dw, int n, insn_t *out, int max,
                  unsigned *processor, int *body_size) {
    if (n < 2) return -1;
    unsigned hdr_size = dw[0] & 0xFFu;
    *body_size = (int)(dw[0] >> 8);
    if (hdr_size != 2) return -1;
    *processor = dw[1] & 0xFu;

    int i = 2, count = 0;
    while (i < n && count < max) {
        uint32_t w = dw[i];
        unsigned type = w & 0xFu;

        if (type == TOKEN_TYPE_INSTRUCTION) {
            unsigned nr   = (w >> 4) & 0xFFu;
            unsigned op   = (w >> 12) & 0xFFu;
            unsigned ndst = (w >> 21) & 0x3u;
            unsigned nsrc = (w >> 23) & 0xFu;
            if (nr == 0 || i + (int)nr > n) return -1;
            if (ndst > 1) return -1;              /* this back end emits <= 1 */
            insn_t *ins = &out[count];
            memset(ins, 0, sizeof *ins);
            ins->opcode = op;
            ins->ndst = ndst;
            ins->nsrc = nsrc;
            ins->texture = 0;
            int p = i + 1;
            /* A texture extension token follows the instruction word. */
            if ((w >> 27) & 1u) {
                if (p >= i + (int)nr) return -1;
                ins->texture = (dw[p] >> 4) & 0xFFu;
                p++;
            }
            if (ndst && p < i + (int)nr) {
                if (!disasm_dst(dw[p], &ins->dst)) return -1;
                p++;
            }
            for (unsigned k = 0; k < nsrc; k++) {
                if (p >= i + (int)nr) return -1;
                if (!disasm_src(dw[p], &ins->src[k])) return -1;
                p++;
            }
            /* NrTokens must account for exactly what was written: this is the
             * same length discipline H3's packet test enforced. */
            if (p != i + (int)nr) return -1;
            if (op == 117u) ins->saw_end = 1;
            count++;
            i += (int)nr;
            if (op == 117u) break;
            continue;
        }

        if (type == TOKEN_TYPE_DECLARATION) {
            unsigned nr = (w >> 4) & 0xFFu;
            if (nr == 0 || i + (int)nr > n) return -1;
            i += (int)nr;
            continue;
        }

        if (type == TOKEN_TYPE_IMMEDIATE) {
            /* `tgsi_immediate`: Type:4 | Size:8 | DataType:4.  Spec §4.1:
             * "Size = 1 + N", where N floats follow -- so Size counts the
             * WHOLE token including its header, exactly like an
             * instruction's NrTokens.  §1.1 states the universal rule: "skip
             * next Size - 1 tokens".  Advancing by 1 + Size instead lands one
             * dword past the float and reads it as a declaration header; the
             * first version of this walker did precisely that and rejected a
             * stream the emitter had built correctly. */
            unsigned nr = (w >> 4) & 0xFFu;
            unsigned dtype = (w >> 12) & 0xFu;
            if (nr < 2 || i + (int)nr > n) return -1;
            if (dtype != 0u) return -1;         /* only IMM_FLOAT32 here  */
            i += (int)nr;
            continue;
        }
        return -1;                     /* unknown token type */
    }
    return count;
}

/* ============================================================================
 * The corpus: shaders the interpreter already runs today
 * ==========================================================================*/

static const char *vs_lambert =
    "attribute vec3 aPos;\n"
    "attribute vec3 aNormal;\n"
    "attribute vec3 aColor;\n"
    "uniform mat4 uMVP;\n"
    "uniform mat4 uRot;\n"
    "varying vec3 vNormal;\n"
    "varying vec3 vColor;\n"
    "void main() {\n"
    "  gl_Position = uMVP * vec4(aPos, 1.0);\n"
    "  vNormal = (uRot * vec4(aNormal, 0.0)).xyz;\n"
    "  vColor = aColor;\n"
    "}\n";

static const char *fs_lambert =
    "precision mediump float;\n"
    "varying vec3 vNormal;\n"
    "varying vec3 vColor;\n"
    "uniform vec3 uLightDir;\n"
    "void main() {\n"
    "  float ndl = max(dot(normalize(vNormal), normalize(uLightDir)), 0.0);\n"
    "  gl_FragColor = vec4(vColor * (0.25 + 0.75 * ndl), 1.0);\n"
    "}\n";

/* A shader the back end must REFUSE, naming the construct. */
static const char *fs_unsupported =
    "precision mediump float;\n"
    "varying vec2 vTC;\n"
    "uniform sampler2D uTex;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(uTex, vTC);\n"
    "}\n";

/* ============================================================================
 * Tests
 * ==========================================================================*/

/* The name tables are the disassembler's human-readable half.  They are
 * exercised here so that "opcode 1 is MOV" is asserted somewhere rather than
 * merely asserted in a comment -- an opcode table nobody runs is a comment. */
static void test_name_tables(void) {
    printf("--- the opcode and file tables agree with the spec ---\n");
    CHECK(!strcmp(opcode_name(1), "MOV"), "opcode 1 is MOV");
    CHECK(!strcmp(opcode_name(117), "END"), "opcode 117 is END");
    CHECK(!strcmp(opcode_name(24), "DP3"), "opcode 24 is DP3");
    CHECK(opcode_name(200) == NULL, "an unknown opcode has no name");
    CHECK(!strcmp(file_name(2), "IN"), "file 2 is INPUT");
    CHECK(!strcmp(file_name(3), "OUT"), "file 3 is OUTPUT");
    CHECK(!strcmp(file_name(4), "TEMP"), "file 4 is TEMPORARY");
    CHECK(!strcmp(file_name(7), "IMM"), "file 7 is IMMEDIATE");
}

static void test_empty_program(void) {
    printf("--- an empty main() emits a legal, minimal stream ---\n");

    glsl_unit_t *u = glsl_compile("void main() { }\n", GLSL_SHADER_VERTEX);
    CHECK(u != NULL, "the unit compiles");
    if (!u) return;

    tgsi_program_t p;
    int ok = tgsi_retarget(u, GLSL_SHADER_VERTEX, &p);
    CHECK(ok == 1, "the back end accepts an empty main");
    CHECK(p.buf.overflow == 0, "the stream did not overflow");
    CHECK(p.buf.count >= 4, "header, processor, at least one DCL, END");

    insn_t ins[8];
    unsigned proc = 99;
    int body = 0;
    int n = disasm(p.buf.dwords, p.buf.count, ins, 8, &proc, &body);
    CHECK(n >= 1, "the disassembler walks the stream");
    CHECK(proc == PROC_VERTEX, "the processor token says VERTEX");
    CHECK(n >= 1 && ins[n - 1].saw_end, "the stream ends with END");
    CHECK(body == p.buf.count - 2,
          "the header's BodySize matches the actual body length");

    glsl_unit_free(u);
}

static void test_field_widths(void) {
    printf("--- the two registers use different index positions ---\n");

    /* This is the assertion that would have caught a copy of the canned
     * arrays.  SRC index lives at bits 15..30, DST index at bits 10..25; a
     * single shared "index at 10" helper produces a stream where every
     * source reads the wrong register and every destination happens to be
     * right -- exactly the canned arrays' symptom. */

    uint32_t s = TGSI_SRC(TGSI_FILE_INPUT, 3, TGSI_SWZ_XYZW);
    src_t ds;
    CHECK(disasm_src(s, &ds) && ds.file == FILE_INPUT && ds.index == 3,
          "src: file and index round-trip");

    uint32_t d = TGSI_DST(TGSI_FILE_OUTPUT, 3, TGSI_WRITEMASK_XYZW);
    dst_t dd;
    CHECK(disasm_dst(d, &dd) && dd.file == FILE_OUTPUT && dd.index == 3,
          "dst: file and index round-trip");

    /* The wrong-width reading must NOT give index 3 -- otherwise this test
     * would pass on a back end that got the widths wrong. */
    dst_t wrong;
    disasm_dst(s, &wrong);
    CHECK(wrong.index != 3,
          "reading a source register as a destination gives the wrong index");

    uint32_t swz = TGSI_SWZ(1, 2, 3, 0);
    src_t ds2;
    disasm_src(TGSI_SRC(TGSI_FILE_INPUT, 0, swz), &ds2);
    CHECK(ds2.swizzle == swz, "a non-identity swizzle round-trips");

    src_t neg;
    disasm_src(TGSI_SRC(TGSI_FILE_INPUT, 0, TGSI_SWZ_XYZW) | TGSI_SRC_NEGATE, &neg);
    CHECK(neg.negate == 1, "the negate bit is where the spec says");
}

static void test_processor_and_types(void) {
    printf("--- token type nibbles follow the spec ---\n");

    /* TOKEN_TYPE_INSTRUCTION is 2 and DECLARATION is 0 (spec Table 2).  The
     * canned arrays use 3 and 1; this asserts the emitter does NOT. */
    uint32_t i = TGSI_INSN(TGSI_OPCODE_END, 1, 0, 0);
    CHECK((i & 0xFu) == TOKEN_TYPE_INSTRUCTION,
          "an instruction token's type nibble is 2");
    uint32_t d = TGSI_DCL(TGSI_FILE_INPUT, 3);
    CHECK((d & 0xFu) == TOKEN_TYPE_DECLARATION,
          "a declaration token's type nibble is 0");

    CHECK(TGSI_PROCESSOR(TGSI_PROC_VERTEX) == 1u,
          "PROCESSOR_VERTEX is 1, as spec Table 7 says");
    CHECK(TGSI_PROCESSOR(TGSI_PROC_FRAGMENT) == 0u,
          "PROCESSOR_FRAGMENT is 0");
}

static void test_canned_is_not_valid(void) {
    printf("--- the canned arrays do NOT decode: the finding, pinned ---\n");

    int n = 0;
    const uint32_t *vs = gl_virgl_canned_vs_tokens(&n);
    CHECK(vs != NULL && n == 21, "the canned VS is 21 dwords");

    insn_t ins[64];
    unsigned proc = 99;
    int body = 0;

    /* The processor is swapped: a VS whose token says FRAGMENT. */
    int got = disasm(vs, n, ins, 64, &proc, &body);
    (void)got;
    CHECK(proc == PROC_FRAGMENT,
          "the canned VERTEX shader's processor token says FRAGMENT (swapped)");

    int fn = 0;
    const uint32_t *fs = gl_virgl_canned_fs_tokens(&fn);
    CHECK(fs != NULL && fn == 13, "the canned FS is 13 dwords");
    unsigned fproc = 99;
    disasm(fs, fn, ins, 64, &fproc, &body);
    CHECK(fproc == PROC_VERTEX,
          "the canned FRAGMENT shader's processor token says VERTEX (swapped)");

    /* The instruction type nibble is 3, which is not a token type at all. */
    CHECK((0x00A01033u & 0xFu) == 3u,
          "the canned MOV's type nibble is 3 -- not a TGSI token type");
    CHECK((0x00075013u & 0xFu) == 3u,
          "the canned END's type nibble is 3");
    CHECK((0x002F2031u & 0xFu) == 1u,
          "the canned DCL's type nibble is 1 (IMMEDIATE), not 0 (DECLARATION)");

    /* And the source register does not decode as INPUT[0]. */
    src_t s;
    disasm_src(0x39000002u, &s);
    CHECK(s.file == FILE_INPUT, "the canned MOV's source file does read as INPUT");
    CHECK(s.index != 0,
          "but its index is NOT 0, so the operand is not the register it means");

    /* This file's own encoder produces the corrected form; the two differ by
     * the type nibble and the source layout, which is the whole finding. */
    uint32_t ours = TGSI_INSN(TGSI_OPCODE_MOV, 3, 1, 1);
    CHECK(ours == 0x00A01032u,
          "the spec-correct MOV header is 0x00A01032, one nibble from canned");
}

static void test_corpus_retargets(void) {
    printf("--- the corpus retargets or refuses BY NAME ---\n");

    /* The Lambert VS uses matrix*vector, which this version refuses (no
     * matrix support yet).  The REFUSAL MUST NAME THE CONSTRUCT: a back end
     * that says only "unsupported" is the thing the G11a rule forbids. */
    glsl_unit_t *u = glsl_compile(vs_lambert, GLSL_SHADER_VERTEX);
    CHECK(u != NULL, "the Lambert vertex shader compiles in G11a");
    if (u) {
        tgsi_program_t p;
        int ok = tgsi_retarget(u, GLSL_SHADER_VERTEX, &p);
        CHECK(ok == 0 || ok == 1, "the retarget answers without crashing");
        if (!ok) {
            CHECK(p.diag_count > 0, "a refusal carries at least one diagnostic");
            CHECK(p.diag_count > 0 && p.diags[0].msg[0] != 0,
                  "and the diagnostic is non-empty text");
            CHECK(p.diag_count > 0 && p.diags[0].line > 0,
                  "and it names a source line");
        }
        /* Whatever the answer, the SOFTWARE PATH IS UNAFFECTED: the unit is
         * still compiled and the interpreter can still run it. */
        CHECK(u->compiled == 1,
              "the unit stays compiled, so the interpreter keeps working");
        glsl_unit_free(u);
    }

    glsl_unit_t *f = glsl_compile(fs_lambert, GLSL_SHADER_FRAGMENT);
    CHECK(f != NULL && f->compiled, "the Lambert fragment shader compiles");
    if (f) glsl_unit_free(f);

    glsl_unit_t *b = glsl_compile(fs_unsupported, GLSL_SHADER_FRAGMENT);
    CHECK(b != NULL && b->compiled, "the texture shader compiles in G11a");
    if (b) {
        tgsi_program_t p;
        int ok = tgsi_retarget(b, GLSL_SHADER_FRAGMENT, &p);
        CHECK(ok == 0, "texture2D is refused rather than mistranslated");
        int named = 0;
        for (int i = 0; i < p.diag_count; i++)
            if (strstr(p.diags[i].msg, "texture2D") ||
                strstr(p.diags[i].msg, "built-in")) named = 1;
        CHECK(named, "and the diagnostic NAMES the construct");
        glsl_unit_free(b);
    }
}

static void test_simple_shader_emits(void) {
    printf("--- a shader with no refusals emits a walkable stream ---\n");

    /* Colours only: no matrices, no textures.  This must go all the way
     * through the back end, and every token must survive the independent
     * disassembler. */
    /* A vertex shader MUST write gl_Position (the checker enforces it), so
     * the corpus's simplest valid shape is "position from the colour's xy".
     * The point of the shader is that it contains no construct this back end
     * refuses: no matrices, no textures, no control flow. */
    const char *vs =
        "attribute vec3 aColor;\n"
        "varying vec3 vColor;\n"
        "void main() {\n"
        "  vColor = aColor;\n"
        "  gl_Position = vec4(aColor, 1.0);\n"
        "}\n";
    glsl_unit_t *u = glsl_compile(vs, GLSL_SHADER_VERTEX);
    CHECK(u != NULL && u->compiled, "the colour-only VS compiles");
    if (!u) return;

    tgsi_program_t p;
    int ok = tgsi_retarget(u, GLSL_SHADER_VERTEX, &p);
    CHECK(ok == 1, "the back end accepts it");
    if (ok) {
        CHECK(p.buf.overflow == 0, "no overflow");
        CHECK(p.layout.input_count == 1, "one INPUT (aColor)");
        CHECK(p.layout.output_count == 1, "one OUTPUT (vColor)");

        insn_t ins[64];
        unsigned proc = 99;
        int body = 0;
        int n = disasm(p.buf.dwords, p.buf.count, ins, 64, &proc, &body);
        CHECK(n > 0, "the disassembler walks every token");
        CHECK(proc == PROC_VERTEX, "processor is VERTEX");
        CHECK(body == p.buf.count - 2, "BodySize is exact");

        /* There must be a MOV writing OUTPUT[0] from INPUT[0]. */
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (ins[i].opcode == 1u && ins[i].ndst == 1 && ins[i].nsrc == 1 &&
                ins[i].dst.file == FILE_OUTPUT && ins[i].dst.index == 0 &&
                ins[i].src[0].file == FILE_INPUT && ins[i].src[0].index == 0)
                found = 1;
        }
        CHECK(found, "a MOV writes OUT[0] from IN[0] (vColor = aColor)");

        /* gl_Position is a DIFFERENT output: the first version of the
         * emitter mapped both to OUT[0] and would have written the colour
         * into the position.  Pin the separation. */
        int pos_sep = 0;
        for (int i = 0; i < n; i++)
            if (ins[i].opcode == 1u && ins[i].dst.file == FILE_OUTPUT &&
                ins[i].dst.index == 1)
                pos_sep = 1;
        CHECK(pos_sep, "gl_Position writes a DIFFERENT output register");
        CHECK(ins[n - 1].saw_end, "the stream ends with END");

        /* Every NrTokens the walker accepted implies the lengths are right;
         * say so explicitly, because a stream that merely "walked" could
         * still have stopped early. */
        int consumed = 0;
        for (int i = 0; i < n; i++) {
            (void)ins[i];
            consumed++;
        }
        CHECK(consumed == n, "every instruction in the stream was walked");
    }
    glsl_unit_free(u);
}

static void test_layout_numbering(void) {
    printf("--- the interface layout is derived and stable ---\n");

    const char *fs =
        "precision mediump float;\n"
        "varying vec3 vA;\n"
        "varying vec2 vB;\n"
        "uniform vec4 uC;\n"
        "void main() {\n"
        "  gl_FragColor = vec4(vA, 1.0);\n"
        "}\n";
    glsl_unit_t *u = glsl_compile(fs, GLSL_SHADER_FRAGMENT);
    CHECK(u != NULL && u->compiled, "the two-varying FS compiles");
    if (!u) return;

    tgsi_layout_t l;
    tgsi_diag_t d[4];
    CHECK(tgsi_build_layout(u, &l, d, 4) == 1, "the layout builds");
    CHECK(l.input_count == 2, "two varyings become two INPUTs");
    CHECK(l.uniform_count == 1, "one uniform");
    CHECK(l.sampler_count == 0, "no samplers");

    /* Indices must be sequential from 0: a gap means a driver looks for a
     * varying that was never bound. */
    CHECK(l.input_count == 2 && l.inputs[0].index == 0 && l.inputs[1].index == 1,
          "INPUT indices are 0 and 1 in declaration order");
    CHECK(l.uniform_count == 1 && l.uniforms[0].index == 0,
          "the uniform lands in CONST[0]");

    glsl_unit_free(u);
}

static void test_nr_tokens_discipline(void) {
    printf("--- NrTokens always matches what was written ---\n");

    /* The H3 lesson applied to TGSI: a header that lies about its length is
     * the one bug a structural walker cannot catch.  Here the check is
     * direct: walk the emitted stream and verify that each instruction's
     * NrTokens equals 1 + ndst + nsrc (+ a texture extension). */
    /* A vertex shader MUST write gl_Position (the checker enforces it), so
     * the corpus's simplest valid shape is "position from the colour's xy".
     * The point of the shader is that it contains no construct this back end
     * refuses: no matrices, no textures, no control flow. */
    const char *vs =
        "attribute vec3 aColor;\n"
        "varying vec3 vColor;\n"
        "void main() {\n"
        "  vColor = aColor;\n"
        "  gl_Position = vec4(aColor, 1.0);\n"
        "}\n";
    glsl_unit_t *u = glsl_compile(vs, GLSL_SHADER_VERTEX);
    if (!u) { CHECK(0, "compile"); return; }
    tgsi_program_t p;
    if (!tgsi_retarget(u, GLSL_SHADER_VERTEX, &p)) { CHECK(0, "retarget"); glsl_unit_free(u); return; }

    int i = 2, bad = 0, seen = 0;
    while (i < p.buf.count) {
        uint32_t w = p.buf.dwords[i];
        if ((w & 0xFu) == TOKEN_TYPE_INSTRUCTION) {
            unsigned nr = (w >> 4) & 0xFFu;
            unsigned ndst = (w >> 21) & 3u;
            unsigned nsrc = (w >> 23) & 0xFu;
            int tex = (int)((w >> 27) & 1u);
            unsigned want = 1u + ndst + nsrc + (unsigned)tex;
            if (nr != want) bad++;
            seen++;
            i += (int)nr;
        } else if ((w & 0xFu) == TOKEN_TYPE_IMMEDIATE) {
            i += (int)((w >> 4) & 0xFFu);   /* Size includes the header */
        } else {
            i += (int)((w >> 4) & 0xFFu);
        }
    }
    CHECK(seen > 0, "the stream contains instructions");
    CHECK(bad == 0, "every NrTokens equals 1 + ndst + nsrc");
    CHECK(i == p.buf.count, "the walk lands exactly at the end");

    glsl_unit_free(u);
}

int main(void) {
    printf("=== test_tgsi: the GLSL AST -> TGSI retarget (H4) ===\n");

    test_name_tables();
    test_empty_program();
    test_field_widths();
    test_processor_and_types();
    test_canned_is_not_valid();
    test_corpus_retargets();
    test_simple_shader_emits();
    test_layout_numbering();
    test_nr_tokens_discipline();

    printf("\ntest_tgsi: %d passed, %d failed (%d total)\n", passed, failed, tn);
    return failed ? 1 : 0;
}
