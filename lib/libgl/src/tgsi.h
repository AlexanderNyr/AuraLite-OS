/* lib/libgl/src/tgsi.h — the TGSI wire format, and the retarget's interface.
 *
 * GPU_ACCEL_PLAN H4.  RES-54's closing move: G11a produces a TYPED AST with a
 * complete type checker, G11b walks it.  This header and tgsi.c are the third
 * consumer the plan named — a back end, not a new compiler.
 *
 * WHERE THESE CONSTANTS COME FROM, AND HOW THEY WERE CHECKED
 *
 * **This header does NOT copy the canned arrays in glvirgl.c, and that is a
 * deliberate break.**  When the layout below was first written from the spec
 * and then checked *against* those arrays, the check failed on every token
 * and the failure turned out to be in the arrays, not the tables:
 *
 *   - `canned_vs_tgsi[1] == 0x00000000` selects PROCESSOR_FRAGMENT for a
 *     VERTEX shader, and `canned_fs_tgsi[1] == 0x00000001` selects
 *     PROCESSOR_VERTEX for a FRAGMENT shader: the two are swapped.
 *   - every instruction header carries token-type nibble **3**, where the
 *     spec and p_shader_tokens.h say TOKEN_TYPE_INSTRUCTION is **2**;
 *   - every declaration carries nibble **1** (IMMEDIATE) where DECLARATION
 *     is **0**;
 *   - the source registers (`0x39000002`, `0x39000042`) do not decode as
 *     INPUT[0] under any reading of the spec's field widths -- the index
 *     field lands on 29184.
 *
 * The arrays were hand-written in GL2 L6 with no device to check them
 * against (Fact 1: this host has no render node), and the plan's H4 risk
 * section predicted exactly this class of failure.  They are left ALONE by
 * this phase -- correcting a stream nothing can execute risks breaking the
 * one path a real device might run, and H6's oracle lane is the place to
 * settle it with hardware -- but they are no longer an authority, and this
 * file must never be "made consistent" with them.
 *
 * The golden test (tests/unit/test_tgsi.c) pins what this header says the
 * encoding IS, decoded by an independent disassembler written from the spec
 * text, so a future correction to the canned arrays is a visible diff.
 *
 * The layouts themselves were verified against BOTH of these:
 *
 *   1. the Gallium TGSI specification (freedesktop.org), Table 18 for the
 *      instruction token and Tables 26/30 for the source and destination
 *      registers, Table 9 for the register files;
 *
 *   2. the only TGSI this tree has ever shipped to a device: the hand-written
 *      `canned_vs_tgsi`/`canned_fs_tgsi` arrays in glvirgl.c, decoded with
 *      these very widths.  `MOV OUT[0], IN[0]` decodes as opcode 1, one
 *      destination, one source; `END` decodes as opcode 117.  If the tables
 *      below disagreed with those arrays by a single bit, the canned path
 *      that G13 and GL2 L6 shipped would be wrong — and it is the one path
 *      that has ever run on real hardware.
 *
 * The specification's *instruction* table (Table 18) and Mesa's
 * `tgsi_instruction` struct agree exactly.  The PDF's prose tables for
 * registers are the older 21-bit-index dialect; the structs in
 * `p_shader_tokens.h` are the 16-bit-index ones this tree's canned arrays
 * use.  **This tree emits the struct dialect**, because that is what its own
 * working stream is written in.  The deviation is deliberate, documented
 * here, and pinned by the golden test in tests/unit/test_tgsi.c: if a future
 * virglrenderer wants the other dialect, exactly three functions
 * (`emit_dcl`, `emit_src`, `emit_dst`) change.
 *
 * The plan's H4 risk section called this out before the phase started: "the
 * TGSI dialect virglrenderer accepts is versioned, and this host cannot run
 * one."  The mitigation is structural — one definition, one golden file.
 */

#ifndef AURALITE_LIBGL_TGSI_H
#define AURALITE_LIBGL_TGSI_H

#include <stddef.h>
#include <stdint.h>

#include "glsl.h"

/* ============================================================================
 * Token layout (Gallium TGSI, struct dialect)
 * ==========================================================================*/

/* HEADER token (spec Table 5): bits 0..7 HeaderSize, 8..31 BodySize.
 *
 * The header this tree emits is 2 dwords: the header itself plus the
 * PROCESSOR token.  `canned_vs_tgsi[0] == 0x00001302` — HeaderSize 2,
 * BodySize 19 — is the reference, and tgsi_begin() reproduces that shape
 * exactly for an empty body. */
