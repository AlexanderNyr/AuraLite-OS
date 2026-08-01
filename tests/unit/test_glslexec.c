/*
 * test_glslexec.c — host-side unit tests for the GLSL execution engine (G11b).
 *
 * Every test compiles a real shader, runs it, and checks the numbers that
 * came out.  There is no rasterizer and no GL context: the shader's view of
 * the outside world is a glsl_env_t supplying uniforms and a synthetic
 * sampler, which is exactly the seam G11c will replace with the real
 * pipeline.
 *
 * The assertions are on VALUES, to a tolerance, because that is the engine's
 * contract.  "It ran without crashing" is not the property under test — a
 * shader that computes the wrong colour is a bug the application can never
 * diagnose, so the numbers are checked against what the specification says
 * they must be.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>

#include "glsl.h"

static int tn = 0, passed = 0, failed = 0;

#define CHECK(cond, name) do {                          \
    tn++;                                               \
    if (cond) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", (name)); }  \
} while (0)

/* ============================================================================
 * A test environment
 *
 * Uniforms are a small name/value table; the sampler returns a colour derived
 * from the coordinate so a test can tell which texel was asked for.
 * ==========================================================================*/

typedef struct {
    const char *name;
    int         n;
    float       v[16];
} uniform_t;

typedef struct {
    uniform_t uniforms[16];
    int       uniform_count;

    float     out_color[4];      /* gl_FragColor */
    float     out_position[4];   /* gl_Position  */
    int       wrote_color;
    int       wrote_position;

    /* Last sampler call, so a test can assert on what was requested. */
    int       last_unit;
    int       last_is_cube;
    float     last_coord[3];
    int       sample_count;

    glsl_env_t env;
} testenv_t;

static int te_read(glsl_env_t *env, const char *name, glsl_value_t *out) {
    testenv_t *te = (testenv_t *)env->user;

    /* A partial write -- gl_Position.xy = ... -- reads the current value back
     * first so the untouched components survive.  A test environment that
     * only ever wrote would report zeros for them and the merge would look
     * broken when it is not, so the outputs are readable as well. */
    if (strcmp(name, "gl_Position") == 0 && te->wrote_position) {
        memcpy(out->v, te->out_position, sizeof te->out_position);
        return 1;
    }
    if (strcmp(name, "gl_FragColor") == 0 && te->wrote_color) {
        memcpy(out->v, te->out_color, sizeof te->out_color);
        return 1;
    }

    for (int i = 0; i < te->uniform_count; i++) {
        if (strcmp(te->uniforms[i].name, name) == 0) {
            for (int k = 0; k < te->uniforms[i].n && k < 16; k++) {
                out->v[k] = te->uniforms[i].v[k];
            }
            return 1;
        }
    }
    return 0;                    /* unset reads as zero, as in GL */
}

static void te_write(glsl_env_t *env, const char *name,
                     const glsl_value_t *val) {
    testenv_t *te = (testenv_t *)env->user;
    if (strcmp(name, "gl_FragColor") == 0) {
        memcpy(te->out_color, val->v, sizeof te->out_color);
        te->wrote_color = 1;
    } else if (strcmp(name, "gl_Position") == 0) {
        memcpy(te->out_position, val->v, sizeof te->out_position);
        te->wrote_position = 1;
    }
}

/* A synthetic texture: the returned colour encodes the coordinate, so a test
 * can prove which texel a shader asked for rather than merely that it asked. */
static void te_sample(glsl_env_t *env, int unit, int is_cube,
                      const float *coord, int ncoord, float *rgba) {
    testenv_t *te = (testenv_t *)env->user;
    te->last_unit    = unit;
    te->last_is_cube = is_cube;
    te->sample_count++;
    for (int i = 0; i < 3; i++) te->last_coord[i] = i < ncoord ? coord[i] : 0.0f;

    rgba[0] = coord[0];
    rgba[1] = ncoord > 1 ? coord[1] : 0.0f;
    rgba[2] = ncoord > 2 ? coord[2] : (float)unit;
    rgba[3] = 1.0f;
}

static void te_init(testenv_t *te) {
    memset(te, 0, sizeof *te);
    te->env.read_var  = te_read;
    te->env.write_var = te_write;
    te->env.sample    = te_sample;
    te->env.user      = te;
}

static void te_uniform(testenv_t *te, const char *name, int n, ...) {
    if (te->uniform_count >= 16) return;
    uniform_t *u = &te->uniforms[te->uniform_count++];
    u->name = name;
    u->n = n;
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n && i < 16; i++) u->v[i] = (float)va_arg(ap, double);
    va_end(ap);
}

static int near_f(float a, float b) {
    float d = a - b;
    if (d < 0.0f) d = -d;
    /* A relative tolerance for large values, absolute for small ones: a
     * shader computing 1e6 cannot be held to 1e-5 absolute. */
    float scale = fabsf(b) > 1.0f ? fabsf(b) : 1.0f;
    return d <= 1e-4f * scale;
}

/* ============================================================================
 * Drivers
 * ==========================================================================*/

/* Run a fragment shader body and assert gl_FragColor. */
static void frag(const char *decls, const char *body,
                 float r, float g, float b, float a,
                 const char *name) {
    char src[4096];
    snprintf(src, sizeof src, "%s\nvoid main() {\n%s\n}\n", decls, body);

    tn++;
    glsl_unit_t *u = glsl_compile(src, GLSL_SHADER_FRAGMENT);
    if (!u || !u->compiled) {
        failed++;
        printf("  FAIL: %s — did not compile\n", name);
        if (u) printf("        %s", glsl_unit_log(u));
        glsl_unit_free(u);
        return;
    }

    testenv_t te;
    te_init(&te);
    glsl_run_status_t st = glsl_run(u, &te.env);

    if (st != GLSL_RUN_OK) {
        failed++;
        printf("  FAIL: %s — run status %d\n", name, (int)st);
        printf("        %s", glsl_unit_log(u));
    } else if (!near_f(te.out_color[0], r) || !near_f(te.out_color[1], g) ||
               !near_f(te.out_color[2], b) || !near_f(te.out_color[3], a)) {
        failed++;
        printf("  FAIL: %s\n        want (%g %g %g %g)\n"
               "        got  (%g %g %g %g)\n",
               name, r, g, b, a,
               te.out_color[0], te.out_color[1],
               te.out_color[2], te.out_color[3]);
    } else {
        passed++;
    }
    glsl_unit_free(u);
}

/* Same, but with an environment the caller has already populated. */
static void frag_env(testenv_t *te, const char *src,
                     float r, float g, float b, float a, const char *name) {
    tn++;
    glsl_unit_t *u = glsl_compile(src, GLSL_SHADER_FRAGMENT);
    if (!u || !u->compiled) {
        failed++;
        printf("  FAIL: %s — did not compile\n", name);
        if (u) printf("        %s", glsl_unit_log(u));
        glsl_unit_free(u);
        return;
    }
    glsl_run_status_t st = glsl_run(u, &te->env);
    if (st != GLSL_RUN_OK) {
        failed++;
        printf("  FAIL: %s — run status %d\n", name, (int)st);
        printf("        %s", glsl_unit_log(u));
    } else if (!near_f(te->out_color[0], r) || !near_f(te->out_color[1], g) ||
               !near_f(te->out_color[2], b) || !near_f(te->out_color[3], a)) {
        failed++;
        printf("  FAIL: %s\n        want (%g %g %g %g)\n"
               "        got  (%g %g %g %g)\n",
               name, r, g, b, a,
               te->out_color[0], te->out_color[1],
               te->out_color[2], te->out_color[3]);
    } else {
        passed++;
    }
    glsl_unit_free(u);
}

