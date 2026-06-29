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
#include "drivers/pci/pci.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/kprintf.h"
#include "drivers/gpu/virtio_gpu.h"
#include "drivers/gpu/virgl.h"

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

/* ---- 3D acceleration/backend state ---- */

static r3d_accel_info_t r3d_accel = {
    .flags = R3D_ACCEL_SOFTWARE,
    .pci_vendor = 0,
    .pci_device = 0,
    .width = 0,
    .height = 0,
    .backend_name = "software",
    .gpu_name = "Limine framebuffer",
};

static float *r3d_zbuf = 0;
static uint32_t r3d_zbuf_w = 0, r3d_zbuf_h = 0;

static inline void r3d_cpuid(uint32_t leaf, uint32_t subleaf,
                             uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(subleaf));
}

static int r3d_cpu_has_sse2(void) {
    uint32_t a, b, c, d;
    r3d_cpuid(1, 0, &a, &b, &c, &d);
    return (d & (1u << 26)) != 0;
}

static const char *r3d_gpu_name(uint16_t vendor, uint16_t device) {
    if (vendor == 0x1234 && device == 0x1111) return "QEMU/Bochs std VGA";
    if (vendor == 0x80ee && device == 0xbeef) return "VirtualBox VMSVGA/VBoxVGA";
    if (vendor == 0x15ad && (device == 0x0405 || device == 0x0710)) return "VMware SVGA II";
    if (vendor == 0x1b36 && device == 0x0100) return "QXL paravirtual GPU";
    if (vendor == 0x1af4 && device == 0x1050) return "Virtio GPU";
    return "PCI display controller";
}

static void r3d_probe_gpu(void) {
    uint8_t bus = 0, dev = 0, func = 0;
    if (pci_find_class(0x03, 0x00, &bus, &dev, &func) == 0 ||
        pci_find_class(0x03, 0x02, &bus, &dev, &func) == 0) {
        r3d_accel.pci_vendor = pci_get_vendor(bus, dev, func);
        r3d_accel.pci_device = pci_get_device(bus, dev, func);
        r3d_accel.gpu_name = r3d_gpu_name(r3d_accel.pci_vendor, r3d_accel.pci_device);
        r3d_accel.flags |= R3D_ACCEL_GPU_DETECTED;
    }
}

void r3d_accel_clear_depth(float depth) {
    if (!r3d_zbuf) return;
    uint32_t n = r3d_zbuf_w * r3d_zbuf_h;
    for (uint32_t i = 0; i < n; i++) r3d_zbuf[i] = depth;
}

void r3d_accel_init(void) {
    r3d_accel.flags = R3D_ACCEL_SOFTWARE;
    r3d_accel.backend_name = "software";
    r3d_accel.gpu_name = "Limine framebuffer";
    r3d_accel.width = (uint16_t)gfx_get_width();
    r3d_accel.height = (uint16_t)gfx_get_height();

    if (r3d_cpu_has_sse2()) {
        r3d_accel.flags |= R3D_ACCEL_SSE;
        r3d_accel.backend_name = "software-sse-zbuffer";
    }

    r3d_probe_gpu();

    if (virtio_gpu_init() == 0 && virtio_gpu_available()) {
        const virtio_gpu_info_t *vg = virtio_gpu_get_info();
        r3d_accel.flags |= R3D_ACCEL_GPU_DETECTED;
        if (vg->virgl_enabled && vg->ctx3d_ready) {
            r3d_accel.flags |= R3D_ACCEL_HW3D;
            r3d_accel.backend_name = "virtio-gpu-virgl-cmdstream+software-zbuffer";
            (void)virgl_init();
        } else if (vg->virgl_supported) {
            r3d_accel.backend_name = "virtio-gpu-virgl-supported+software-zbuffer";
        } else {
            r3d_accel.backend_name = "virtio-gpu-2d-ready+software-zbuffer";
        }
        r3d_accel.pci_vendor = vg->pci_vendor;
        r3d_accel.pci_device = vg->pci_device;
        r3d_accel.gpu_name = "Virtio GPU";
    }

    uint32_t w = gfx_get_width();
    uint32_t h = gfx_get_height();
    if (w && h && (w != r3d_zbuf_w || h != r3d_zbuf_h || !r3d_zbuf)) {
        if (r3d_zbuf) kfree(r3d_zbuf);
        r3d_zbuf = (float *)kmalloc((uint64_t)w * (uint64_t)h * sizeof(float));
        if (r3d_zbuf) {
            r3d_zbuf_w = w;
            r3d_zbuf_h = h;
            r3d_accel.flags |= R3D_ACCEL_ZBUFFER;
            r3d_accel_clear_depth(1.0e30f);
        } else {
            r3d_zbuf_w = r3d_zbuf_h = 0;
        }
    }

    kprintf("[3d] backend=%s flags=0x%x gpu=%s (%04x:%04x) zbuf=%ux%u\n",
            r3d_accel.backend_name, r3d_accel.flags, r3d_accel.gpu_name,
            r3d_accel.pci_vendor, r3d_accel.pci_device,
            r3d_zbuf_w, r3d_zbuf_h);
}