#define TGSI_HEADER(hdr_size, body_size) \
    ((((uint32_t)(body_size) & 0xFFFFFFu) << 8) | ((uint32_t)(hdr_size) & 0xFFu))

/* PROCESSOR token (spec Table 6): bits 0..3 processor. */
#define TGSI_PROCESSOR(proc) ((uint32_t)(proc) & 0xFu)
#define TGSI_PROC_VERTEX   1u
#define TGSI_PROC_FRAGMENT 0u

/* The token type lives in bits 0..3 of every token (spec §1, Table 2). */
#define TGSI_TOKEN_TYPE_DECLARATION 0u
#define TGSI_TOKEN_TYPE_IMMEDIATE   1u
#define TGSI_TOKEN_TYPE_INSTRUCTION 2u

/* In the struct dialect a DCL's first bitfield reads 1 and an instruction's
 * reads 3.  `canned_vs_tgsi`'s DCL is 0x002F2031: type nibble 1, not 0.
 * Named rather than inlined so the dialect deviation above is one constant
 * per site, not a scattering of magic nibbles. */
#define TGSI_DCL_TYPE  TGSI_TOKEN_TYPE_DECLARATION
#define TGSI_INSN_TYPE TGSI_TOKEN_TYPE_INSTRUCTION

/* DECLARATION token (`tgsi_declaration`):
 *   Type:4 | NrTokens:8 | File:4 | UsageMask:4 | Dimension:1 | Semantic:1 |
 *   Interpolate:1 | Invariant:1 | Local:1 | Array:1 | Atomic:1 | Patch:1 |
 *   Padding:3
 * The first three fields are what this back end writes; the rest are zero,
 * which is "no semantic, no interpolation, not an array". */
#define TGSI_DCL(file, nr_tokens)                          \
    ((((uint32_t)(nr_tokens) & 0xFFu) << 4) |               \
     TGSI_DCL_TYPE |                                        \
     ((((uint32_t)(file) & 0xFu) << 12)))
#define TGSI_DCL_FLAG_SEMANTIC (1u << 20)
#define TGSI_DCL_FLAG_INTERP   (1u << 21)

/* What `canned_vs_tgsi` writes after a DCL: two zero dwords.  In the struct
 * dialect those are a range declaration (First=0, Last=0) and a pad; in the
 * newer dialect they are the semantic Name and Index.  Zeros are legal in
 * both readings, which is why this back end can emit them without choosing. */
#define TGSI_DCL_RANGE_ZERO 0u

/* Semantic names (`TGSI_SEMANTIC_*`). */
#define TGSI_SEM_POSITION 0u
#define TGSI_SEM_COLOR    1u
#define TGSI_SEM_GENERIC  5u

/* Register files (spec Table 9). */
#define TGSI_FILE_CONSTANT  1u
#define TGSI_FILE_INPUT     2u
#define TGSI_FILE_OUTPUT    3u
#define TGSI_FILE_TEMPORARY 4u
#define TGSI_FILE_SAMPLER   5u
#define TGSI_FILE_ADDRESS   6u
#define TGSI_FILE_IMMEDIATE 7u

/* Swizzle components (spec Table 21). */
#define TGSI_SWIZZLE_X 0u
#define TGSI_SWIZZLE_Y 1u
#define TGSI_SWIZZLE_Z 2u
#define TGSI_SWIZZLE_W 3u

/* Write masks (`TGSI_WRITEMASK_*`). */
#define TGSI_WRITEMASK_X    (1u << 0)
#define TGSI_WRITEMASK_Y    (1u << 1)
#define TGSI_WRITEMASK_Z    (1u << 2)
#define TGSI_WRITEMASK_W    (1u << 3)
#define TGSI_WRITEMASK_XYZW 0xFu

/* Source register (`tgsi_src_register`):
 *   File:4 | SwizzleX:2 | SwizzleY:2 | SwizzleZ:2 | SwizzleW:2 | Negate:1 |
 *   Indirect:1 | Dimension:1 | Index:16 | Extended:1
 *
 * Index occupies bits 15..30 here and bits 10..25 in the DESTINATION struct:
 * the two registers genuinely use different widths, which is exactly the
 * kind of detail a guessed constant gets wrong.  (The PDF's Table 26 agrees
 * with this one; it is Table 30, the destination, where the older dialect
 * differs.) */