/* A single float, broadcast into all four channels by the shader. */
static void frag1(const char *decls, const char *expr, float want,
                  const char *name) {
    char body[2048];
    snprintf(body, sizeof body, "  gl_FragColor = vec4(%s);", expr);
    frag(decls, body, want, want, want, want, name);
}

/* ============================================================================
 * Arithmetic
 * ==========================================================================*/

static void test_arithmetic(void) {
    printf("--- arithmetic ---\n");

    frag1("", "1.0 + 2.0", 3.0f, "float addition");
    frag1("", "7.0 - 2.5", 4.5f, "float subtraction");
    frag1("", "3.0 * 4.0", 12.0f, "float multiplication");
    frag1("", "10.0 / 4.0", 2.5f, "float division");
    frag1("", "-(3.0)", -3.0f, "unary negation");

    /* Integer division truncates towards zero — the classic surprise. */
    frag("", "  int a = 7 / 2;\n  int b = -7 / 2;\n"
             "  gl_FragColor = vec4(float(a), float(b), 0.0, 1.0);",
         3.0f, -3.0f, 0.0f, 1.0f, "integer division truncates towards zero");

    /* Division by zero is undefined in GLSL; this implementation returns 0
     * rather than an infinity, so a bad fragment cannot poison a blend. */
    frag1("", "1.0 / 0.0", 0.0f, "division by zero yields zero, not infinity");

    frag("", "  vec3 a = vec3(1.0, 2.0, 3.0);\n"
             "  vec3 b = vec3(10.0, 20.0, 30.0);\n"
             "  vec3 c = a + b;\n"
             "  gl_FragColor = vec4(c, 1.0);",
         11.0f, 22.0f, 33.0f, 1.0f, "vector addition is component-wise");

    frag("", "  vec3 v = vec3(1.0, 2.0, 3.0) * 2.0;\n"
             "  gl_FragColor = vec4(v, 1.0);",
         2.0f, 4.0f, 6.0f, 1.0f, "vector times scalar broadcasts");

    frag("", "  vec3 v = 2.0 * vec3(1.0, 2.0, 3.0);\n"
             "  gl_FragColor = vec4(v, 1.0);",
         2.0f, 4.0f, 6.0f, 1.0f, "scalar times vector broadcasts");

    /* Precedence, verified by the value rather than by the parse. */
    frag1("", "1.0 + 2.0 * 3.0", 7.0f, "multiplication binds tighter than +");
    frag1("", "(1.0 + 2.0) * 3.0", 9.0f, "parentheses override precedence");
    frag1("", "10.0 - 2.0 - 3.0", 5.0f, "subtraction is left-associative");
}

static void test_comparisons(void) {
    printf("--- comparisons and logic ---\n");

    frag1("", "1.0 < 2.0 ? 1.0 : 0.0", 1.0f, "less-than");
    frag1("", "2.0 <= 2.0 ? 1.0 : 0.0", 1.0f, "less-or-equal");
    frag1("", "3.0 > 4.0 ? 1.0 : 0.0", 0.0f, "greater-than");

    frag1("", "vec3(1.0) == vec3(1.0) ? 1.0 : 0.0", 1.0f,
          "vector equality compares every component");
    frag1("", "vec3(1.0, 2.0, 3.0) == vec3(1.0, 2.0, 4.0) ? 1.0 : 0.0", 0.0f,
          "vector equality detects a single differing component");

    frag1("", "(true && false) ? 1.0 : 0.0", 0.0f, "logical and");
    frag1("", "(true || false) ? 1.0 : 0.0", 1.0f, "logical or");
    frag1("", "(true ^^ true) ? 1.0 : 0.0", 0.0f, "logical xor");
    frag1("", "(!false) ? 1.0 : 0.0", 1.0f, "logical not");

    /* Short-circuit evaluation is observable: the right operand of a decided
     * && must not run, because it may have side effects. */
    frag("", "  float side = 0.0;\n"
             "  bool b = false && (side = 1.0) > 0.0;\n"
             "  gl_FragColor = vec4(side);",
         0.0f, 0.0f, 0.0f, 0.0f, "&& short-circuits and skips side effects");

    frag("", "  float side = 0.0;\n"
             "  bool b = true || (side = 1.0) > 0.0;\n"
             "  gl_FragColor = vec4(side);",
         0.0f, 0.0f, 0.0f, 0.0f, "|| short-circuits and skips side effects");

    /* The untaken branch of ?: must not be evaluated either. */
    frag("", "  float side = 0.0;\n"
             "  float r = true ? 1.0 : (side = 5.0);\n"
             "  gl_FragColor = vec4(side);",
         0.0f, 0.0f, 0.0f, 0.0f, "?: evaluates only the taken branch");
}

/* ============================================================================
 * Vectors, swizzles and matrices
 * ==========================================================================*/

static void test_swizzles(void) {
    printf("--- swizzles ---\n");

    frag("", "  vec4 v = vec4(1.0, 2.0, 3.0, 4.0);\n"
             "  gl_FragColor = v.wzyx;",
         4.0f, 3.0f, 2.0f, 1.0f, "a reversing swizzle");

    frag("", "  vec4 v = vec4(1.0, 2.0, 3.0, 4.0);\n"
             "  gl_FragColor = vec4(v.xy, v.zw);",
         1.0f, 2.0f, 3.0f, 4.0f, "swizzles compose in a constructor");

    frag("", "  vec4 v = vec4(1.0, 2.0, 3.0, 4.0);\n"
             "  gl_FragColor = v.xxyy;",
         1.0f, 1.0f, 2.0f, 2.0f, "a repeating swizzle reads fine");

    frag("", "  vec4 v = vec4(9.0);\n  v.xz = vec2(1.0, 3.0);\n"
             "  gl_FragColor = v;",
         1.0f, 9.0f, 3.0f, 9.0f, "a swizzled write touches only its components");

    frag("", "  vec4 v = vec4(0.0);\n  v.w = 7.0;\n  gl_FragColor = v;",
         0.0f, 0.0f, 0.0f, 7.0f, "a single-component write");

    /* Out-of-order swizzled writes are where an implementation that builds
     * the component map wrongly falls over. */
    frag("", "  vec4 v = vec4(0.0);\n  v.zyx = vec3(1.0, 2.0, 3.0);\n"
             "  gl_FragColor = v;",
         3.0f, 2.0f, 1.0f, 0.0f, "an out-of-order swizzled write");

    frag("", "  vec4 v = vec4(1.0, 2.0, 3.0, 4.0);\n"
             "  gl_FragColor = vec4(v.rgb, v.a);",
         1.0f, 2.0f, 3.0f, 4.0f, "the rgba alphabet reads the same components");

    frag("", "  vec4 v = vec4(1.0, 2.0, 3.0, 4.0);\n"
             "  gl_FragColor = vec4(v.stp, v.q);",
         1.0f, 2.0f, 3.0f, 4.0f, "the stpq alphabet reads the same components");

    /* A swizzle of a swizzle has to compose the maps, not apply them twice
     * against the original. */
    frag1("", "vec4(1.0, 2.0, 3.0, 4.0).xyz.z", 3.0f,
          "a swizzle of a swizzle composes");
}

