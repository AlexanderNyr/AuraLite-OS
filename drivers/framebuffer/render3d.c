/* render3d.c — software 3D renderer.
 *
 * Implements vector math, 4x4 matrix operations, perspective projection,
 * wireframe drawing, and flat-shaded triangle rasterisation with painter's
 * algorithm depth sorting.
 *
 * All rendering goes through the graphics library's back buffer and must be
 * followed by gfx_flip() to make it visible.
 */

#include <stdint.h>
#include "drivers/framebuffer/render3d.h"
#include "drivers/framebuffer/graphics.h"
#include "drivers/timer/pit.h"

/* ---- Freestanding math (no <math.h> available in -ffreestanding mode) ---- */

static float r3d_sqrtf(float x) {
    /* Newton-Raphson iteration. */
    if (x <= 0) return 0;
    float guess = x * 0.5f;
    for (int i = 0; i < 10; i++) {
        guess = (guess + x / guess) * 0.5f;
    }
    return guess;
}

static float r3d_sinf(float x) {
    /* Reduce to [-pi, pi]. */
    float pi = 3.14159265358979f;
    float tau = 2.0f * pi;
    /* Modulo. */
    while (x > pi) x -= tau;
    while (x < -pi) x += tau;
    /* Taylor series (7 terms is accurate enough for visuals). */
    float x2 = x * x;
    float x3 = x2 * x;
    float x5 = x3 * x2;
    float x7 = x5 * x2;
    float x9 = x7 * x2;
    return x - x3/6.0f + x5/120.0f - x7/5040.0f + x9/362880.0f;
}

static float r3d_cosf(float x) {
    /* cos(x) = sin(x + pi/2). */
    return r3d_sinf(x + 1.57079632679f);
}

static float r3d_tanf(float x) {
    float c = r3d_cosf(x);
    if (c < 1e-6f && c > -1e-6f) return 1e6f;
    return r3d_sinf(x) / c;
}

/* ---- Constants ---- */
#define PI_F 3.14159265358979f
#define DEG2RAD(x) ((x) * PI_F / 180.0f)

/* ---- Camera state ---- */
static float cam_fov   = 60.0f * PI_F / 180.0f;
static float cam_near  = 0.1f;
static float cam_far   = 100.0f;
static float cam_dist  = 4.0f;   /* distance from camera to origin */

/* ---- Vector operations ---- */

vec3 vec3_make(float x, float y, float z) {
    vec3 v = {x, y, z};
    return v;
}

vec3 vec3_add(vec3 a, vec3 b) {
    return vec3_make(a.x + b.x, a.y + b.y, a.z + b.z);
}

vec3 vec3_sub(vec3 a, vec3 b) {
    return vec3_make(a.x - b.x, a.y - b.y, a.z - b.z);
}

vec3 vec3_scale(vec3 a, float s) {
    return vec3_make(a.x * s, a.y * s, a.z * s);
}

float vec3_dot(vec3 a, vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

vec3 vec3_cross(vec3 a, vec3 b) {
    return vec3_make(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    );
}

float vec3_length(vec3 a) {
    return r3d_sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
}

vec3 vec3_normalize(vec3 a) {
    float len = vec3_length(a);
    if (len < 1e-6f) return vec3_make(0, 0, 0);
    return vec3_scale(a, 1.0f / len);
}

/* ---- Matrix operations (column-major, OpenGL-style) ---- */

mat4 mat4_identity(void) {
    mat4 r = {{1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1}};
    return r;
}

mat4 mat4_mul(mat4 a, mat4 b) {
    mat4 r;
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++) {
                sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = sum;
        }
    }
    return r;
}

mat4 mat4_rot_x(float angle) {
    float c = r3d_cosf(angle), s = r3d_sinf(angle);
    mat4 r = {{1,0,0,0,  0,c,s,0,  0,-s,c,0,  0,0,0,1}};
    return r;
}

mat4 mat4_rot_y(float angle) {
    float c = r3d_cosf(angle), s = r3d_sinf(angle);
    mat4 r = {{c,0,-s,0,  0,1,0,0,  s,0,c,0,  0,0,0,1}};
    return r;
}