const r3d_accel_info_t *r3d_accel_info(void) { return &r3d_accel; }
const char *r3d_accel_backend_name(void) { return r3d_accel.backend_name; }

static inline int r3d_depth_test_and_store(int x, int y, float z) {
    if (!r3d_zbuf || x < 0 || y < 0 ||
        x >= (int)r3d_zbuf_w || y >= (int)r3d_zbuf_h) return 0;
    uint32_t idx = (uint32_t)y * r3d_zbuf_w + (uint32_t)x;
    if (z >= r3d_zbuf[idx]) return 0;
    r3d_zbuf[idx] = z;
    return 1;
}

/* ---- Camera state ---- */

static float cam_near  = 0.1f;

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

typedef struct {
    float x, y, z;
} r3d_screen_vertex;

static r3d_screen_vertex r3d_project_full(vec3 world) {
    float z = world.z + cam_dist;
    if (z < cam_near) z = cam_near;
    float scale = 1.0f / z;
    r3d_screen_vertex out;
    out.x = (world.x * scale * (float)gfx_get_height() * 0.5f)
            + (float)gfx_get_width() * 0.5f;
    out.y = (-world.y * scale * (float)gfx_get_height() * 0.5f)
            + (float)gfx_get_height() * 0.5f;
    out.z = z;
    return out;
}

static float r3d_edge(r3d_screen_vertex a, r3d_screen_vertex b, float x, float y) {
    return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
}

static int r3d_floor_to_int(float x) {
    int i = (int)x;
    return (x < (float)i) ? i - 1 : i;
}

static int r3d_ceil_to_int(float x) {
    int i = (int)x;
    return (x > (float)i) ? i + 1 : i;
}

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

    r3d_screen_vertex p0 = r3d_project_full(a);
    r3d_screen_vertex p1 = r3d_project_full(b);
    r3d_screen_vertex p2 = r3d_project_full(c);

    float area = r3d_edge(p0, p1, p2.x, p2.y);
    if (area > -0.0001f && area < 0.0001f) return;

    int min_x = r3d_floor_to_int(p0.x);
    int max_x = r3d_ceil_to_int(p0.x);
    int min_y = r3d_floor_to_int(p0.y);
    int max_y = r3d_ceil_to_int(p0.y);
    int p1_floor_x = r3d_floor_to_int(p1.x), p2_floor_x = r3d_floor_to_int(p2.x);
    int p1_ceil_x  = r3d_ceil_to_int(p1.x),  p2_ceil_x  = r3d_ceil_to_int(p2.x);
    int p1_floor_y = r3d_floor_to_int(p1.y), p2_floor_y = r3d_floor_to_int(p2.y);
    int p1_ceil_y  = r3d_ceil_to_int(p1.y),  p2_ceil_y  = r3d_ceil_to_int(p2.y);
    if (p1_floor_x < min_x) min_x = p1_floor_x;
    if (p2_floor_x < min_x) min_x = p2_floor_x;
    if (p1_ceil_x > max_x) max_x = p1_ceil_x;
    if (p2_ceil_x > max_x) max_x = p2_ceil_x;
    if (p1_floor_y < min_y) min_y = p1_floor_y;
    if (p2_floor_y < min_y) min_y = p2_floor_y;
    if (p1_ceil_y > max_y) max_y = p1_ceil_y;
    if (p2_ceil_y > max_y) max_y = p2_ceil_y;

    int fw = (int)gfx_get_width();
    int fh = (int)gfx_get_height();
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= fw) max_x = fw - 1;
    if (max_y >= fh) max_y = fh - 1;
    if (min_x > max_x || min_y > max_y) return;

    float inv_area = 1.0f / area;
    int use_z = (r3d_accel.flags & R3D_ACCEL_ZBUFFER) != 0;

    for (int y = min_y; y <= max_y; y++) {
        float py = (float)y + 0.5f;
        for (int x = min_x; x <= max_x; x++) {
            float px = (float)x + 0.5f;
            float w0 = r3d_edge(p1, p2, px, py) * inv_area;
            float w1 = r3d_edge(p2, p0, px, py) * inv_area;
            float w2 = r3d_edge(p0, p1, px, py) * inv_area;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            float z = w0 * p0.z + w1 * p1.z + w2 * p2.z;
            if (!use_z || r3d_depth_test_and_store(x, y, z)) {
                gfx_putpixel((uint32_t)x, (uint32_t)y, shaded);
            }
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
    r3d_accel_init();
    if (r3d_accel.flags & R3D_ACCEL_HW3D) {
        if (virgl_demo_submit_triangle() != 0) {
            (void)virgl_demo_submit_clear();
        }
    }

    vec3 light = vec3_normalize(vec3_make(0.5f, -0.7f, -0.5f));

    for (int f = 0; f < frames; f++) {
        float angle = DEG2RAD(f * 3);

        gfx_clear(0x00001010);
        r3d_accel_clear_depth(1.0e30f);

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
        gfx_draw_string(10, 10, "AuraLite OS — 3D Accelerated Renderer",
                        GFX_WHITE);
        gfx_draw_string(10, 22, r3d_accel_backend_name(), GFX_CYAN);

        gfx_flip();

        /* Small delay between frames. */
        timer_sleep_ms(50);
    }
}
