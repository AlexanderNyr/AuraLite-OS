/*
 * test_glsl.c — host-side unit tests for the GLSL ES 1.0 front end (G11a).
 *
 * The front end is tested against SHADER SOURCE, not against internal APIs:
 * every case feeds a string to glsl_compile() and asserts what came out.  That
 * is deliberate — a compiler's contract with its users is "this source is
 * accepted, that source is rejected, and here is why", and a test written
 * against the AST shape would pass while the contract broke.
 *
 * Negative tests assert on the MESSAGE as well as the failure, using a
 * substring match.  A compiler that rejects a bad shader with an unhelpful
 * diagnostic has only done half its job: the application author sees the log
 * and nothing else.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "glsl.h"

static int tn = 0, passed = 0, failed = 0;

#define CHECK(cond, name) do {                          \
    tn++;                                               \
    if (cond) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", (name)); }  \
} while (0)

/* Compile and assert success.  On failure the log is printed, because a test
 * that says "expected this to compile" without saying why is a test that will
 * cost an hour later. */
static void ok(const char *src, glsl_shader_kind_t kind, const char *name) {
    glsl_unit_t *u = glsl_compile(src, kind);
    tn++;
    if (u && u->compiled) {
        passed++;
    } else {
        failed++;
        printf("  FAIL: %s\n", name);
        if (u) printf("        log: %s", glsl_unit_log(u));
    }
    glsl_unit_free(u);
}

/* Compile and assert failure, with `want` appearing in the log. */
static void bad(const char *src, glsl_shader_kind_t kind,
                const char *want, const char *name) {
    glsl_unit_t *u = glsl_compile(src, kind);
    tn++;
    if (!u) { failed++; printf("  FAIL: %s (no unit)\n", name); return; }

    if (u->compiled) {
        failed++;
        printf("  FAIL: %s — expected a diagnostic, compiled cleanly\n", name);
    } else if (want && !strstr(glsl_unit_log(u), want)) {
        failed++;
        printf("  FAIL: %s — log lacks \"%s\"\n", name, want);
        printf("        got: %s", glsl_unit_log(u));
    } else {
        passed++;
    }
    glsl_unit_free(u);
}

#define VS GLSL_SHADER_VERTEX
#define FS GLSL_SHADER_FRAGMENT

/* A minimal well-formed body for each stage, so a test can focus on one
 * construct without repeating the boilerplate that keeps the shader legal. */
#define VS_WRAP(body) \
    "void main() {\n" body "\n  gl_Position = vec4(0.0);\n}\n"
#define FS_WRAP(body) \
    "void main() {\n" body "\n  gl_FragColor = vec4(1.0);\n}\n"

/* ============================================================================
 * Lexer
 * ==========================================================================*/

static void test_lexer(void) {
    printf("--- lexer ---\n");

    ok(FS_WRAP("  // a line comment\n  /* a block\n     comment */"),
       FS, "comments are skipped");

    ok("#version 100\n" FS_WRAP("  float f = 1.0;"),
       FS, "#version is accepted");

    ok("#extension GL_OES_standard_derivatives : enable\n" FS_WRAP(""),
       FS, "#extension is accepted");

    bad("#define TWO 2\n" FS_WRAP(""), FS,
        "preprocessor", "#define is refused, not silently ignored");

    bad(FS_WRAP("  /* never closed"), FS,
        "unterminated comment", "unterminated block comment");

    /* Number spelling decides the type, and the type checker then enforces
     * the absence of conversions — so this is really a lexer test with a
     * semantic assertion. */
    ok(FS_WRAP("  float a = 1.0; float b = 1.; float c = .5;\n"
               "  float d = 1e3; float e = 1.5E-2; float g = 2.0f;"),
       FS, "float literal spellings");

    ok(FS_WRAP("  int a = 42; int b = 0x2A;"),
       FS, "integer literal spellings");

    bad(FS_WRAP("  int i = 1; i = i << 2;"), FS,
        "shift", "shift operators are diagnosed by name");

    bad(FS_WRAP("  int i = 1 & 2;"), FS,
        "bitwise", "bitwise operators are diagnosed by name");

    bad(FS_WRAP("  float f = 1.0; f = f $ 2.0;"), FS,
        "unexpected character", "an unknown character is reported");

    bad("double d;\n" FS_WRAP(""), FS,
        "reserved word", "reserved words are named in the diagnostic");

    /* Line numbers are the point of the lexer's bookkeeping. */
    {
        glsl_unit_t *u = glsl_compile(
            "void main() {\n"
            "  float a = 1.0;\n"
            "  float b = 2.0;\n"
            "  nope = 3.0;\n"
            "  gl_FragColor = vec4(1.0);\n"
            "}\n", FS);
        CHECK(u && !u->compiled && strstr(glsl_unit_log(u), "0:4:"),
              "diagnostic carries the right line number");
        glsl_unit_free(u);
    }
}