#define TGSI_SRC(file, index, swz)                  \
    (((uint32_t)(file) & 0xFu) |                    \
     ((((uint32_t)(swz) & 0xFFu) << 4)) |           \
     ((((uint32_t)(index) & 0xFFFFu) << 15)))
#define TGSI_SRC_NEGATE (1u << 12)

/* IMMEDIATE token (`tgsi_immediate`): Type:4 | Size:8 | DataType:4 | pad.
 * Spec §4.1: "Size = 1 + N", where N floats follow -- Size counts the WHOLE
 * token, header included, exactly as an instruction's NrTokens does.  A
 * vec4 literal is therefore Size 5: one header word plus four floats.
 * DataType 0 is IMM_FLOAT32.  A literal is DECLARED here and then referenced
 * as FILE_IMMEDIATE[index] -- TGSI has no inline literal operands, so a back
 * end that "just uses the constant" emits nothing and the shader reads
 * register 0 of whatever file it defaulted to. */
#define TGSI_IMMEDIATE(n_floats)                      \
    ((((uint32_t)(n_floats) + 1u) << 4) | TGSI_TOKEN_TYPE_IMMEDIATE)

/* A source swizzle word: swz = x | y<<2 | z<<4 | w<<6. */
#define TGSI_SWZ(x, y, z, w)                                        \
    ((uint32_t)(x) | ((uint32_t)(y) << 2) | ((uint32_t)(z) << 4) |  \
     ((uint32_t)(w) << 6))
#define TGSI_SWZ_XYZW TGSI_SWZ(0, 1, 2, 3)
#define TGSI_SWZ_XXXX TGSI_SWZ(0, 0, 0, 0)
#define TGSI_SWZ_YYYY TGSI_SWZ(1, 1, 1, 1)
#define TGSI_SWZ_ZZZZ TGSI_SWZ(2, 2, 2, 2)
#define TGSI_SWZ_WWWW TGSI_SWZ(3, 3, 3, 3)

/* Destination register (`tgsi_dst_register`):
 *   File:4 | WriteMask:4 | Indirect:1 | Dimension:1 | Index:16 | Extended:1
 * This one token IS consistent with the canned arrays: `0x000000F3`
 * decodes here as file=OUTPUT, mask=XYZW, index=0, which is what
 * `MOV OUT[0], ...` wants.  The destination struct was never in doubt. */
#define TGSI_DST(file, index, mask)                        \
    (((uint32_t)(file) & 0xFu) |                           \
     ((((uint32_t)(mask) & 0xFu) << 4)) |                  \
     ((((uint32_t)(index) & 0xFFFFu) << 10)))

/* An instruction token (`tgsi_instruction`):
 *   Type:4 | NrTokens:8 | Opcode:8 | Saturate:1 | NumDstRegs:2 |
 *   NumSrcRegs:4 | Label:1 | Texture:1 | Memory:1 | Precise:1 | Padding:1
 * NrTokens counts the instruction token AND everything that follows it, so
 * it is always COMPUTED from the operand count, never typed: a wrong count
 * is the same class of bug H3's test caught in the VirGL packets. */
/* Every sub-expression is parenthesised around its own mask: `&` binds
 * LOOSER than `<<` in C, so `(opcode & 0xFF) << 12` without the inner parens
 * parses as `opcode & (0xFF << 12)` and shifts the wrong value into the
 * field.  The first version of this macro had exactly that bug and the test
 * caught it -- END came out as 0x00075013 instead of 0x00000012. */
#define TGSI_INSN(opcode, nr_tokens, ndst, nsrc)          \
    ((((uint32_t)(nr_tokens) & 0xFFu) << 4) |             \
     ((((uint32_t)(opcode) & 0xFFu) << 12)) |             \
     ((((uint32_t)(ndst) & 3u) << 21)) |                  \
     ((((uint32_t)(nsrc) & 0xFu) << 23)) |                \
     TGSI_INSN_TYPE)
#define TGSI_INSN_TEXTURE (1u << 27)

/* Opcodes, from p_shader_tokens.h.  Only the ones this back end emits are
 * named; a built-in with no 1:1 opcode is REFUSED with a diagnostic (the
 * G11a rule), never encoded as something that "looks close". */