static void test_constructors(void) {
    printf("--- constructors ---\n");

    frag("", "  gl_FragColor = vec4(1.0, 2.0, 3.0, 4.0);",
         1.0f, 2.0f, 3.0f, 4.0f, "an explicit four-component constructor");

    frag("", "  gl_FragColor = vec4(5.0);",
         5.0f, 5.0f, 5.0f, 5.0f, "one scalar fills every component");

    frag("", "  gl_FragColor = vec4(vec2(1.0, 2.0), vec2(3.0, 4.0));",
         1.0f, 2.0f, 3.0f, 4.0f, "constructors flatten their arguments");

    frag("", "  gl_FragColor = vec4(vec3(1.0, 2.0, 3.0), 4.0);",
         1.0f, 2.0f, 3.0f, 4.0f, "a vector plus a scalar");

    frag("", "  int i = int(3.7);\n  gl_FragColor = vec4(float(i));",
         3.0f, 3.0f, 3.0f, 3.0f, "int() truncates towards zero");

    frag("", "  int i = int(-3.7);\n  gl_FragColor = vec4(float(i));",
         -3.0f, -3.0f, -3.0f, -3.0f, "int() truncates a negative towards zero");

    frag("", "  bool b = bool(2.5);\n  gl_FragColor = vec4(b ? 1.0 : 0.0);",
         1.0f, 1.0f, 1.0f, 1.0f, "bool() is a non-zero test");

    /* mat4(1.0) is the IDENTITY, not a matrix of ones.  Getting this wrong
     * produces a scene that renders but is subtly wrong everywhere. */
    frag("", "  mat4 m = mat4(2.0);\n"
             "  vec4 r = m * vec4(1.0, 1.0, 1.0, 1.0);\n"
             "  gl_FragColor = r;",
         2.0f, 2.0f, 2.0f, 2.0f, "matN(s) builds a diagonal, not a fill");

    frag("", "  mat2 m = mat2(1.0, 2.0, 3.0, 4.0);\n"
             "  vec2 r = m * vec2(1.0, 0.0);\n"
             "  gl_FragColor = vec4(r, 0.0, 1.0);",
         1.0f, 2.0f, 0.0f, 1.0f,
         "matrices are column-major: the first pair is column 0");
}

static void test_matrices(void) {
    printf("--- matrices ---\n");

    /* A translation matrix, written the way an application would build one,
     * proves the column-major convention end to end. */
    frag("", "  mat4 t = mat4(1.0);\n"
             "  t[3] = vec4(10.0, 20.0, 30.0, 1.0);\n"
             "  vec4 p = t * vec4(1.0, 2.0, 3.0, 1.0);\n"
             "  gl_FragColor = p;",
         11.0f, 22.0f, 33.0f, 1.0f, "a translation matrix transforms a point");

    frag("", "  mat2 a = mat2(1.0, 2.0, 3.0, 4.0);\n"
             "  mat2 b = mat2(5.0, 6.0, 7.0, 8.0);\n"
             "  mat2 c = a * b;\n"
             "  gl_FragColor = vec4(c[0], c[1]);",
         23.0f, 34.0f, 31.0f, 46.0f, "matrix times matrix");

    /* v * M is the row-vector product and differs from M * v unless the
     * matrix is symmetric — a test that catches a transposed implementation. */
    frag("", "  mat2 m = mat2(1.0, 2.0, 3.0, 4.0);\n"
             "  vec2 a = m * vec2(1.0, 0.0);\n"
             "  vec2 b = vec2(1.0, 0.0) * m;\n"
             "  gl_FragColor = vec4(a, b);",
         1.0f, 2.0f, 1.0f, 3.0f, "M*v and v*M differ, and both are right");

    frag("", "  mat3 m = mat3(1.0);\n"
             "  vec3 r = m * vec3(4.0, 5.0, 6.0);\n"
             "  gl_FragColor = vec4(r, 1.0);",
         4.0f, 5.0f, 6.0f, 1.0f, "the identity matrix leaves a vector alone");

    frag("", "  mat2 a = mat2(1.0, 2.0, 3.0, 4.0);\n"
             "  mat2 b = matrixCompMult(a, a);\n"
             "  gl_FragColor = vec4(b[0], b[1]);",
         1.0f, 4.0f, 9.0f, 16.0f, "matrixCompMult is component-wise");

    frag("", "  mat4 m = mat4(1.0);\n  float e = m[2][2];\n"
             "  gl_FragColor = vec4(e);",
         1.0f, 1.0f, 1.0f, 1.0f, "double indexing reaches a matrix element");
}

/* ============================================================================
 * Built-in functions
 * ==========================================================================*/

static void test_builtins_math(void) {
    printf("--- built-ins: maths ---\n");

    frag1("", "abs(-3.5)", 3.5f, "abs");
    frag1("", "sign(-2.0)", -1.0f, "sign of a negative");
    frag1("", "sign(0.0)", 0.0f, "sign of zero");
    frag1("", "floor(2.7)", 2.0f, "floor");
    frag1("", "floor(-2.3)", -3.0f, "floor of a negative rounds down");
    frag1("", "ceil(2.1)", 3.0f, "ceil");
    frag1("", "fract(2.75)", 0.75f, "fract");
    frag1("", "sqrt(16.0)", 4.0f, "sqrt");
    frag1("", "inversesqrt(4.0)", 0.5f, "inversesqrt");
    frag1("", "pow(2.0, 10.0)", 1024.0f, "pow");
    frag1("", "exp2(5.0)", 32.0f, "exp2");
    frag1("", "log2(64.0)", 6.0f, "log2");
    frag1("", "min(3.0, 7.0)", 3.0f, "min");
    frag1("", "max(3.0, 7.0)", 7.0f, "max");
    frag1("", "clamp(11.0, 0.0, 10.0)", 10.0f, "clamp above the range");
    frag1("", "clamp(-1.0, 0.0, 10.0)", 0.0f, "clamp below the range");
    frag1("", "step(0.5, 0.7)", 1.0f, "step above the edge");
    frag1("", "step(0.5, 0.3)", 0.0f, "step below the edge");
    frag1("", "mix(0.0, 10.0, 0.25)", 2.5f, "mix");
    frag1("", "smoothstep(0.0, 1.0, 0.5)", 0.5f, "smoothstep at the midpoint");
    frag1("", "smoothstep(0.0, 1.0, -1.0)", 0.0f, "smoothstep clamps below");
    frag1("", "smoothstep(0.0, 1.0, 2.0)", 1.0f, "smoothstep clamps above");
    frag1("", "radians(180.0)", 3.14159265f, "radians");
    frag1("", "degrees(3.14159265)", 180.0f, "degrees");

    /* GLSL's mod takes the sign of the DIVISOR, unlike C's fmod.  Shaders
     * that wrap a coordinate depend on the GLSL answer. */
    frag1("", "mod(-1.0, 3.0)", 2.0f, "mod takes the sign of the divisor");
    frag1("", "mod(7.0, 3.0)", 1.0f, "mod of positives");
    frag1("", "mod(5.0, 0.0)", 0.0f, "mod by zero is defined as zero");

    /* Trigonometry, at values with exact answers. */
    frag1("", "sin(0.0)", 0.0f, "sin(0)");
    frag1("", "cos(0.0)", 1.0f, "cos(0)");
    frag1("", "sin(radians(90.0))", 1.0f, "sin(90 degrees)");

    /* Component-wise application over a vector. */
    frag("", "  vec3 v = abs(vec3(-1.0, 2.0, -3.0));\n"
             "  gl_FragColor = vec4(v, 1.0);",
         1.0f, 2.0f, 3.0f, 1.0f, "a genType built-in maps over components");

    frag("", "  vec3 v = min(vec3(1.0, 5.0, 3.0), 2.0);\n"
             "  gl_FragColor = vec4(v, 1.0);",
         1.0f, 2.0f, 2.0f, 1.0f, "min broadcasts a scalar over a vector");

    frag("", "  vec3 v = mix(vec3(0.0), vec3(10.0), vec3(0.0, 0.5, 1.0));\n"
             "  gl_FragColor = vec4(v, 1.0);",
         0.0f, 5.0f, 10.0f, 1.0f, "mix accepts a per-component factor");
}