/* ============================================================================
 * Parser
 * ==========================================================================*/

static void test_parser(void) {
    printf("--- parser ---\n");

    ok(FS_WRAP("  float a = 1.0, b = 2.0, c;"),
       FS, "multiple declarators in one statement");

    ok(FS_WRAP("  if (true) { } else if (false) { } else { }"),
       FS, "if / else if / else");

    ok(FS_WRAP("  for (int i = 0; i < 4; i++) { }"),
       FS, "for loop with a declaration initialiser");

    ok(FS_WRAP("  int i = 0;\n  while (i < 4) { i++; }\n"
               "  do { i--; } while (i > 0);"),
       FS, "while and do-while");

    ok(FS_WRAP("  for (int i = 0; i < 4; i++) {\n"
               "    if (i == 2) continue;\n"
               "    if (i == 3) break;\n  }"),
       FS, "break and continue inside a loop");

    ok("float square(float x) { return x * x; }\n"
       FS_WRAP("  float f = square(2.0);"),
       FS, "user function definition and call");

    ok("float later(float x);\n"
       "void main() { gl_FragColor = vec4(later(1.0)); }\n"
       "float later(float x) { return x; }\n",
       FS, "prototype before definition");

    ok("struct Light { vec3 pos; float power; };\n"
       "uniform Light uLight;\n"
       FS_WRAP("  vec3 p = uLight.pos * uLight.power;"),
       FS, "struct declaration and member access");

    ok("uniform vec3 uPos[4];\n"
       FS_WRAP("  vec3 v = uPos[2];"),
       FS, "array declaration and indexing");

    ok(FS_WRAP("  float f = true ? 1.0 : 2.0;"),
       FS, "conditional expression");

    ok(FS_WRAP("  vec4 v = vec4(1.0);\n  v.xy = vec2(2.0);\n"
               "  float f = v.rgb.b + v.stp.s;"),
       FS, "swizzles in all three alphabets");

    /* Operator precedence, checked through the type system: if `+` bound
     * tighter than `*` this would be vec2 * (vec2 + vec2) and still typecheck,
     * so the assertion is on a case where precedence changes the TYPE. */
    ok(FS_WRAP("  bool b = 1.0 + 2.0 * 3.0 > 5.0 && true;"),
       FS, "precedence: arithmetic before comparison before &&");

    bad(FS_WRAP("  float f = 1.0"), FS,
        "expected ';'", "missing semicolon names the expected token");

    bad(FS_WRAP("  if (true) { "), FS,
        "expected", "unclosed brace is reported");

    bad("void main() { gl_FragColor = vec4(1.0; }\n", FS,
        "expected ')'", "missing close paren names the expected token");

    bad(FS_WRAP("  float f = ;"), FS,
        "expected expression", "missing operand");

    /* Error recovery: two independent mistakes must both be reported, or the
     * author fixes one and recompiles to find the next. */
    {
        glsl_unit_t *u = glsl_compile(
            "void main() {\n"
            "  float a = ;\n"
            "  float b = ;\n"
            "  gl_FragColor = vec4(1.0);\n"
            "}\n", FS);
        CHECK(u && !u->compiled && u->error_count >= 2,
              "recovery reports more than one error");
        glsl_unit_free(u);
    }

    /* Untrusted input must not blow the stack. */
    {
        char *src = (char *)malloc(8192);
        strcpy(src, "void main() { float f = ");
        for (int i = 0; i < 200; i++) strcat(src, "(");
        strcat(src, "1.0");
        for (int i = 0; i < 200; i++) strcat(src, ")");
        strcat(src, "; gl_FragColor = vec4(f); }\n");
        glsl_unit_t *u = glsl_compile(src, FS);
        CHECK(u != NULL, "deep nesting terminates instead of overflowing");
        CHECK(u && !u->compiled && strstr(glsl_unit_log(u), "nested"),
              "deep nesting is diagnosed as such");
        glsl_unit_free(u);
        free(src);
    }
}

/* ============================================================================
 * The type system
 * ==========================================================================*/