mat4 mat4_rot_z(float angle) {
    float c = r3d_cosf(angle), s = r3d_sinf(angle);
    mat4 r = {{c,s,0,0,  -s,c,0,0,  0,0,1,0,  0,0,0,1}};
    return r;
}

mat4 mat4_translate(float x, float y, float z) {
    mat4 r = {{1,0,0,0,  0,1,0,0,  0,0,1,0,  x,y,z,1}};
    return r;
}

mat4 mat4_perspective(float fov, float aspect, float near_p, float far_p) {
    float f = 1.0f / r3d_tanf(fov / 2.0f);
    mat4 r;
    for (int i = 0; i < 16; i++) r.m[i] = 0;
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (far_p + near_p) / (near_p - far_p);
    r.m[11] = -1;
    r.m[14] = (2 * far_p * near_p) / (near_p - far_p);
    return r;
}

vec3 mat4_transform(mat4 m, vec3 v) {
    float x = v.x, y = v.y, z = v.z;
    float w = m.m[12] * x + m.m[13] * y + m.m[14] * z + m.m[15];
    /* w should be 1 for affine transforms; for perspective it's the depth. */
    float rx = (m.m[0] * x + m.m[1] * y + m.m[2] * z + m.m[3]) ;
    float ry = (m.m[4] * x + m.m[5] * y + m.m[6] * z + m.m[7]) ;
    float rz = (m.m[8] * x + m.m[9] * y + m.m[10] * z + m.m[11]);
    /* For perspective projection, divide by w. */
    if (w != 0 && w != 1) {
        rx /= w; ry /= w; rz /= w;
    }
    return vec3_make(rx, ry, rz);
}

/* ---- Projection: 3D world → 2D screen ---- */

void r3d_project(vec3 world, int *out_x, int *out_y) {
    /* Translate by camera distance (camera looks down -Z). */
    float z = world.z + cam_dist;
    if (z < cam_near) z = cam_near;

    /* Perspective divide. */
    float scale = 1.0f / z;
    float aspect = (float)gfx_get_width() / (float)gfx_get_height();

    /* Map to screen coordinates. */
    int sx = (int)((world.x * scale * (float)gfx_get_height() * 0.5f)
                   + (float)gfx_get_width() * 0.5f);
    int sy = (int)((-world.y * scale * (float)gfx_get_height() * 0.5f)
                   + (float)gfx_get_height() * 0.5f);

    *out_x = sx;
    *out_y = sy;
}

/* ---- Wireframe rendering ---- */

void r3d_draw_line3d(vec3 a, vec3 b, uint32_t color) {
    int ax, ay, bx, by;
    r3d_project(a, &ax, &ay);
    r3d_project(b, &bx, &by);
    gfx_draw_line((uint32_t)ax, (uint32_t)ay,
                  (uint32_t)bx, (uint32_t)by, color);
}

void r3d_draw_mesh_wire(const mesh *m, mat4 transform, uint32_t color) {
    /* Transform all vertices first. */
    vec3 *tv = (vec3 *)__builtin_alloca(sizeof(vec3) * m->num_verts);
    for (int i = 0; i < m->num_verts; i++) {
        tv[i] = mat4_transform(transform, m->verts[i].pos);
    }

    /* Draw triangle edges. */
    for (int i = 0; i < m->num_tris; i++) {
        const tri *t = &m->tris[i];
        r3d_draw_line3d(tv[t->v[0]], tv[t->v[1]], color);
        r3d_draw_line3d(tv[t->v[1]], tv[t->v[2]], color);
        r3d_draw_line3d(tv[t->v[2]], tv[t->v[0]], color);
    }
}

/* ---- Filled triangle rendering (flat shaded) ---- */

/* Compute intensity from face normal vs light direction (0..1). */
static float face_light(vec3 a, vec3 b, vec3 c, vec3 light_dir) {
    vec3 edge1 = vec3_sub(b, a);
    vec3 edge2 = vec3_sub(c, a);
    vec3 normal = vec3_normalize(vec3_cross(edge1, edge2));
    /* Dot with -light_dir (light points toward the surface). */
    float d = -vec3_dot(normal, light_dir);
    if (d < 0) d = 0;
    if (d > 1) d = 1;
    return 0.2f + 0.8f * d;  /* ambient + diffuse */
}