static void test_builtins_geometry(void) {
    printf("--- built-ins: geometry ---\n");

    frag1("", "length(vec3(3.0, 4.0, 0.0))", 5.0f, "length");
    frag1("", "distance(vec2(0.0, 0.0), vec2(3.0, 4.0))", 5.0f, "distance");
    frag1("", "dot(vec3(1.0, 2.0, 3.0), vec3(4.0, 5.0, 6.0))", 32.0f, "dot");

    frag("", "  vec3 c = cross(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0));\n"
             "  gl_FragColor = vec4(c, 1.0);",
         0.0f, 0.0f, 1.0f, 1.0f, "cross of the x and y axes is z");

    /* A built-in inside another built-in's argument list.  The argument
     * scratch is shared, so an implementation that keys it on the wrong
     * counter lets the inner call clobber the outer's arguments -- which
     * happened here, and showed up as a lighting term that silently ignored
     * its clamp.  Every real lighting shader is this shape. */
    frag1("", "max(dot(vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0)), 0.0)", 0.0f,
          "a built-in nested in a built-in argument");
    frag1("", "max(dot(vec3(0.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0)), 0.0)", 1.0f,
          "the same nesting when the clamp does not bite");
    frag1("", "clamp(min(max(1.0, 2.0), 5.0), 0.0, 3.0)", 2.0f,
          "three levels of nested built-ins");
    frag("", "  vec3 v = normalize(cross(vec3(2.0, 0.0, 0.0),\n"
             "                           vec3(0.0, 3.0, 0.0)));\n"
             "  gl_FragColor = vec4(v, 1.0);",
         0.0f, 0.0f, 1.0f, 1.0f, "a built-in taking a built-in's result");

    frag("", "  vec3 n = normalize(vec3(3.0, 4.0, 0.0));\n"
             "  gl_FragColor = vec4(n, 1.0);",
         0.6f, 0.8f, 0.0f, 1.0f, "normalize");

    /* Normalising a zero vector is undefined in GLSL; returning zero keeps a
     * NaN out of the framebuffer. */
    frag("", "  vec3 n = normalize(vec3(0.0));\n"
             "  gl_FragColor = vec4(n, 1.0);",
         0.0f, 0.0f, 0.0f, 1.0f, "normalize of zero is zero, not NaN");

    /* reflect(I, N) = I - 2*dot(N,I)*N.  Bouncing straight down off a
     * horizontal surface must come straight back up. */
    frag("", "  vec3 r = reflect(vec3(0.0, -1.0, 0.0), vec3(0.0, 1.0, 0.0));\n"
             "  gl_FragColor = vec4(r, 1.0);",
         0.0f, 1.0f, 0.0f, 1.0f, "reflect off a horizontal surface");

    frag("", "  vec3 f = faceforward(vec3(1.0, 0.0, 0.0),\n"
             "                       vec3(0.0, 0.0, 1.0),\n"
             "                       vec3(0.0, 0.0, 1.0));\n"
             "  gl_FragColor = vec4(f, 1.0);",
         -1.0f, 0.0f, 0.0f, 1.0f, "faceforward flips when the normal agrees");
}

static void test_builtins_relational(void) {
    printf("--- built-ins: relational ---\n");

    frag("", "  bvec3 b = lessThan(vec3(1.0, 5.0, 3.0), vec3(2.0, 2.0, 3.0));\n"
             "  gl_FragColor = vec4(b.x ? 1.0 : 0.0, b.y ? 1.0 : 0.0,\n"
             "                      b.z ? 1.0 : 0.0, 1.0);",
         1.0f, 0.0f, 0.0f, 1.0f, "lessThan is component-wise");

    frag1("", "any(lessThan(vec3(1.0, 5.0, 3.0), vec3(2.0, 2.0, 2.0))) ? 1.0 : 0.0",
          1.0f, "any");
    frag1("", "all(lessThan(vec3(1.0, 5.0, 3.0), vec3(2.0, 2.0, 2.0))) ? 1.0 : 0.0",
          0.0f, "all");
    frag1("", "all(lessThan(vec3(1.0), vec3(2.0))) ? 1.0 : 0.0",
          1.0f, "all when every component passes");

    frag("", "  bvec2 b = not(lessThan(vec2(1.0, 5.0), vec2(2.0, 2.0)));\n"
             "  gl_FragColor = vec4(b.x ? 1.0 : 0.0, b.y ? 1.0 : 0.0, 0.0, 1.0);",
         0.0f, 1.0f, 0.0f, 1.0f, "not inverts a bvec");

    frag("", "  bvec3 b = equal(vec3(1.0, 2.0, 3.0), vec3(1.0, 9.0, 3.0));\n"
             "  gl_FragColor = vec4(b.x ? 1.0 : 0.0, b.y ? 1.0 : 0.0,\n"
             "                      b.z ? 1.0 : 0.0, 1.0);",
         1.0f, 0.0f, 1.0f, 1.0f, "equal is component-wise, unlike ==");
}

/* ============================================================================
 * Control flow
 * ==========================================================================*/

static void test_control_flow(void) {
    printf("--- control flow ---\n");

    frag("", "  float r = 0.0;\n  if (1.0 < 2.0) r = 5.0; else r = 9.0;\n"
             "  gl_FragColor = vec4(r);",
         5.0f, 5.0f, 5.0f, 5.0f, "if takes the true branch");

    frag("", "  float r = 0.0;\n  if (2.0 < 1.0) r = 5.0; else r = 9.0;\n"
             "  gl_FragColor = vec4(r);",
         9.0f, 9.0f, 9.0f, 9.0f, "if takes the else branch");

    frag("", "  float a = 0.0;\n  for (int i = 0; i < 5; i++) { a += 2.0; }\n"
             "  gl_FragColor = vec4(a);",
         10.0f, 10.0f, 10.0f, 10.0f, "a for loop runs the right number of times");

    frag("", "  float a = 0.0;\n  int i = 0;\n"
             "  while (i < 4) { a += 3.0; i++; }\n"
             "  gl_FragColor = vec4(a);",
         12.0f, 12.0f, 12.0f, 12.0f, "a while loop");

    frag("", "  float a = 0.0;\n  int i = 0;\n"
             "  do { a += 3.0; i++; } while (i < 4);\n"
             "  gl_FragColor = vec4(a);",
         12.0f, 12.0f, 12.0f, 12.0f, "a do-while loop");

    /* A do-while always runs once, even when the condition is false. */
    frag("", "  float a = 0.0;\n"
             "  do { a += 7.0; } while (false);\n"
             "  gl_FragColor = vec4(a);",
         7.0f, 7.0f, 7.0f, 7.0f, "do-while runs its body at least once");

    frag("", "  float a = 0.0;\n"
             "  for (int i = 0; i < 10; i++) { if (i == 3) break; a += 1.0; }\n"
             "  gl_FragColor = vec4(a);",
         3.0f, 3.0f, 3.0f, 3.0f, "break leaves the loop");

    frag("", "  float a = 0.0;\n"
             "  for (int i = 0; i < 5; i++) { if (i == 2) continue; a += 1.0; }\n"
             "  gl_FragColor = vec4(a);",
         4.0f, 4.0f, 4.0f, 4.0f, "continue skips the rest of the body");

    /* continue in a for loop must still run the step expression, or the loop
     * never advances — an easy way to write an accidental infinite loop. */
    frag("", "  float a = 0.0;\n  int n = 0;\n"
             "  for (int i = 0; i < 4; i++) { n++; if (i < 2) continue; a += 1.0; }\n"
             "  gl_FragColor = vec4(a, float(n), 0.0, 1.0);",
         2.0f, 4.0f, 0.0f, 1.0f, "continue still runs the for-loop step");

    frag("", "  float a = 0.0;\n"
             "  for (int i = 0; i < 3; i++) {\n"
             "    for (int j = 0; j < 3; j++) { a += 1.0; }\n"
             "  }\n  gl_FragColor = vec4(a);",
         9.0f, 9.0f, 9.0f, 9.0f, "nested loops");

    /* break leaves only the inner loop. */
    frag("", "  float a = 0.0;\n"
             "  for (int i = 0; i < 3; i++) {\n"
             "    for (int j = 0; j < 3; j++) { if (j == 1) break; a += 1.0; }\n"
             "  }\n  gl_FragColor = vec4(a);",
         3.0f, 3.0f, 3.0f, 3.0f, "break leaves only the innermost loop");

    frag("", "  float a = 0.0;\n"
             "  for (int i = 0; i < 3; i++) { float local = 5.0; a += local; }\n"
             "  gl_FragColor = vec4(a);",
         15.0f, 15.0f, 15.0f, 15.0f,
         "a loop body's locals do not accumulate storage");
}