static void test_types(void) {
    printf("--- types and conversions ---\n");

    /* The rule that catches everyone: no implicit conversions at all. */
    bad(FS_WRAP("  float f = 1;"), FS,
        "no implicit conversions", "int literal does not become a float");

    bad(FS_WRAP("  int i = 1.0;"), FS,
        "no implicit conversions", "float literal does not become an int");

    bad(FS_WRAP("  vec3 v = vec3(1.0); v = v * 2;"), FS,
        "no implicit conversions", "vec3 * int is refused");

    ok(FS_WRAP("  vec3 v = vec3(1.0); v = v * 2.0;"),
       FS, "vec3 * float is component-wise");

    ok(FS_WRAP("  vec3 a = vec3(1.0); vec3 b = a + a * a;"),
       FS, "vector arithmetic is component-wise");

    bad(FS_WRAP("  vec3 a = vec3(1.0); vec2 b = vec2(1.0); vec3 c = a + b;"),
        FS, "cannot combine", "mismatched vector sizes are refused");

    /* Matrix multiply is the one operator whose result is neither operand. */
    ok("uniform mat4 m;\n" VS_WRAP("  vec4 v = m * vec4(1.0);"),
       VS, "mat4 * vec4 yields vec4");
    ok("uniform mat3 m;\n" FS_WRAP("  mat3 n = m * m;"),
       FS, "mat3 * mat3 yields mat3");
    bad("uniform mat3 m;\n" FS_WRAP("  vec4 v = m * vec4(1.0);"),
        FS, "cannot multiply", "mat3 * vec4 is a dimension error");

    bad(FS_WRAP("  bool b = true + true;"), FS,
        "arithmetic on bool", "arithmetic on bool is refused");

    bad(FS_WRAP("  int i = 5 % 2;"), FS,
        "mod()", "the %% operator is refused with a pointer to mod()");

    /* Comparison rules. */
    ok(FS_WRAP("  bool b = vec3(1.0) == vec3(2.0);"),
       FS, "== on vectors yields one bool");
    bad(FS_WRAP("  bool b = vec3(1.0) < vec3(2.0);"), FS,
        "lessThan", "< on vectors points at lessThan()");
    bad(FS_WRAP("  bool b = 1 < 2.0;"), FS,
        "no implicit conversions", "< between int and float is refused");
    bad(FS_WRAP("  bool b = vec3(1.0) == vec2(1.0);"), FS,
        "cannot compare", "comparing different types is refused");

    /* Logical operators are bool-only. */
    bad(FS_WRAP("  bool b = 1.0 && true;"), FS,
        "logical operator", "&& on a float is refused");
    bad(FS_WRAP("  bool b = !1.0;"), FS,
        "'!' requires a bool", "! on a float is refused");

    /* Conditions must be bool, not "anything nonzero". */
    bad(FS_WRAP("  if (1.0) { }"), FS,
        "must be bool", "a float condition is refused");
    bad(FS_WRAP("  int i = 0; while (i) { }"), FS,
        "must be bool", "an int loop condition is refused");
    bad(FS_WRAP("  float f = 1.0 ? 2.0 : 3.0;"), FS,
        "must be bool", "a float ?: condition is refused");
    bad(FS_WRAP("  float f = true ? 1.0 : 2;"), FS,
        "same type", "?: branches of different types are refused");
}

static void test_constructors(void) {
    printf("--- constructors ---\n");

    ok(FS_WRAP("  vec4 a = vec4(1.0);\n"
               "  vec4 b = vec4(1.0, 2.0, 3.0, 4.0);\n"
               "  vec4 c = vec4(vec2(1.0), vec2(2.0));\n"
               "  vec4 d = vec4(vec3(1.0), 1.0);\n"
               "  vec3 e = vec3(a.xy, 0.0);"),
       FS, "vector constructors flatten their arguments");

    ok(FS_WRAP("  vec3 v = vec3(1);"),
       FS, "a single scalar converts in a constructor");

    ok(FS_WRAP("  mat4 m = mat4(1.0);\n  mat3 n = mat3(m);"),
       FS, "matrix constructors");

    ok(FS_WRAP("  float f = float(1);\n  int i = int(1.5);\n"
               "  bool b = bool(1);"),
       FS, "scalar conversion constructors");

    bad(FS_WRAP("  vec4 v = vec4(1.0, 2.0);"), FS,
        "needs 4 components", "too few components names the shortfall");

    bad(FS_WRAP("  vec2 v = vec2(1.0, 2.0, 3.0, 4.0);"), FS,
        "unused argument", "a wholly redundant argument is refused");

    bad(FS_WRAP("  vec4 v = vec4();"), FS,
        "needs arguments", "an empty constructor is refused");

    bad("uniform sampler2D s;\n" FS_WRAP("  vec4 v = vec4(s);"), FS,
        "sampler", "a sampler cannot be constructed from");

    ok("struct P { float a; vec2 b; };\n"
       FS_WRAP("  P p = P(1.0, vec2(2.0));\n  float f = p.a + p.b.x;"),
       FS, "struct constructor");

    bad("struct P { float a; vec2 b; };\n"
        FS_WRAP("  P p = P(1.0);"), FS,
        "expects 2 argument", "a struct constructor checks its arity");

    bad("struct P { float a; vec2 b; };\n"
        FS_WRAP("  P p = P(1.0, 2.0);"), FS,
        "expects 'vec2'", "a struct constructor checks field types");
}