/* Scale a packed colour by an intensity factor (0..1). */
static uint32_t scale_color(uint32_t color, float intensity) {
    /* Extract RGB (assuming the color was created by make_color in graphics.c
     * which packs based on the framebuffer's mask shifts). For simplicity,
     * we work with the raw 0x00RRGGBB interpretation. */
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    r = (uint8_t)(r * intensity);
    g = (uint8_t)(g * intensity);
    b = (uint8_t)(b * intensity);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void r3d_fill_triangle(vec3 a, vec3 b, vec3 c,
                       uint32_t color, vec3 light_dir) {
    float light = face_light(a, b, c, light_dir);
    uint32_t shaded = scale_color(color, light);

    /* Project to screen. */
    int ax, ay, bx, by, cx, cy;
    r3d_project(a, &ax, &ay);
    r3d_project(b, &bx, &by);
    r3d_project(c, &cx, &cy);

    /* Sort vertices by Y (top to bottom). */
    int xs[3] = {ax, bx, cx};
    int ys[3] = {ay, by, cy};
    /* Simple bubble sort by Y. */
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2 - i; j++) {
            if (ys[j] > ys[j + 1]) {
                int tmp = ys[j]; ys[j] = ys[j + 1]; ys[j + 1] = tmp;
                tmp = xs[j]; xs[j] = xs[j + 1]; xs[j + 1] = tmp;
            }
        }
    }

    /* Flat-bottom / flat-top triangle fill (scanline). */
    int total_h = ys[2] - ys[0];
    if (total_h <= 0) return;

    for (int y = ys[0]; y <= ys[2]; y++) {
        int h2 = y - ys[0];
        float alpha = (float)h2 / total_h;
        int x_left = (int)(xs[0] + alpha * (xs[2] - xs[0]));

        int x_right;
        if (y < ys[1]) {
            /* Upper half: left edge is 0→2, right edge is 0→1. */
            int seg_h = ys[1] - ys[0];
            if (seg_h <= 0) seg_h = 1;
            float beta = (float)h2 / seg_h;
            x_right = (int)(xs[0] + beta * (xs[1] - xs[0]));
        } else {
            /* Lower half: left edge is 0→2, right edge is 1→2. */
            int seg_h = ys[2] - ys[1];
            if (seg_h <= 0) seg_h = 1;
            float beta = (float)(y - ys[1]) / seg_h;
            x_right = (int)(xs[1] + beta * (xs[2] - xs[1]));
        }

        if (x_left > x_right) {
            int tmp = x_left; x_left = x_right; x_right = tmp;
        }
        for (int x = x_left; x <= x_right; x++) {
            gfx_putpixel((uint32_t)x, (uint32_t)y, shaded);
        }
    }
}

/* ---- Painter's algorithm mesh rendering ---- */

typedef struct {
    float depth;
    const tri *t;
    vec3 a, b, c;
} render_tri;

/* Simple sort (insertion sort — small N). */
static void sort_tris(render_tri *arr, int n) {
    for (int i = 1; i < n; i++) {
        render_tri key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j].depth < key.depth) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

void r3d_draw_mesh_filled(const mesh *m, mat4 transform,
                          uint32_t base_color, vec3 light_dir) {
    /* Transform all vertices. */
    vec3 *tv = (vec3 *)__builtin_alloca(sizeof(vec3) * m->num_verts);
    for (int i = 0; i < m->num_verts; i++) {
        tv[i] = mat4_transform(transform, m->verts[i].pos);
    }

    /* Build render list with average depth. */
    render_tri *rt = (render_tri *)__builtin_alloca(sizeof(render_tri) * m->num_tris);
    for (int i = 0; i < m->num_tris; i++) {
        const tri *t = &m->tris[i];
        rt[i].a = tv[t->v[0]];
        rt[i].b = tv[t->v[1]];
        rt[i].c = tv[t->v[2]];
        rt[i].depth = (rt[i].a.z + rt[i].b.z + rt[i].c.z) / 3.0f;
        rt[i].t = t;
    }

    /* Back-to-front sort (far first). */
    sort_tris(rt, m->num_tris);

    /* Render each triangle. */
    for (int i = 0; i < m->num_tris; i++) {
        /* Backface culling: skip triangles facing away from camera. */
        vec3 normal = vec3_normalize(vec3_cross(
            vec3_sub(rt[i].b, rt[i].a),
            vec3_sub(rt[i].c, rt[i].a)
        ));
        vec3 view_dir = vec3_normalize(rt[i].a);
        if (vec3_dot(normal, view_dir) > 0) {
            continue;  /* facing away */
        }

        r3d_fill_triangle(rt[i].a, rt[i].b, rt[i].c, base_color, light_dir);
    }
}