static void test_functions(void) {
    printf("--- functions ---\n");

    frag("float sq(float x) { return x * x; }",
         "  gl_FragColor = vec4(sq(7.0));",
         49.0f, 49.0f, 49.0f, 49.0f, "a user function returns a value");

    frag("vec3 scale(vec3 v, float k) { return v * k; }",
         "  gl_FragColor = vec4(scale(vec3(1.0, 2.0, 3.0), 2.0), 1.0);",
         2.0f, 4.0f, 6.0f, 1.0f, "a function taking a vector and a scalar");

    frag("float pick(float a, float b) { if (a > b) return a; return b; }",
         "  gl_FragColor = vec4(pick(3.0, 8.0));",
         8.0f, 8.0f, 8.0f, 8.0f, "an early return");

    /* An `in` parameter is a copy: writing it must not touch the caller. */
    frag("void bump(float x) { x = 99.0; }",
         "  float f = 1.0;\n  bump(f);\n  gl_FragColor = vec4(f);",
         1.0f, 1.0f, 1.0f, 1.0f, "an in parameter is passed by value");

    frag("void get(out float v) { v = 42.0; }",
         "  float f = 1.0;\n  get(f);\n  gl_FragColor = vec4(f);",
         42.0f, 42.0f, 42.0f, 42.0f, "an out parameter writes back");

    frag("void twice(inout float v) { v = v * 2.0; }",
         "  float f = 21.0;\n  twice(f);\n  gl_FragColor = vec4(f);",
         42.0f, 42.0f, 42.0f, 42.0f, "an inout parameter reads and writes back");

    /* A callee must not see the caller's locals, even by the same name. */
    frag("float inner() { return 7.0; }",
         "  float x = 1.0;\n  float r = inner();\n"
         "  gl_FragColor = vec4(x + r);",
         8.0f, 8.0f, 8.0f, 8.0f, "a callee does not disturb the caller's frame");

    frag("float a(float x) { return x + 1.0; }\n"
         "float b(float x) { return a(x) * 2.0; }",
         "  gl_FragColor = vec4(b(3.0));",
         8.0f, 8.0f, 8.0f, 8.0f, "nested calls");

    /* Globals stay visible inside a function. */
    frag("float gScale = 3.0;\nfloat use() { return gScale; }",
         "  gl_FragColor = vec4(use());",
         3.0f, 3.0f, 3.0f, 3.0f, "a function sees globals");

    frag("void setBoth(out float a, out float b) { a = 1.0; b = 2.0; }",
         "  float p = 0.0; float q = 0.0;\n  setBoth(p, q);\n"
         "  gl_FragColor = vec4(p, q, 0.0, 1.0);",
         1.0f, 2.0f, 0.0f, 1.0f, "two out parameters");

    /* An out parameter targeting a swizzle exercises the lvalue machinery. */
    frag("void get(out float v) { v = 9.0; }",
         "  vec4 c = vec4(0.0);\n  get(c.z);\n  gl_FragColor = c;",
         0.0f, 0.0f, 9.0f, 0.0f, "an out parameter writes through a swizzle");
}

/* ============================================================================
 * Increment, assignment and lvalues
 * ==========================================================================*/

static void test_assignment(void) {
    printf("--- assignment and increment ---\n");

    frag("", "  float f = 1.0;\n  f += 2.0;\n  gl_FragColor = vec4(f);",
         3.0f, 3.0f, 3.0f, 3.0f, "compound add-assign");

    frag("", "  vec3 v = vec3(1.0, 2.0, 3.0);\n  v *= 2.0;\n"
             "  gl_FragColor = vec4(v, 1.0);",
         2.0f, 4.0f, 6.0f, 1.0f, "compound multiply-assign broadcasts");

    frag("", "  vec2 v = vec2(1.0, 0.0);\n"
             "  mat2 m = mat2(1.0, 2.0, 3.0, 4.0);\n"
             "  v *= m;\n  gl_FragColor = vec4(v, 0.0, 1.0);",
         1.0f, 3.0f, 0.0f, 1.0f,
         "vec *= mat is a transform, not a component-wise multiply");

    /* Pre- and post-increment differ in the value they yield. */
    frag("", "  float i = 5.0;\n  float r = i++;\n"
             "  gl_FragColor = vec4(r, i, 0.0, 1.0);",
         5.0f, 6.0f, 0.0f, 1.0f, "post-increment yields the old value");

    frag("", "  float i = 5.0;\n  float r = ++i;\n"
             "  gl_FragColor = vec4(r, i, 0.0, 1.0);",
         6.0f, 6.0f, 0.0f, 1.0f, "pre-increment yields the new value");

    frag("", "  float a = 1.0; float b = 2.0;\n  a = b = 3.0;\n"
             "  gl_FragColor = vec4(a, b, 0.0, 1.0);",
         3.0f, 3.0f, 0.0f, 1.0f, "assignment is right-associative and chains");

    frag("", "  vec4 v = vec4(1.0);\n  v.y += 4.0;\n  gl_FragColor = v;",
         1.0f, 5.0f, 1.0f, 1.0f, "compound assignment through a swizzle");
}

/* ============================================================================
 * Structs and arrays
 * ==========================================================================*/

static void test_aggregates(void) {
    printf("--- structs and arrays ---\n");

    frag("struct P { float a; vec2 b; };",
         "  P p = P(1.0, vec2(2.0, 3.0));\n"
         "  gl_FragColor = vec4(p.a, p.b, 1.0);",
         1.0f, 2.0f, 3.0f, 1.0f, "a struct constructor and member reads");

    frag("struct P { float a; vec2 b; };",
         "  P p = P(1.0, vec2(2.0, 3.0));\n  p.b.y = 9.0;\n"
         "  gl_FragColor = vec4(p.a, p.b, 1.0);",
         1.0f, 2.0f, 9.0f, 1.0f, "writing a struct member through a swizzle");

    frag("struct Inner { float x; };\nstruct Outer { Inner i; float y; };",
         "  Outer o = Outer(Inner(3.0), 4.0);\n"
         "  gl_FragColor = vec4(o.i.x, o.y, 0.0, 1.0);",
         3.0f, 4.0f, 0.0f, 1.0f, "a nested struct");

    frag("", "  float a[3];\n  a[0] = 1.0; a[1] = 2.0; a[2] = 3.0;\n"
             "  gl_FragColor = vec4(a[0], a[1], a[2], 1.0);",
         1.0f, 2.0f, 3.0f, 1.0f, "an array of floats");

    frag("", "  vec3 v[2];\n  v[0] = vec3(1.0, 2.0, 3.0);\n"
             "  v[1] = vec3(4.0, 5.0, 6.0);\n"
             "  gl_FragColor = vec4(v[1], 1.0);",
         4.0f, 5.0f, 6.0f, 1.0f, "an array of vectors");

    frag("", "  float a[4];\n"
             "  for (int i = 0; i < 4; i++) { a[i] = float(i) * 2.0; }\n"
             "  float sum = 0.0;\n"
             "  for (int i = 0; i < 4; i++) { sum += a[i]; }\n"
             "  gl_FragColor = vec4(sum);",
         12.0f, 12.0f, 12.0f, 12.0f, "an array indexed by a loop variable");

    /* A dynamic index out of range must be clamped, not read out of bounds.
     * GLSL leaves the behaviour undefined; a defined answer is safer than an
     * out-of-bounds read in an interpreter running on a live framebuffer. */
    frag("", "  float a[3];\n  a[0] = 1.0; a[1] = 2.0; a[2] = 3.0;\n"
             "  int i = 7;\n  gl_FragColor = vec4(a[i]);",
         3.0f, 3.0f, 3.0f, 3.0f, "a dynamic index past the end is clamped");

    frag("", "  float a[3];\n  a[0] = 1.0; a[1] = 2.0; a[2] = 3.0;\n"
             "  int i = -5;\n  gl_FragColor = vec4(a[i]);",
         1.0f, 1.0f, 1.0f, 1.0f, "a negative dynamic index is clamped");
}