static void test_swizzles(void) {
    printf("--- swizzles ---\n");

    ok(FS_WRAP("  vec4 v = vec4(1.0);\n"
               "  float a = v.x; vec2 b = v.xy; vec3 c = v.xyz;\n"
               "  vec4 d = v.wzyx; vec4 e = v.xxxx;"),
       FS, "swizzles of every length, including repeats");

    bad(FS_WRAP("  vec2 v = vec2(1.0); float f = v.z;"), FS,
        "beyond 'vec2'", "a swizzle past the end names the type");

    bad(FS_WRAP("  vec4 v = vec4(1.0); vec2 f = v.xr;"), FS,
        "not a valid swizzle", "mixing xyzw with rgba is refused");

    bad(FS_WRAP("  vec4 v = vec4(1.0); float f = v.q2;"), FS,
        "not a valid swizzle", "a non-component letter is refused");

    ok(FS_WRAP("  vec4 v = vec4(1.0);\n  v.xy = vec2(2.0);\n"
               "  v.w = 3.0;"),
       FS, "a swizzle with distinct components is assignable");

    bad(FS_WRAP("  vec4 v = vec4(1.0); v.xx = vec2(2.0);"), FS,
        "not assignable", "a repeated swizzle is not assignable");

    /* Swizzle result types must be exact, since there are no conversions. */
    bad(FS_WRAP("  vec4 v = vec4(1.0); vec3 c = v.xy;"), FS,
        "cannot initialise", "a swizzle's length determines its type");

    ok("uniform ivec4 iv;\n" FS_WRAP("  ivec2 a = iv.xy; int b = iv.z;"),
       FS, "swizzling an integer vector keeps the element type");
}

/* ============================================================================
 * Scopes, qualifiers and lvalues
 * ==========================================================================*/

static void test_scopes(void) {
    printf("--- scopes and lvalues ---\n");

    ok(FS_WRAP("  float f = 1.0;\n  { float f = 2.0; }\n"),
       FS, "an inner scope may shadow an outer one");

    bad(FS_WRAP("  float f = 1.0; float f = 2.0;"), FS,
        "already declared", "redeclaring in one scope is refused");

    bad(FS_WRAP("  { float g = 1.0; }\n  float h = g;"), FS,
        "not declared", "a name does not escape its block");

    ok(FS_WRAP("  for (int i = 0; i < 2; i++) { }\n"
               "  for (int i = 0; i < 2; i++) { }"),
       FS, "a for-loop variable does not leak");

    bad(FS_WRAP("  for (int i = 0; i < 2; i++) { }\n  int j = i;"), FS,
        "not declared", "a for-loop variable is scoped to the loop");

    bad("uniform float u;\n" FS_WRAP("  u = 1.0;"), FS,
        "not assignable", "a uniform cannot be written");

    bad(FS_WRAP("  const float c = 1.0; c = 2.0;"), FS,
        "not assignable", "a const cannot be written");

    bad("attribute vec4 a;\n" VS_WRAP("  a = vec4(1.0);"), VS,
        "not assignable", "an attribute cannot be written");

    bad(FS_WRAP("  1.0 = 2.0;"), FS,
        "not assignable", "a literal cannot be assigned to");

    bad(FS_WRAP("  float f = 1.0; (f + f) = 2.0;"), FS,
        "not assignable", "an expression result cannot be assigned to");

    bad(FS_WRAP("  const float c;"), FS,
        "must be initialised", "an uninitialised const is refused");

    bad(FS_WRAP("  float f = 1.0; const float c = f;"), FS,
        "not a constant expression", "a const needs a constant initialiser");

    ok(FS_WRAP("  const float c = 1.0 + 2.0;"),
       FS, "constant folding is not required, only constness");

    bad(FS_WRAP("  float gl_Thing = 1.0;"), FS,
        "reserved", "declaring a gl_ name is refused");
}