/* ---- Built-in meshes ---- */

/* Cube: 8 vertices, 12 triangles. */
static const vertex cube_verts[8] = {
    {{-1,-1,-1}}, {{ 1,-1,-1}}, {{ 1, 1,-1}}, {{-1, 1,-1}},
    {{-1,-1, 1}}, {{ 1,-1, 1}}, {{ 1, 1, 1}}, {{-1, 1, 1}},
};

static const tri cube_tris[12] = {
    /* Front face. */
    {{0,1,2}}, {{0,2,3}},
    /* Back face. */
    {{5,4,7}}, {{5,7,6}},
    /* Left face. */
    {{4,0,3}}, {{4,3,7}},
    /* Right face. */
    {{1,5,6}}, {{1,6,2}},
    /* Top face. */
    {{3,2,6}}, {{3,6,7}},
    /* Bottom face. */
    {{4,5,1}}, {{4,1,0}},
};

const mesh mesh_cube = {
    .verts = cube_verts, .num_verts = 8,
    .tris = cube_tris, .num_tris = 12,
};

/* Pyramid: 5 vertices, 6 triangles (4 sides + 2 for base). */
static const vertex pyr_verts[5] = {
    {{ 0, 1, 0}},   /* apex */
    {{-1,-1,-1}},   {{ 1,-1,-1}},
    {{ 1,-1, 1}},   {{-1,-1, 1}},
};

static const tri pyr_tris[6] = {
    {{0,1,2}}, {{0,2,3}}, {{0,3,4}}, {{0,4,1}}, /* sides */
    {{1,4,3}}, {{1,3,2}},                        /* base */
};

const mesh mesh_pyramid = {
    .verts = pyr_verts, .num_verts = 5,
    .tris = pyr_tris, .num_tris = 6,
};

/* ---- Demo ---- */

void r3d_demo(int frames) {
    uint32_t screen_w = gfx_get_width();
    uint32_t screen_h = gfx_get_height();

    vec3 light = vec3_normalize(vec3_make(0.5f, -0.7f, -0.5f));

    for (int f = 0; f < frames; f++) {
        float angle = DEG2RAD(f * 3);

        gfx_clear(0x00001010);

        /* Build rotation matrix: Y rotation + slight X tilt. */
        mat4 rot_y = mat4_rot_y(angle);
        mat4 rot_x = mat4_rot_x(DEG2RAD(20));
        mat4 transform = mat4_mul(rot_y, rot_x);

        /* Draw a filled rotating cube. */
        r3d_draw_mesh_filled(&mesh_cube, transform, GFX_RED, light);

        /* Draw a wireframe cube offset to the right. */
        mat4 trans = mat4_translate(3.5f, 0, 0);
        mat4 wt = mat4_mul(trans, transform);
        r3d_draw_mesh_wire(&mesh_cube, wt, GFX_GREEN);

        /* Draw a wireframe pyramid offset to the left. */
        mat4 trans2 = mat4_translate(-3.5f, 0, 0);
        mat4 wt2 = mat4_mul(trans2, transform);
        r3d_draw_mesh_wire(&mesh_pyramid, wt2, GFX_CYAN);

        /* Title text. */
        gfx_draw_string(10, 10, "AuraLite OS — 3D Software Renderer",
                        GFX_WHITE);

        gfx_flip();

        /* Small delay between frames. */
        timer_sleep_ms(50);
    }
}