/* ============================================================================
 * The environment: uniforms, varyings, samplers
 * ==========================================================================*/

static void test_environment(void) {
    printf("--- environment ---\n");

    {
        testenv_t te;
        te_init(&te);
        te_uniform(&te, "uScale", 1, 3.0);
        frag_env(&te,
                 "uniform float uScale;\n"
                 "void main() { gl_FragColor = vec4(uScale * 2.0); }\n",
                 6.0f, 6.0f, 6.0f, 6.0f, "a scalar uniform is read");
    }

    {
        testenv_t te;
        te_init(&te);
        te_uniform(&te, "uColor", 3, 0.25, 0.5, 0.75);
        frag_env(&te,
                 "uniform vec3 uColor;\n"
                 "void main() { gl_FragColor = vec4(uColor, 1.0); }\n",
                 0.25f, 0.5f, 0.75f, 1.0f, "a vector uniform is read");
    }

    {
        /* A uniform nobody set reads as zero, matching GL. */
        testenv_t te;
        te_init(&te);
        frag_env(&te,
                 "uniform vec4 uMissing;\n"
                 "void main() { gl_FragColor = uMissing; }\n",
                 0.0f, 0.0f, 0.0f, 0.0f, "an unset uniform reads as zero");
    }

    {
        /* A matrix uniform, in the column-major order glUniformMatrix uses. */
        testenv_t te;
        te_init(&te);
        te_uniform(&te, "uM", 16,
                   1.0, 0.0, 0.0, 0.0,
                   0.0, 1.0, 0.0, 0.0,
                   0.0, 0.0, 1.0, 0.0,
                   5.0, 6.0, 7.0, 1.0);
        frag_env(&te,
                 "uniform mat4 uM;\n"
                 "void main() { gl_FragColor = uM * vec4(1.0, 2.0, 3.0, 1.0); }\n",
                 6.0f, 8.0f, 10.0f, 1.0f,
                 "a column-major matrix uniform transforms correctly");
    }

    {
        /* The sampler receives the unit number the uniform was set to, and
         * the coordinate the shader asked for. */
        testenv_t te;
        te_init(&te);
        te_uniform(&te, "uTex", 1, 3.0);
        frag_env(&te,
                 "uniform sampler2D uTex;\n"
                 "void main() { gl_FragColor = texture2D(uTex, vec2(0.25, 0.5)); }\n",
                 0.25f, 0.5f, 3.0f, 1.0f, "texture2D reaches the sampler");
        CHECK(te.last_unit == 3, "the sampler receives its texture unit");
        CHECK(near_f(te.last_coord[0], 0.25f) && near_f(te.last_coord[1], 0.5f),
              "the sampler receives the requested coordinate");
        CHECK(te.last_is_cube == 0, "texture2D is not a cube lookup");
    }

    {
        testenv_t te;
        te_init(&te);
        te_uniform(&te, "uCube", 1, 1.0);
        frag_env(&te,
                 "uniform samplerCube uCube;\n"
                 "void main() { gl_FragColor = textureCube(uCube, vec3(0.1, 0.2, 0.3)); }\n",
                 0.1f, 0.2f, 0.3f, 1.0f, "textureCube passes three coordinates");
        CHECK(te.last_is_cube == 1, "textureCube is flagged as a cube lookup");
    }

    {
        /* texture2DProj divides by the last component. */
        testenv_t te;
        te_init(&te);
        te_uniform(&te, "uTex", 1, 0.0);
        frag_env(&te,
                 "uniform sampler2D uTex;\n"
                 "void main() {\n"
                 "  gl_FragColor = texture2DProj(uTex, vec4(1.0, 2.0, 0.0, 4.0));\n"
                 "}\n",
                 0.25f, 0.5f, 0.0f, 1.0f,
                 "texture2DProj divides by the last component");
    }

    {
        /* A vertex shader writes gl_Position through the environment. */
        const char *vs =
            "attribute vec4 aPos;\n"
            "uniform mat4 uM;\n"
            "void main() { gl_Position = uM * aPos; }\n";
        testenv_t te;
        te_init(&te);
        te_uniform(&te, "aPos", 4, 1.0, 2.0, 3.0, 1.0);
        te_uniform(&te, "uM", 16,
                   2.0, 0.0, 0.0, 0.0,
                   0.0, 2.0, 0.0, 0.0,
                   0.0, 0.0, 2.0, 0.0,
                   0.0, 0.0, 0.0, 1.0);
        glsl_unit_t *u = glsl_compile(vs, GLSL_SHADER_VERTEX);
        CHECK(u && u->compiled, "the vertex shader compiles");
        if (u && u->compiled) {
            glsl_run_status_t st = glsl_run(u, &te.env);
            CHECK(st == GLSL_RUN_OK, "the vertex shader runs");
            CHECK(te.wrote_position, "gl_Position was written");
            CHECK(near_f(te.out_position[0], 2.0f) &&
                  near_f(te.out_position[1], 4.0f) &&
                  near_f(te.out_position[2], 6.0f) &&
                  near_f(te.out_position[3], 1.0f),
                  "gl_Position carries the transformed vertex");
        }
        glsl_unit_free(u);
    }

    {
        /* A partial write to an environment variable must merge, not clobber
         * the components it does not name. */
        const char *vs =
            "void main() {\n"
            "  gl_Position = vec4(9.0, 9.0, 9.0, 9.0);\n"
            "  gl_Position.xy = vec2(1.0, 2.0);\n"
            "}\n";
        testenv_t te;
        te_init(&te);
        glsl_unit_t *u = glsl_compile(vs, GLSL_SHADER_VERTEX);
        if (u && u->compiled) {
            glsl_run(u, &te.env);
            CHECK(near_f(te.out_position[0], 1.0f) &&
                  near_f(te.out_position[1], 2.0f) &&
                  near_f(te.out_position[2], 9.0f) &&
                  near_f(te.out_position[3], 9.0f),
                  "a swizzled write to gl_Position merges");
        } else {
            CHECK(0, "a swizzled write to gl_Position merges (compile failed)");
        }
        glsl_unit_free(u);
    }

    {
        /* An environment with no callbacks at all must not crash: every
         * external read is zero and every write is dropped. */
        glsl_env_t bare;
        memset(&bare, 0, sizeof bare);
        glsl_unit_t *u = glsl_compile(
            "uniform vec4 uC;\n"
            "void main() { gl_FragColor = uC + vec4(1.0); }\n",
            GLSL_SHADER_FRAGMENT);
        CHECK(u && u->compiled && glsl_run(u, &bare) == GLSL_RUN_OK,
              "a null environment is survivable");
        glsl_unit_free(u);

        u = glsl_compile("void main() { gl_FragColor = vec4(1.0); }\n",
                         GLSL_SHADER_FRAGMENT);
        CHECK(u && glsl_run(u, NULL) == GLSL_RUN_OK,
              "a NULL environment pointer is survivable");
        glsl_unit_free(u);
    }
}