static void test_qualifiers(void) {
    printf("--- storage qualifiers ---\n");

    ok("attribute vec4 aPos;\nuniform mat4 uM;\nvarying vec2 vUV;\n"
       "void main() { vUV = aPos.xy; gl_Position = uM * aPos; }\n",
       VS, "a realistic vertex shader");

    ok("precision mediump float;\nvarying vec2 vUV;\nuniform sampler2D uT;\n"
       "void main() { gl_FragColor = texture2D(uT, vUV); }\n",
       FS, "a realistic fragment shader");

    bad("attribute vec4 a;\n" FS_WRAP(""), FS,
        "fragment shader", "attribute in a fragment shader is refused");

    bad(FS_WRAP("  uniform float u;"), FS,
        "global variables", "a local uniform is refused");

    bad("attribute int a;\n" VS_WRAP(""), VS,
        "must be float", "an integer attribute is refused");

    bad("varying int v;\n" FS_WRAP(""), FS,
        "must be float", "an integer varying is refused");

    bad("uniform float u = 1.0;\n" FS_WRAP(""), FS,
        "cannot have an initialiser", "an initialised uniform is refused");

    bad("sampler2D s;\n" FS_WRAP(""), FS,
        "must be declared uniform", "a non-uniform sampler is refused");

    bad(FS_WRAP("  sampler2D s;"), FS,
        "cannot be a local", "a local sampler is refused");

    /* A fragment shader reads varyings; a vertex shader writes them. */
    bad("varying vec2 v;\n" FS_WRAP("  v = vec2(1.0);"), FS,
        "not assignable", "a fragment shader cannot write a varying");
    ok("varying vec2 v;\n" VS_WRAP("  v = vec2(1.0);"),
       VS, "a vertex shader can write a varying");
}

/* ============================================================================
 * Functions
 * ==========================================================================*/

static void test_functions(void) {
    printf("--- functions ---\n");

    ok("vec3 scale(vec3 v, float k) { return v * k; }\n"
       FS_WRAP("  vec3 r = scale(vec3(1.0), 2.0);"),
       FS, "a user function with several parameters");

    bad("float f(float x) { return x; }\n"
        FS_WRAP("  float r = f(1.0, 2.0);"), FS,
        "expects 1 argument", "wrong argument count names the arity");

    bad("float f(float x) { return x; }\n"
        FS_WRAP("  float r = f(1);"), FS,
        "expects 'float'", "wrong argument type names both types");

    bad("float f(float x) { }\n" FS_WRAP("  float r = f(1.0);"), FS,
        "not all control paths", "a missing return is diagnosed");

    ok("float f(float x) { if (x > 0.0) return x; else return -x; }\n"
       FS_WRAP("  float r = f(1.0);"),
       FS, "returns in both branches satisfy the check");

    bad("void f() { return 1.0; }\n" FS_WRAP("  f();"), FS,
        "void function", "returning a value from void is refused");

    bad("float f() { return; }\n" FS_WRAP("  float r = f();"), FS,
        "no value", "returning nothing from a non-void is refused");

    bad("float f(float x) { return x; }\n"
        "float f(float y) { return y; }\n"
        FS_WRAP("  float r = f(1.0);"), FS,
        "already defined", "a duplicate definition is refused");

    bad(FS_WRAP("  float r = nosuch(1.0);"), FS,
        "not declared", "calling an unknown function is refused");

    bad("float v = 1.0;\n" FS_WRAP("  float r = v(1.0);"), FS,
        "not a function", "calling a variable is refused");

    bad("float f(float x) { return x; }\n" FS_WRAP("  float r = f;"), FS,
        "is a function", "using a function as a value is refused");

    bad("float f(float x, float x) { return x; }\n" FS_WRAP("  f(1.0);"), FS,
        "duplicate parameter", "duplicate parameter names are refused");

    /* out parameters need something to write back into. */
    ok("void get(out float v) { v = 1.0; }\n"
       FS_WRAP("  float f; get(f);"),
       FS, "an out parameter accepts an lvalue");

    bad("void get(out float v) { v = 1.0; }\n"
        FS_WRAP("  get(1.0);"), FS,
        "must be assignable", "an out parameter refuses a literal");

    bad("int main() { return 0; }\n", VS,
        "must return void", "main must return void");

    bad("void main(float x) { gl_Position = vec4(x); }\n", VS,
        "no parameters", "main must take no parameters");

    bad("float f(float x) { return x; }\n", FS,
        "no 'main'", "a shader without main is refused");
}

/* ============================================================================
 * Built-in functions and variables
 * ==========================================================================*/

