#ifndef AURALITE_DRIVERS_FRAMEBUFFER_RENDER3D_H
#define AURALITE_DRIVERS_FRAMEBUFFER_RENDER3D_H

#include <stdint.h>

/*
 * Software 3D renderer for AuraLite OS.
 *
 * Renders 3D wireframe and filled models on the 2D framebuffer using:
 *   - vec3 vector math (add, sub, dot, cross, normalize, scale)
 *   - 4x4 transformation matrices (rotation, translation, perspective)
 *   - Perspective projection (3D world → 2D screen)
 *   - Painter's algorithm depth sorting for filled triangles
 *
 * No GPU — pure CPU rasterisation through the graphics library.
 */

/* ---- 3D vector ---- */
typedef struct { float x, y, z; } vec3;

/* ---- 4x4 matrix (column-major, OpenGL-style) ---- */
typedef struct { float m[16]; } mat4;

/* ---- Vertex ---- */
typedef struct { vec3 pos; } vertex;

/* ---- Triangle (indices into a vertex array) ---- */
typedef struct { int v[3]; } tri;

/* ---- Mesh ---- */
typedef struct {
    const vertex *verts;
    int num_verts;
    const tri *tris;
    int num_tris;
} mesh;

/* ---- Vector operations ---- */
vec3 vec3_make(float x, float y, float z);
vec3 vec3_add(vec3 a, vec3 b);
vec3 vec3_sub(vec3 a, vec3 b);
vec3 vec3_scale(vec3 a, float s);
float vec3_dot(vec3 a, vec3 b);
vec3 vec3_cross(vec3 a, vec3 b);
float vec3_length(vec3 a);
vec3 vec3_normalize(vec3 a);

/* ---- Matrix operations ---- */
mat4 mat4_identity(void);
mat4 mat4_mul(mat4 a, mat4 b);
mat4 mat4_rot_x(float angle);
mat4 mat4_rot_y(float angle);
mat4 mat4_rot_z(float angle);
mat4 mat4_translate(float x, float y, float z);
mat4 mat4_perspective(float fov_rad, float aspect, float near, float far);

/* Transform a point by a matrix. */
vec3 mat4_transform(mat4 m, vec3 v);

/* ---- Rendering ---- */

/* Project a 3D point to 2D screen coordinates. */
void r3d_project(vec3 world, int *out_x, int *out_y);

/* Draw a 3D line (wireframe). */
void r3d_draw_line3d(vec3 a, vec3 b, uint32_t color);

/* Draw a 3D wireframe mesh with a transform matrix. */
void r3d_draw_mesh_wire(const mesh *m, mat4 transform, uint32_t color);

/* Draw a filled triangle with flat shading. */
void r3d_fill_triangle(vec3 a, vec3 b, vec3 c,
                       uint32_t color, vec3 light_dir);

/* Draw a filled mesh with flat shading (painter's algorithm). */
void r3d_draw_mesh_filled(const mesh *m, mat4 transform,
                          uint32_t base_color, vec3 light_dir);

/* ---- Built-in meshes ---- */
extern const mesh mesh_cube;
extern const mesh mesh_pyramid;

/* ---- Demo ---- */
/* Render a rotating 3D cube (or pyramid) for N frames. */
void r3d_demo(int frames);

#endif /* AURALITE_DRIVERS_FRAMEBUFFER_RENDER3D_H */