/* ============================================================================
 * discard, and running the same shader repeatedly
 * ==========================================================================*/

static void test_discard_and_reuse(void) {
    printf("--- discard and reuse ---\n");

    {
        glsl_unit_t *u = glsl_compile("void main() { discard; }\n",
                                      GLSL_SHADER_FRAGMENT);
        testenv_t te;
        te_init(&te);
        CHECK(u && u->compiled && glsl_run(u, &te.env) == GLSL_RUN_DISCARD,
              "discard reports GLSL_RUN_DISCARD");
        CHECK(!te.wrote_color, "a discarded fragment writes no colour");
        glsl_unit_free(u);
    }

    {
        /* Statements after a discard must not run. */
        glsl_unit_t *u = glsl_compile(
            "void main() { discard; gl_FragColor = vec4(1.0); }\n",
            GLSL_SHADER_FRAGMENT);
        testenv_t te;
        te_init(&te);
        glsl_run(u, &te.env);
        CHECK(!te.wrote_color, "discard stops execution immediately");
        glsl_unit_free(u);
    }

    {
        /* A conditional discard leaves the other path intact. */
        glsl_unit_t *u = glsl_compile(
            "uniform float uCut;\n"
            "void main() {\n"
            "  if (uCut > 0.5) discard;\n"
            "  gl_FragColor = vec4(0.5);\n"
            "}\n", GLSL_SHADER_FRAGMENT);

        testenv_t keep;
        te_init(&keep);
        te_uniform(&keep, "uCut", 1, 0.0);
        CHECK(glsl_run(u, &keep.env) == GLSL_RUN_OK && keep.wrote_color,
              "a conditional discard is not taken when the test fails");

        testenv_t cut;
        te_init(&cut);
        te_uniform(&cut, "uCut", 1, 1.0);
        CHECK(glsl_run(u, &cut.env) == GLSL_RUN_DISCARD,
              "a conditional discard is taken when the test passes");
        glsl_unit_free(u);
    }

    {
        /* The realistic case: one compiled shader, run once per fragment.
         * The arena must not grow, and each run must be independent. */
        glsl_unit_t *u = glsl_compile(
            "uniform float uX;\n"
            "void main() {\n"
            "  float acc = 0.0;\n"
            "  for (int i = 0; i < 4; i++) { acc += uX; }\n"
            "  gl_FragColor = vec4(acc);\n"
            "}\n", GLSL_SHADER_FRAGMENT);
        CHECK(u && u->compiled, "the reuse shader compiles");

        size_t arena_after_first = 0;
        int all_ok = 1;
        for (int k = 0; k < 500; k++) {
            testenv_t te;
            te_init(&te);
            te_uniform(&te, "uX", 1, (double)k);
            if (glsl_run(u, &te.env) != GLSL_RUN_OK) { all_ok = 0; break; }
            if (!near_f(te.out_color[0], (float)k * 4.0f)) { all_ok = 0; break; }
            if (k == 0) arena_after_first = u->arena_used;
        }
        CHECK(all_ok, "500 runs each produce the right answer");
        CHECK(u && u->arena_used == arena_after_first,
              "repeated runs do not grow the arena");
        glsl_unit_free(u);
    }
}

/* ============================================================================
 * Robustness: a shader is untrusted input
 * ==========================================================================*/

static void test_robustness(void) {
    printf("--- robustness ---\n");

    {
        /* `while (true) {}` is a legal program.  On hardware a watchdog
         * resets the GPU; here it would hang the compositor, so the budget
         * turns it into a diagnostic. */
        glsl_unit_t *u = glsl_compile(
            "void main() { while (true) { } gl_FragColor = vec4(1.0); }\n",
            GLSL_SHADER_FRAGMENT);
        testenv_t te;
        te_init(&te);
        CHECK(u && u->compiled && glsl_run(u, &te.env) == GLSL_RUN_ERROR,
              "an infinite loop terminates with an error");
        CHECK(u && strstr(glsl_unit_log(u), "iteration budget"),
              "the infinite-loop diagnostic explains itself");
        glsl_unit_free(u);
    }

    {
        /* A for loop that never advances is the same hazard in disguise. */
        glsl_unit_t *u = glsl_compile(
            "void main() {\n"
            "  for (int i = 0; i < 10; ) { }\n"
            "  gl_FragColor = vec4(1.0);\n"
            "}\n", GLSL_SHADER_FRAGMENT);
        testenv_t te;
        te_init(&te);
        CHECK(u && glsl_run(u, &te.env) == GLSL_RUN_ERROR,
              "a for loop with no step terminates");
        glsl_unit_free(u);
    }

    {
        /* Recursion is forbidden by GLSL ES, but the compiler accepts a
         * self-call; the interpreter must bound it rather than overflow the
         * user stack -- the failure mode G12 found in aglxResize(). */
        glsl_unit_t *u = glsl_compile(
            "float r(float x) { return r(x) + 1.0; }\n"
            "void main() { gl_FragColor = vec4(r(1.0)); }\n",
            GLSL_SHADER_FRAGMENT);
        testenv_t te;
        te_init(&te);
        CHECK(u && u->compiled && glsl_run(u, &te.env) == GLSL_RUN_ERROR,
              "runaway recursion terminates with an error");
        CHECK(u && strstr(glsl_unit_log(u), "depth"),
              "the recursion diagnostic mentions depth");
        glsl_unit_free(u);
    }

    {
        /* A deeply nested but legal loop nest must still finish. */
        glsl_unit_t *u = glsl_compile(
            "void main() {\n"
            "  float a = 0.0;\n"
            "  for (int i = 0; i < 20; i++)\n"
            "    for (int j = 0; j < 20; j++)\n"
            "      for (int k = 0; k < 20; k++) a += 1.0;\n"
            "  gl_FragColor = vec4(a);\n"
            "}\n", GLSL_SHADER_FRAGMENT);
        testenv_t te;
        te_init(&te);
        glsl_run_status_t st = glsl_run(u, &te.env);
        CHECK(st == GLSL_RUN_OK && near_f(te.out_color[0], 8000.0f),
              "8000 iterations complete within the budget");
        glsl_unit_free(u);
    }

    {
        /* Running an uncompiled unit must be refused, not attempted. */
        glsl_unit_t *u = glsl_compile("void main() { nope; }\n",
                                      GLSL_SHADER_FRAGMENT);
        testenv_t te;
        te_init(&te);
        CHECK(u && !u->compiled && glsl_run(u, &te.env) == GLSL_RUN_ERROR,
              "a failed compilation cannot be run");
        glsl_unit_free(u);

        CHECK(glsl_run(NULL, &te.env) == GLSL_RUN_ERROR,
              "running a NULL unit is refused");
    }

    {
        /* Values that would produce NaN or infinity must come out finite:
         * one bad fragment must not poison a blend or a depth test. */
        glsl_unit_t *u = glsl_compile(
            "void main() {\n"
            "  float a = sqrt(-1.0);\n"
            "  float b = log(0.0);\n"
            "  float c = 1.0 / 0.0;\n"
            "  float d = length(normalize(vec3(0.0)));\n"
            "  gl_FragColor = vec4(a, b, c, d);\n"
            "}\n", GLSL_SHADER_FRAGMENT);
        testenv_t te;
        te_init(&te);
        glsl_run(u, &te.env);
        int finite = 1;
        for (int i = 0; i < 4; i++) {
            if (isnan(te.out_color[i]) || isinf(te.out_color[i])) finite = 0;
        }
        CHECK(finite, "undefined maths yields finite values, not NaN");
        glsl_unit_free(u);
    }
}