static void test_builtins(void) {
    printf("--- built-ins ---\n");

    ok(FS_WRAP("  float a = sin(1.0); vec3 b = cos(vec3(1.0));\n"
               "  vec2 c = abs(vec2(-1.0)); float d = sqrt(2.0);"),
       FS, "genType functions accept float and any vector");

    ok(FS_WRAP("  float a = length(vec3(1.0));\n"
               "  float b = dot(vec3(1.0), vec3(2.0));\n"
               "  float c = distance(vec2(0.0), vec2(1.0));"),
       FS, "geometric functions collapse to a scalar");

    ok(FS_WRAP("  vec3 c = cross(vec3(1.0), vec3(2.0));\n"
               "  vec3 n = normalize(vec3(1.0));\n"
               "  vec3 r = reflect(vec3(1.0), n);"),
       FS, "cross, normalize and reflect");

    ok(FS_WRAP("  vec3 a = mix(vec3(0.0), vec3(1.0), 0.5);\n"
               "  vec3 b = mix(vec3(0.0), vec3(1.0), vec3(0.5));\n"
               "  vec3 c = clamp(vec3(2.0), 0.0, 1.0);\n"
               "  float d = smoothstep(0.0, 1.0, 0.5);"),
       FS, "mix and clamp accept a scalar or a matching vector");

    ok(FS_WRAP("  bvec3 b = lessThan(vec3(1.0), vec3(2.0));\n"
               "  bool c = any(b); bool d = all(b); bvec3 e = not(b);"),
       FS, "vector relational functions and their bvec results");

    ok("uniform sampler2D t;\nuniform samplerCube c;\n"
       FS_WRAP("  vec4 a = texture2D(t, vec2(0.5));\n"
               "  vec4 b = textureCube(c, vec3(1.0));"),
       FS, "texture lookups");

    bad(FS_WRAP("  float f = sin();"), FS,
        "expects 1 argument", "a built-in checks its arity");

    bad(FS_WRAP("  float f = sin(1.0, 2.0);"), FS,
        "expects 1 argument", "a built-in rejects extra arguments");

    bad(FS_WRAP("  float f = sin(1);"), FS,
        "must be float", "a built-in rejects an int argument");

    bad(FS_WRAP("  bool b = true; bool c = any(b);"), FS,
        "argument", "any() needs a bvec, not a bool");

    bad(FS_WRAP("  float f = dot(vec3(1.0), vec2(1.0));"), FS,
        "same type", "dot() requires matching vector sizes");

    bad("uniform sampler2D t;\n" FS_WRAP("  vec4 v = texture2D(t, vec3(1.0));"),
        FS, "expects 'vec2'", "texture2D checks its coordinate type");

    bad("uniform samplerCube c;\n"
        FS_WRAP("  vec4 v = texture2D(c, vec2(0.5));"), FS,
        "expects 'sampler2D'", "texture2D refuses a cube sampler");

    /* Built-in variables belong to their stage. */
    ok(VS_WRAP("  gl_PointSize = 2.0;"), VS, "gl_PointSize in a vertex shader");
    bad(VS_WRAP("  gl_FragColor = vec4(1.0);"), VS,
        "not declared", "gl_FragColor is not visible to a vertex shader");
    bad(FS_WRAP("  gl_Position = vec4(1.0);"), FS,
        "not declared", "gl_Position is not visible to a fragment shader");

    ok("void main() { if (gl_FragCoord.x > 0.5) discard;\n"
       "  gl_FragColor = vec4(gl_FrontFacing ? 1.0 : 0.0); }\n",
       FS, "gl_FragCoord and gl_FrontFacing");

    bad(FS_WRAP("  gl_FragCoord = vec4(1.0);"), FS,
        "not assignable", "gl_FragCoord is read-only");
}

/* ============================================================================
 * Stage rules and the whole-shader checks
 * ==========================================================================*/

static void test_stage_rules(void) {
    printf("--- stage rules ---\n");

    ok("void main() { discard; }\n", FS,
       "a fragment shader may only discard");

    bad(VS_WRAP("  discard;"), VS,
        "fragment shader", "discard in a vertex shader is refused");

    bad("void main() { }\n", VS,
        "never writes gl_Position", "a vertex shader must write gl_Position");

    bad("void main() { }\n", FS,
        "never writes gl_FragColor", "a fragment shader must write a colour");

    /* Writing through a swizzle or from a helper still counts. */
    ok("void setPos(vec4 p) { gl_Position = p; }\n"
       "void main() { setPos(vec4(1.0)); }\n",
       VS, "gl_Position written from a helper function");

    ok("void main() { gl_Position.xyz = vec3(1.0); gl_Position.w = 1.0; }\n",
       VS, "gl_Position written through swizzles");

    bad(FS_WRAP("  break;"), FS,
        "outside a loop", "break outside a loop is refused");

    bad(FS_WRAP("  continue;"), FS,
        "outside a loop", "continue outside a loop is refused");

    ok(FS_WRAP("  for (int i = 0; i < 2; i++) { if (true) break; }"),
       FS, "break inside a nested if inside a loop");
}

/* ============================================================================
 * Arrays and indexing
 * ==========================================================================*/