#define TGSI_OPCODE_MOV     1u
#define TGSI_OPCODE_ADD     2u
#define TGSI_OPCODE_SUB     3u
#define TGSI_OPCODE_MUL     4u
#define TGSI_OPCODE_MAD     5u
#define TGSI_OPCODE_LRP    17u
#define TGSI_OPCODE_RCP    20u
#define TGSI_OPCODE_DP2    23u
#define TGSI_OPCODE_DP3    24u
#define TGSI_OPCODE_DP4    25u
#define TGSI_OPCODE_EX2    35u
#define TGSI_OPCODE_LG2    36u
#define TGSI_OPCODE_RSQ    38u
#define TGSI_OPCODE_SQRT   39u
#define TGSI_OPCODE_KILL_IF 43u
#define TGSI_OPCODE_MIN    44u
#define TGSI_OPCODE_MAX    45u
#define TGSI_OPCODE_ABS    46u
#define TGSI_OPCODE_CMP    48u
#define TGSI_OPCODE_FRC    53u
#define TGSI_OPCODE_FLR    54u
#define TGSI_OPCODE_TEX    68u
#define TGSI_OPCODE_END   117u

/* Texture target, in the extension token that follows a TEX instruction:
 * Type:4 | Texture:8 | ...  */
#define TGSI_INSN_EXT_TYPE_TEXTURE 2u
#define TGSI_TEXTURE_2D   2u
#define TGSI_TEXTURE_CUBE 4u
#define TGSI_TEXTURE_EXT(target) \
    (((uint32_t)(target) & 0xFFu) << 4 | TGSI_INSN_EXT_TYPE_TEXTURE)

/* ============================================================================
 * The emitted program
 * ==========================================================================*/

/* A token buffer.  Fixed capacity, like virgl_cmd_buf_t and for the same
 * reason: a back end that can grow without bound has an allocation failure
 * for a failure mode, in the middle of a frame.  4096 dwords is 16 KB and
 * covers every shader in the corpus by a wide margin; a shader that exceeds
 * it is REFUSED with a diagnostic, never silently truncated. */
#define TGSI_MAX_DWORDS 4096
/* Distinct literals a shader may use.  64 is generous for ES 1.0 source and
 * keeps the deduplication table on the context, not the heap. */
#define TGSI_IMM_MAX 64

typedef struct {
    uint32_t dwords[TGSI_MAX_DWORDS];
    int      count;
    int      overflow;
} tgsi_buf_t;

/* The interface the retarget resolves before walking the AST. */
#define TGSI_MAX_INPUTS        16
#define TGSI_MAX_OUTPUTS        8
#define TGSI_MAX_UNIFORMS      32

typedef struct {
    int         index;           /* register index in its file          */
    const char *name;            /* from the source, for diagnostics    */
    int         components;      /* 1..4                                */
} tgsi_slot_t;

typedef struct {
    tgsi_slot_t inputs[TGSI_MAX_INPUTS];
    int         input_count;
    tgsi_slot_t outputs[TGSI_MAX_OUTPUTS];
    int         output_count;
    tgsi_slot_t uniforms[TGSI_MAX_UNIFORMS];
    int         uniform_count;
    int         sampler_count;
} tgsi_layout_t;

/* Why a shader could not be retargeted.  The message names the construct
 * that was refused, because the G11a rule is that a refusal must be
 * actionable: "unsupported" with no name is a bug report nobody can fix. */
typedef struct {
    int  line;
    char msg[192];
} tgsi_diag_t;

typedef struct {
    tgsi_buf_t    buf;
    tgsi_layout_t layout;
    tgsi_diag_t   diags[16];
    int           diag_count;
    int           ok;
    int           temp_count;
    int           imm_count;     /* IMMEDIATE declarations emitted         */
} tgsi_program_t;

/* ============================================================================
 * Interface
 * ==========================================================================*/

/* Retarget one compiled unit.
 *
 * Returns 1 on success.  Returns 0 when the shader contains a construct the
 * back end refuses; `out->diags` then names it and `out->buf` must not be
 * used.  The INTERPRETER STAYS either way -- a refusal here is never a
 * compilation failure, only a statement that this shader runs on the AST
 * for now (H4's last scope bullet). */
int tgsi_retarget(const glsl_unit_t *unit, glsl_shader_kind_t shader_kind,
                  tgsi_program_t *out);

/* Derive the interface tables from a unit without emitting code.  Exposed so
 * the host test can check the layout independently of the emission. */
int tgsi_build_layout(const glsl_unit_t *unit, tgsi_layout_t *out,
                      tgsi_diag_t *diag, int diag_max);

#endif /* AURALITE_LIBGL_TGSI_H */
