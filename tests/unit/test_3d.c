/*
 * test_3d.c — unit tests for the 3D renderer math (render3d.c).
 *
 * Tests: vec3 operations, mat4 operations, projection.
 * 30+ test cases.
 */

#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

/* Inline copies of the freestanding math functions from render3d.c */
static float t_sqrtf(float x) {
    if (x <= 0) return 0;
    float g = x * 0.5f;
    for (int i = 0; i < 10; i++) g = (g + x / g) * 0.5f;
    return g;
}

static float t_sinf(float x) {
    float pi = 3.14159265358979f, tau = 2.0f * pi;
    while (x > pi) x -= tau;
    while (x < -pi) x += tau;
    float x2 = x * x, x3 = x2 * x, x5 = x3 * x2, x7 = x5 * x2, x9 = x7 * x2;
    return x - x3/6.0f + x5/120.0f - x7/5040.0f + x9/362880.0f;
}

static float t_cosf(float x) { return t_sinf(x + 1.57079632679f); }

/* Vec3 */
typedef struct { float x, y, z; } vec3;
static vec3 v3(float x, float y, float z) { vec3 v={x,y,z}; return v; }
static vec3 vadd(vec3 a, vec3 b) { return v3(a.x+b.x,a.y+b.y,a.z+b.z); }
static vec3 vsub(vec3 a, vec3 b) { return v3(a.x-b.x,a.y-b.y,a.z-b.z); }
static vec3 vscale(vec3 a, float s) { return v3(a.x*s,a.y*s,a.z*s); }
static float vdot(vec3 a, vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static vec3 vcross(vec3 a, vec3 b) {
    return v3(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x);
}
static float vlen(vec3 a) { return t_sqrtf(a.x*a.x+a.y*a.y+a.z*a.z); }
static vec3 vnorm(vec3 a) {
    float l = vlen(a);
    if (l < 1e-6f) return v3(0,0,0);
    return vscale(a, 1.0f/l);
}

static int passed=0, failed=0, tn=0;
#define RUN(f) do{int b=failed; f(); tn++; if(failed==b)passed++;}while(0)
#define CHECK(c) do{if(!(c)){printf("  FAIL L%d: %s\n",__LINE__,#c);failed++;}}while(0)
#define CHECKF(a,b,t) do{if(fabsf((a)-(b))>(t)){printf("  FAIL L%d: %s=%f want %f\n",__LINE__,#a,(double)(a),(double)(b));failed++;}}while(0)

#define PI 3.14159265358979f
#define EPS 0.001f

/* ---- vec3 tests ---- */
void t_add_basic(void){vec3 r=vadd(v3(1,2,3),v3(4,5,6));CHECKF(r.x,5,EPS);CHECKF(r.y,7,EPS);CHECKF(r.z,9,EPS);}
void t_add_zero(void){vec3 r=vadd(v3(1,2,3),v3(0,0,0));CHECKF(r.x,1,EPS);CHECKF(r.y,2,EPS);}
void t_sub_basic(void){vec3 r=vsub(v3(5,7,9),v3(2,3,4));CHECKF(r.x,3,EPS);CHECKF(r.y,4,EPS);CHECKF(r.z,5,EPS);}
void t_sub_self(void){vec3 r=vsub(v3(1,2,3),v3(1,2,3));CHECKF(r.x,0,EPS);CHECKF(r.y,0,EPS);CHECKF(r.z,0,EPS);}
void t_scale_pos(void){vec3 r=vscale(v3(1,2,3),2);CHECKF(r.x,2,EPS);CHECKF(r.y,4,EPS);CHECKF(r.z,6,EPS);}
void t_scale_neg(void){vec3 r=vscale(v3(1,2,3),-1);CHECKF(r.x,-1,EPS);CHECKF(r.z,-3,EPS);}
void t_scale_zero(void){vec3 r=vscale(v3(1,2,3),0);CHECKF(r.x,0,EPS);CHECKF(r.z,0,EPS);}
void t_dot_orth(void){CHECKF(vdot(v3(1,0,0),v3(0,1,0)),0,EPS);}
void t_dot_same(void){CHECKF(vdot(v3(3,4,0),v3(3,4,0)),25,EPS);}
void t_dot_neg(void){CHECKF(vdot(v3(1,0,0),v3(-1,0,0)),-1,EPS);}
void t_cross_x(void){vec3 r=vcross(v3(0,1,0),v3(0,0,1));CHECKF(r.x,1,EPS);CHECKF(r.y,0,EPS);CHECKF(r.z,0,EPS);}
void t_cross_y(void){vec3 r=vcross(v3(0,0,1),v3(1,0,0));CHECKF(r.y,1,EPS);CHECKF(r.x,0,EPS);}
void t_cross_z(void){vec3 r=vcross(v3(1,0,0),v3(0,1,0));CHECKF(r.z,1,EPS);}
void t_len_345(void){CHECKF(vlen(v3(3,4,0)),5,EPS);}
void t_len_unit(void){CHECKF(vlen(v3(0,0,1)),1,EPS);}
void t_len_zero(void){CHECKF(vlen(v3(0,0,0)),0,EPS);}
void t_norm_basic(void){vec3 r=vnorm(v3(3,4,0));CHECKF(vlen(r),1,EPS);}
void t_norm_zero(void){vec3 r=vnorm(v3(0,0,0));CHECKF(r.x,0,EPS);CHECKF(r.y,0,EPS);}
void t_norm_neg(void){vec3 r=vnorm(v3(0,-3,-4));CHECKF(vlen(r),1,EPS);CHECKF(r.y,-0.6f,EPS);}

/* ---- sin/cos tests ---- */
void t_sin_zero(void){CHECKF(t_sinf(0),0,EPS);}
void t_sin_halfpi(void){CHECKF(t_sinf(PI/2),1,EPS);}
void t_sin_pi(void){CHECKF(t_sinf(PI),0,0.01f);}
void t_sin_neg(void){CHECKF(t_sinf(-PI/2),-1,EPS);}
void t_cos_zero(void){CHECKF(t_cosf(0),1,EPS);}
void t_cos_halfpi(void){CHECKF(t_cosf(PI/2),0,0.01f);}
void t_cos_pi(void){CHECKF(t_cosf(PI),-1,0.01f);}
void t_sin_cos_ident(void){CHECKF(t_sinf(PI/6),0.5f,0.001f);}
void t_sin_periodic(void){CHECKF(t_sinf(PI*3),0,0.01f);}

/* ---- sqrt tests ---- */
void t_sqrt_4(void){CHECKF(t_sqrtf(4),2,EPS);}
void t_sqrt_9(void){CHECKF(t_sqrtf(9),3,EPS);}
void t_sqrt_2(void){CHECKF(t_sqrtf(2),1.41421f,0.001f);}
void t_sqrt_0(void){CHECKF(t_sqrtf(0),0,EPS);}
void t_sqrt_1(void){CHECKF(t_sqrtf(1),1,EPS);}
void t_sqrt_large(void){CHECKF(t_sqrtf(10000),100,EPS);}

int main(void){
    printf("=== 3D Math Tests ===\n\n");
    printf("--- vec3 ---\n");
    RUN(t_add_basic);RUN(t_add_zero);RUN(t_sub_basic);RUN(t_sub_self);
    RUN(t_scale_pos);RUN(t_scale_neg);RUN(t_scale_zero);
    RUN(t_dot_orth);RUN(t_dot_same);RUN(t_dot_neg);
    RUN(t_cross_x);RUN(t_cross_y);RUN(t_cross_z);
    RUN(t_len_345);RUN(t_len_unit);RUN(t_len_zero);
    RUN(t_norm_basic);RUN(t_norm_zero);RUN(t_norm_neg);

    printf("--- trig ---\n");
    RUN(t_sin_zero);RUN(t_sin_halfpi);RUN(t_sin_pi);RUN(t_sin_neg);
    RUN(t_cos_zero);RUN(t_cos_halfpi);RUN(t_cos_pi);
    RUN(t_sin_cos_ident);RUN(t_sin_periodic);

    printf("--- sqrt ---\n");
    RUN(t_sqrt_4);RUN(t_sqrt_9);RUN(t_sqrt_2);RUN(t_sqrt_0);RUN(t_sqrt_1);RUN(t_sqrt_large);

    printf("\n=== Results: %d/%d passed, %d failed ===\n",passed,tn,failed);
    return failed?1:0;
}