/* ============================================================================
 * Realistic shaders, end to end
 * ==========================================================================*/

static void test_realistic(void) {
    printf("--- realistic shaders ---\n");

    {
        /* Lambert shading with the light directly overhead: N.L is 1, so the
         * result is the base colour. */
        testenv_t te;
        te_init(&te);
        te_uniform(&te, "vNormal", 3, 0.0, 1.0, 0.0);
        te_uniform(&te, "uLightDir", 3, 0.0, 1.0, 0.0);
        te_uniform(&te, "uBase", 3, 0.2, 0.4, 0.6);
        frag_env(&te,
                 "precision mediump float;\n"
                 "varying vec3 vNormal;\n"
                 "uniform vec3 uLightDir;\n"
                 "uniform vec3 uBase;\n"
                 "void main() {\n"
                 "  float ndl = max(dot(normalize(vNormal),\n"
                 "                      normalize(uLightDir)), 0.0);\n"
                 "  gl_FragColor = vec4(uBase * ndl, 1.0);\n"
                 "}\n",
                 0.2f, 0.4f, 0.6f, 1.0f, "Lambert with the light head-on");
    }

    {
        /* The same shader with the light behind: N.L is negative, clamped to
         * zero, so the surface is black. */
        testenv_t te;
        te_init(&te);
        te_uniform(&te, "vNormal", 3, 0.0, 1.0, 0.0);
        te_uniform(&te, "uLightDir", 3, 0.0, -1.0, 0.0);
        te_uniform(&te, "uBase", 3, 0.2, 0.4, 0.6);
        frag_env(&te,
                 "precision mediump float;\n"
                 "varying vec3 vNormal;\n"
                 "uniform vec3 uLightDir;\n"
                 "uniform vec3 uBase;\n"
                 "void main() {\n"
                 "  float ndl = max(dot(normalize(vNormal),\n"
                 "                      normalize(uLightDir)), 0.0);\n"
                 "  gl_FragColor = vec4(uBase * ndl, 1.0);\n"
                 "}\n",
                 0.0f, 0.0f, 0.0f, 1.0f, "Lambert with the light behind");
    }

    {
        /* Blinn-Phong with a helper function, the shader G11c will use as its
         * smoke test.  Light and eye both along +Y over a +Y normal, so the
         * half vector is +Y and the specular term is exactly 1. */
        testenv_t te;
        te_init(&te);
        te_uniform(&te, "vNormal", 3, 0.0, 1.0, 0.0);
        te_uniform(&te, "vEyePos", 3, 0.0, -1.0, 0.0);
        te_uniform(&te, "uLightPos", 3, 0.0, 1.0, 0.0);
        te_uniform(&te, "uLightColor", 3, 1.0, 1.0, 1.0);
        te_uniform(&te, "uShininess", 1, 32.0);
        te_uniform(&te, "uBase", 3, 0.1, 0.1, 0.1);
        frag_env(&te,
                 "precision mediump float;\n"
                 "varying vec3 vNormal;\n"
                 "varying vec3 vEyePos;\n"
                 "uniform vec3 uLightPos;\n"
                 "uniform vec3 uLightColor;\n"
                 "uniform float uShininess;\n"
                 "uniform vec3 uBase;\n"
                 "vec3 blinnPhong(vec3 n, vec3 l, vec3 v, vec3 base) {\n"
                 "  float ndl = max(dot(n, l), 0.0);\n"
                 "  vec3 h = normalize(l + v);\n"
                 "  float spec = pow(max(dot(n, h), 0.0), uShininess);\n"
                 "  return base * ndl + uLightColor * spec;\n"
                 "}\n"
                 "void main() {\n"
                 "  vec3 n = normalize(vNormal);\n"
                 "  vec3 l = normalize(uLightPos - vEyePos);\n"
                 "  vec3 v = normalize(-vEyePos);\n"
                 "  gl_FragColor = vec4(blinnPhong(n, l, v, uBase), 1.0);\n"
                 "}\n",
                 1.1f, 1.1f, 1.1f, 1.0f, "Blinn-Phong with a perfect highlight");
    }

    {
        /* Four-light accumulation in a loop, with arrays indexed by the loop
         * variable — the combination G11b has to get right for anything real. */
        testenv_t te;
        te_init(&te);
        te_uniform(&te, "uCount", 1, 4.0);
        frag_env(&te,
                 "precision mediump float;\n"
                 "uniform float uCount;\n"
                 "void main() {\n"
                 "  vec3 lights[4];\n"
                 "  lights[0] = vec3(0.1, 0.0, 0.0);\n"
                 "  lights[1] = vec3(0.0, 0.2, 0.0);\n"
                 "  lights[2] = vec3(0.0, 0.0, 0.3);\n"
                 "  lights[3] = vec3(0.1, 0.1, 0.1);\n"
                 "  vec3 sum = vec3(0.0);\n"
                 "  for (int i = 0; i < 4; i++) { sum += lights[i]; }\n"
                 "  gl_FragColor = vec4(sum, 1.0);\n"
                 "}\n",
                 0.2f, 0.3f, 0.4f, 1.0f, "a four-light accumulation loop");
    }

    {
        /* A procedural shader: no inputs, all maths. */
        testenv_t te;
        te_init(&te);
        te_uniform(&te, "gl_FragCoord", 4, 100.0, 50.0, 0.5, 1.0);
        frag_env(&te,
                 "precision mediump float;\n"
                 "void main() {\n"
                 "  vec2 p = gl_FragCoord.xy * 0.01;\n"
                 "  float v = p.x + p.y;\n"
                 "  gl_FragColor = vec4(vec3(v), 1.0);\n"
                 "}\n",
                 1.5f, 1.5f, 1.5f, 1.0f, "a procedural shader reading gl_FragCoord");
    }

    {
        /* A texture-modulated shader, the commonest fragment shader there is. */
        testenv_t te;
        te_init(&te);
        te_uniform(&te, "uTex", 1, 0.0);
        te_uniform(&te, "vTexCoord", 2, 0.5, 0.25);
        te_uniform(&te, "uTint", 4, 2.0, 2.0, 2.0, 1.0);
        frag_env(&te,
                 "precision mediump float;\n"
                 "uniform sampler2D uTex;\n"
                 "uniform vec4 uTint;\n"
                 "varying vec2 vTexCoord;\n"
                 "void main() {\n"
                 "  gl_FragColor = texture2D(uTex, vTexCoord) * uTint;\n"
                 "}\n",
                 1.0f, 0.5f, 0.0f, 1.0f, "a texture modulated by a tint");
    }
}

/* ============================================================================
 * Driver
 * ==========================================================================*/

int main(void) {
    printf("=== test_glslexec: GLSL execution engine (phase G11b) ===\n");

    test_arithmetic();
    test_comparisons();
    test_swizzles();
    test_constructors();
    test_matrices();
    test_builtins_math();
    test_builtins_geometry();
    test_builtins_relational();
    test_control_flow();
    test_functions();
    test_assignment();
    test_aggregates();
    test_environment();
    test_discard_and_reuse();
    test_robustness();
    test_realistic();

    printf("\ntest_glslexec: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}