static void test_arrays(void) {
    printf("--- arrays and indexing ---\n");

    ok("uniform vec4 uColors[4];\n"
       FS_WRAP("  vec4 c = uColors[1];"),
       FS, "indexing a uniform array");

    ok("uniform mat4 m;\n" FS_WRAP("  vec4 col = m[2]; float e = m[1][3];"),
       FS, "indexing a matrix gives a column, then a scalar");

    ok(FS_WRAP("  vec4 v = vec4(1.0); float f = v[2];"),
       FS, "indexing a vector gives its element type");

    bad("uniform vec4 c[4];\n" FS_WRAP("  vec4 v = c[4];"), FS,
        "out of range", "a constant index past the end is caught");

    bad("uniform vec4 c[4];\n" FS_WRAP("  vec4 v = c[-1];"), FS,
        "out of range", "a negative constant index is caught");

    bad(FS_WRAP("  vec2 v = vec2(1.0); float f = v[3];"), FS,
        "out of range", "indexing a vector past its size is caught");

    bad("uniform vec4 c[4];\n" FS_WRAP("  vec4 v = c[1.0];"), FS,
        "must be int", "a float index is refused");

    bad(FS_WRAP("  float f = 1.0; float g = f[0];"), FS,
        "cannot index", "indexing a scalar is refused");

    bad("uniform float u[0];\n" FS_WRAP(""), FS,
        "must be positive", "a zero-length array is refused");

    /* An array is not assignable as a whole in GLSL ES 1.0, and its elements
     * carry the base type. */
    ok("uniform vec3 a[2];\n" FS_WRAP("  vec3 v = a[0] + a[1];"),
       FS, "array elements have the base type");
}

/* ============================================================================
 * Realistic shaders, end to end
 * ==========================================================================*/

static void test_realistic(void) {
    printf("--- realistic shaders ---\n");

    /* The shader the G11c pipeline will use as its smoke test. */
    ok("attribute vec4 aPosition;\n"
       "attribute vec3 aNormal;\n"
       "attribute vec2 aTexCoord;\n"
       "uniform mat4 uModelView;\n"
       "uniform mat4 uProjection;\n"
       "uniform mat3 uNormalMatrix;\n"
       "varying vec3 vNormal;\n"
       "varying vec2 vTexCoord;\n"
       "varying vec3 vEyePos;\n"
       "void main() {\n"
       "  vec4 eye = uModelView * aPosition;\n"
       "  vEyePos = eye.xyz;\n"
       "  vNormal = normalize(uNormalMatrix * aNormal);\n"
       "  vTexCoord = aTexCoord;\n"
       "  gl_Position = uProjection * eye;\n"
       "}\n",
       VS, "a full transform-and-light vertex shader");

    ok("precision mediump float;\n"
       "varying vec3 vNormal;\n"
       "varying vec2 vTexCoord;\n"
       "varying vec3 vEyePos;\n"
       "uniform sampler2D uTexture;\n"
       "uniform vec3 uLightPos;\n"
       "uniform vec3 uLightColor;\n"
       "uniform float uShininess;\n"
       "\n"
       "vec3 blinnPhong(vec3 n, vec3 l, vec3 v, vec3 base) {\n"
       "  float ndl = max(dot(n, l), 0.0);\n"
       "  vec3 h = normalize(l + v);\n"
       "  float spec = pow(max(dot(n, h), 0.0), uShininess);\n"
       "  return base * ndl + uLightColor * spec;\n"
       "}\n"
       "\n"
       "void main() {\n"
       "  vec3 n = normalize(vNormal);\n"
       "  vec3 l = normalize(uLightPos - vEyePos);\n"
       "  vec3 v = normalize(-vEyePos);\n"
       "  vec4 tex = texture2D(uTexture, vTexCoord);\n"
       "  if (tex.a < 0.1) discard;\n"
       "  vec3 lit = blinnPhong(n, l, v, tex.rgb);\n"
       "  gl_FragColor = vec4(lit, tex.a);\n"
       "}\n",
       FS, "a full Blinn-Phong fragment shader");

    /* Loops with real work in them, which G11b will have to execute. */
    ok("precision mediump float;\n"
       "uniform vec3 uLightPos[4];\n"
       "uniform vec3 uLightColor[4];\n"
       "varying vec3 vNormal;\n"
       "varying vec3 vEyePos;\n"
       "void main() {\n"
       "  vec3 n = normalize(vNormal);\n"
       "  vec3 sum = vec3(0.0);\n"
       "  for (int i = 0; i < 4; i++) {\n"
       "    vec3 d = uLightPos[i] - vEyePos;\n"
       "    float atten = 1.0 / (1.0 + dot(d, d) * 0.01);\n"
       "    sum += uLightColor[i] * max(dot(n, normalize(d)), 0.0) * atten;\n"
       "  }\n"
       "  gl_FragColor = vec4(sum, 1.0);\n"
       "}\n",
       FS, "a four-light accumulation loop");

    /* A procedural shader with no inputs at all: the other extreme. */
    ok("precision mediump float;\n"
       "uniform float uTime;\n"
       "void main() {\n"
       "  vec2 p = gl_FragCoord.xy * 0.01;\n"
       "  float v = sin(p.x + uTime) * cos(p.y - uTime);\n"
       "  gl_FragColor = vec4(vec3(v * 0.5 + 0.5), 1.0);\n"
       "}\n",
       FS, "a procedural fragment shader");
}

/* ============================================================================
 * Robustness
 * ==========================================================================*/

static void test_robustness(void) {
    printf("--- robustness ---\n");

    {
        glsl_unit_t *u = glsl_compile("", FS);
        CHECK(u && !u->compiled, "an empty shader fails rather than crashing");
        CHECK(u && strlen(glsl_unit_log(u)) > 0, "an empty shader has a log");
        glsl_unit_free(u);
    }

    CHECK(glsl_compile(NULL, FS) == NULL, "a NULL source returns NULL");

    {
        /* Only whitespace and comments. */
        glsl_unit_t *u = glsl_compile("  \n\t/* nothing */\n", VS);
        CHECK(u && !u->compiled, "a comment-only shader fails cleanly");
        glsl_unit_free(u);
    }

    {
        /* Unterminated everything, to exercise the recovery paths together. */
        glsl_unit_t *u = glsl_compile("void main() { vec4 v = vec4(1.0", FS);
        CHECK(u != NULL, "truncated source does not crash");
        CHECK(u && !u->compiled, "truncated source fails");
        glsl_unit_free(u);
    }

    {
        /* Many errors: the log must stay bounded and say how many were
         * dropped, rather than silently truncating. */
        char *src = (char *)malloc(65536);
        strcpy(src, "void main() {\n");
        for (int i = 0; i < 200; i++) strcat(src, "  undeclared_name_x = 1;\n");
        strcat(src, "  gl_FragColor = vec4(1.0);\n}\n");
        glsl_unit_t *u = glsl_compile(src, FS);
        CHECK(u && !u->compiled, "many errors still fails");
        CHECK(u && strlen(glsl_unit_log(u)) < GLSL_MAX_LOG,
              "the log stays within its bound");
        CHECK(u && u->error_count > u->diag_count &&
              strstr(glsl_unit_log(u), "more error"),
              "dropped diagnostics are counted in the log");
        glsl_unit_free(u);
        free(src);
    }

    {
        /* Source over the size limit is refused by size, not by parsing. */
        size_t big = GLSL_MAX_SOURCE + 100;
        char *src = (char *)malloc(big + 1);
        memset(src, ' ', big);
        src[big] = '\0';
        glsl_unit_t *u = glsl_compile(src, FS);
        CHECK(u && !u->compiled && strstr(glsl_unit_log(u), "exceeds"),
              "oversized source is refused by size");
        glsl_unit_free(u);
        free(src);
    }

    {
        /* A long but legal shader must still compile: the limits must not be
         * so tight that real work hits them. */
        char *src = (char *)malloc(65536);
        strcpy(src, "precision mediump float;\nvoid main() {\n  float acc = 0.0;\n");
        for (int i = 0; i < 300; i++) {
            strcat(src, "  acc += sin(acc) * 0.5;\n");
        }
        strcat(src, "  gl_FragColor = vec4(acc);\n}\n");
        glsl_unit_t *u = glsl_compile(src, FS);
        tn++;
        if (u && u->compiled) passed++;
        else {
            failed++;
            printf("  FAIL: a 300-statement shader compiles\n");
            if (u) printf("        log: %s", glsl_unit_log(u));
        }
        glsl_unit_free(u);
        free(src);
    }

    {
        /* Compiling many shaders in a row must not leak the arena.  Run under
         * a leak checker this is the test that catches a missing free. */
        for (int i = 0; i < 200; i++) {
            glsl_unit_t *u = glsl_compile(FS_WRAP("  float f = sin(1.0);"), FS);
            glsl_unit_free(u);
        }
        CHECK(1, "repeated compilation does not exhaust memory");
    }

    {
        /* A successful compile must leave an EMPTY log: applications print it
         * unconditionally, and a spurious warning looks like a failure. */
        glsl_unit_t *u = glsl_compile(FS_WRAP("  float f = 1.0;"), FS);
        CHECK(u && u->compiled && glsl_unit_log(u)[0] == '\0',
              "a clean compile has an empty log");
        glsl_unit_free(u);
    }
}

/* ============================================================================
 * Driver
 * ==========================================================================*/

int main(void) {
    printf("=== test_glsl: GLSL ES 1.0 front end (phase G11a) ===\n");

    test_lexer();
    test_parser();
    test_types();
    test_constructors();
    test_swizzles();
    test_scopes();
    test_qualifiers();
    test_functions();
    test_builtins();
    test_stage_rules();
    test_arrays();
    test_realistic();
    test_robustness();

    printf("\ntest_glsl: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}
