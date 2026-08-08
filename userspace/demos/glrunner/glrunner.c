/* glrunner.c — "Cube Runner", a 3D dodge-the-cubes game for AuraLite OS.
 *
 * Built on the in-house software OpenGL stack (libgl + AuraGLX), the same
 * way userspace/demos/glcube does it: a window, an AGLX context, immediate
 * mode geometry, one directional light, linear fog for depth cueing.
 *
 * Gameplay: you glide forward through an endless field of floating cubes.
 * Steer left/right to slip through the gaps; every cube that passes you
 * raises the score, and the speed ramps up forever.  Hitting a cube ends
 * the run — press R to restart.
 *
 * Controls:
 *   A / D or Left / Right   steer
 *   Space                   pause
 *   R                       restart after a crash
 *   Q / Esc / window close  quit
 *
 * HUD: a 7-segment overlay drawn in an ortho pass (the GL stack has no font
 * machinery), plus the live score in the window title every so often.
 *
 * Frame limit for automated testing follows the glcube convention: an
 * optional decimal frame count in /tmp/glrunner.frames (the shell's "run"
 * cannot forward argv).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "auragui.h"
#include "GL/gl.h"
#include "GL/glu.h"
#include "GL/auraglx.h"

/* Small buffers keep the software rasterizer snappy under emulation. */
#define RUN_W 400
#define RUN_H 300
#define WIN_W (RUN_W + 16)
#define WIN_H (RUN_H + 48)

/* ---- World / gameplay constants ---- */
#define LANES        9          /* x positions: -8,-6,...,+8           */
#define LANE_STEP    2.0f
#define X_LIMIT      9.0f
#define NUM_OBST     36
#define SPAWN_DEPTH  90.0f      /* respawn band depth                 */
#define HIT_Z        0.9f
#define HIT_X        1.15f
#define SPEED0       0.16f      /* world units per frame  (start)     */
#define SPEED_RAMP   0.00016f   /* added per frame                    */
#define STEER        0.42f      /* holding a key moves this / frame   */

/* Player pseudo-physics. */
static float player_x   = 0.0f;
static float bank       = 0.0f;    /* visual roll while steering      */
static float speed      = SPEED0;
static long  score      = 0;
static long  best       = 0;
static int   crashed    = 0;
static int   paused     = 0;

/* Obstacle pool: each owns a lane x, world z, a colour and a slow spin. */
typedef struct {
    float x, z, y, spin, sspin;
    float r, g, b;
} obst_t;
static obst_t obst[NUM_OBST];

/* Tiny deterministic PRNG: good enough for spawn rolls, reproducible. */
static unsigned rng = 0xC0FFEE;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 16; }

static const float palette[6][3] = {
    { 0.95f, 0.30f, 0.35f }, { 0.30f, 0.75f, 1.00f }, { 0.40f, 0.95f, 0.45f },
    { 1.00f, 0.80f, 0.25f }, { 0.85f, 0.45f, 1.00f }, { 1.00f, 0.50f, 0.20f },
};

/* Respawn rule that keeps the game winnable: a new cube must not share its
 * lane with two more cubes already within +-4 z of its slot (else a wall can
 * form that no steering can get around). */
static int lane_ok(float x, float z) {
    int near = 0;
    for (int i = 0; i < NUM_OBST; i++) {
        if (obst[i].x == x && obst[i].z > z - 4.0f && obst[i].z < z + 4.0f)
            near++;
    }
    return near < 2;
}

static void spawn_obstacle(obst_t *o, float z) {
    for (int tries = 0; tries < 6; tries++) {
        float x = ((int)(rnd() % LANES) - LANES / 2) * LANE_STEP;
        if (lane_ok(x, z)) { o->x = x; break; }
    }
    const float *c = &palette[rnd() % 6][0];
    o->z = z; o->y = -0.55f + (float)(rnd() % 3) * 1.0f;
    o->r = c[0]; o->g = c[1]; o->b = c[2];
    o->spin = (float)(rnd() % 360); o->sspin = 0.4f + (float)(rnd() % 3) * 0.4f;
}

static void reset_game(void) {
    player_x = 0.0f; bank = 0.0f;
    speed = SPEED0; score = 0; crashed = 0;
    for (int i = 0; i < NUM_OBST; i++)
        spawn_obstacle(&obst[i], -15.0f - (float)i * (SPAWN_DEPTH / NUM_OBST));
}

/* ---- 7-segment HUD ----
 * One quad per segment; seven on/off bits per glyph.  Digits plus the few
 * letters needed for SCORE / SPEED / PAUSE banners. */
static const uint8_t glyph_A = 0x77, glyph_C = 0x39, glyph_E = 0x79,
                     glyph_P = 0x73, glyph_S = 0x6D, glyph_U = 0x3E,
                     glyph_O = 0x3F;
/*                      bit: 6543210 == g f e d c b a of a classic segment */

static uint8_t segbits(char c) {
    static const uint8_t dig[10] = { 0x3F,0x06,0x5B,0x4F,0x66,
                                     0x6D,0x7D,0x07,0x7F,0x6F };
    if (c >= '0' && c <= '9') return dig[c - '0'];
    switch (c) {
    case 'A': return glyph_A;  case 'C': return glyph_C;
    case 'E': return glyph_E;  case 'P': return glyph_P;
    case 'S': return glyph_S;  case 'U': return glyph_U;
    case 'O': return glyph_O;  case 'V': return 0x1C;   /* like v */
    case 'R': return 0x05;                               /* like r */
    case 'G': return 0x7D;
    case 'M': return 0x37;   /* like П */
    case 'D': return 0x5E;   /* like d */
    case 'B': return 0x7C;   /* like b */
    case 'T': return 0x78;   /* like t */
    case ' ': return 0;
    default:  return 0x40;    /* lone middle bar for unknown chars */
    }
}

/* Draw one glyph at (x,y), h pixels tall, in the current colour. */
static void seg_glyph(float x, float y, float h, char c) {
    uint8_t m = segbits(c);
    float w = h * 0.6f, t = h * 0.14f;           /* width, stroke */
    /* segment rectangles (a..g): x0,y0,x1,y1 in pixel coords (y grows up) */
    float s[7][4] = {
        { x+t,   y+h-t, x+w-t, y+h     },  /* a top        */
        { x+w-t, y+h/2, x+w,   y+h-t   },  /* b upper right*/
        { x+w-t, y+t,   x+w,   y+h/2   },  /* c lower right*/
        { x+t,   y,     x+w-t, y+t     },  /* d bottom     */
        { x,     y+t,   x+t,   y+h/2   },  /* e lower left */
        { x,     y+h/2, x+t,   y+h-t   },  /* f upper left */
        { x+t,   y+h/2-t/2, x+w-t, y+h/2+t/2 }, /* g middle */
    };
    glBegin(GL_QUADS);
    for (int i = 0; i < 7; i++) {
        if (!(m & (1 << i))) continue;
        glVertex2f(s[i][0], s[i][1]); glVertex2f(s[i][2], s[i][1]);
        glVertex2f(s[i][2], s[i][3]); glVertex2f(s[i][0], s[i][3]);
    }
    glEnd();
}

static void seg_text(float x, float y, float h, const char *s) {
    for (; *s; s++) { seg_glyph(x, y, h, *s); x += h * 0.78f; }
}

static void seg_number(float x, float y, float h, long v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", v);
    seg_text(x, y, h, buf);
}

/* Everything in the ortho overlay: score, speed, banners. */
static void draw_hud(void) {
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, RUN_W, 0, RUN_H, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING);

    /* Score block, top left. */
    seg_text(8, RUN_H - 26, 9, "SCORE");
    seg_number(78, RUN_H - 26, 9, score);
    /* Speed block, top right. */
    char sp[16]; snprintf(sp, sizeof(sp), "%d", (int)(speed * 100.0f));
    seg_text(RUN_W - 108, RUN_H - 26, 9, "SPEED");
    seg_text(RUN_W - 40, RUN_H - 26, 9, sp);

    if (paused && !crashed) {
        seg_text(RUN_W / 2 - 38, RUN_H - 60, 12, "PAUSE");
    }
    if (crashed) {
        /* pulsing banner: bright on, off in short blinks */
        seg_text(RUN_W / 2 - 78, RUN_H / 2 + 6, 20, "GAME OVER");
        seg_text(RUN_W / 2 - 46, RUN_H / 2 - 26, 11, "BEST");
        seg_number(RUN_W / 2 + 4, RUN_H / 2 - 26, 11, best);
        /* red frame under the text */
        glColor3f(0.85f, 0.15f, 0.2f);
        glBegin(GL_LINE_LOOP);
        glVertex2f(RUN_W/2 - 100, RUN_H/2 - 44); glVertex2f(RUN_W/2 + 92, RUN_H/2 - 44);
        glVertex2f(RUN_W/2 + 92, RUN_H/2 + 34);  glVertex2f(RUN_W/2 - 100, RUN_H/2 + 34);
        glEnd();
    }

    glEnable(GL_LIGHTING); glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
}

/* ---- Geometry: one compiled unit cube, the grid, a starfield ---- */
static const GLfloat cv[8][3] = {
    {-0.5f,-0.5f,-0.5f},{0.5f,-0.5f,-0.5f},{0.5f,0.5f,-0.5f},{-0.5f,0.5f,-0.5f},
    {-0.5f,-0.5f, 0.5f},{0.5f,-0.5f, 0.5f},{0.5f,0.5f, 0.5f},{-0.5f,0.5f, 0.5f},
};
static const int cf[6][4] = {
    {0,3,2,1},{4,5,6,7},{0,4,7,3},{1,2,6,5},{0,1,5,4},{3,7,6,2},
};
static const GLfloat cn[6][3] = {
    {0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},
};

static GLuint cube_list = 0;
static void compile_cube(void) {
    cube_list = glGenLists(1);
    glNewList(cube_list, GL_COMPILE);
    glBegin(GL_QUADS);
    for (int f = 0; f < 6; f++) {
        glNormal3f(cn[f][0], cn[f][1], cn[f][2]);
        for (int v = 0; v < 4; v++) {
            const GLfloat *p = cv[cf[f][v]];
            glVertex3f(p[0], p[1], p[2]);
        }
    }
    glEnd();
    glEndList();
}

static void draw_cube_at(float x, float y, float z, float spin,
                         float sx, float sy, float sz,
                         float r, float g, float b) {
    glPushMatrix();
    glTranslatef(x, y, z);
    glRotatef(spin, 0.35f, 1.0f, 0.15f);
    glScalef(sx, sy, sz);
    glColor3f(r, g, b);
    glCallList(cube_list);
    glPopMatrix();
}

/* Scrolling floor grid: line endpoints shift by the distance covered so the
 * ground visibly rushes past (fractional part of travelled distance). */
static float grid_scroll = 0.0f;
#define GRID_Y   -1.1f
#define GRID_EXT  14.0f

static void draw_grid(void) {
    glDisable(GL_LIGHTING);
    glColor3f(0.16f, 0.22f, 0.34f);
    float off = grid_scroll - (float)(int)grid_scroll;   /* 0..1 */
    glBegin(GL_LINES);
    for (float x = -GRID_EXT; x <= GRID_EXT; x += 2.0f) {
        glVertex3f(x, GRID_Y,  4.0f); glVertex3f(x, GRID_Y, -70.0f);
    }
    for (float z = 4.0f - off * 2.0f; z > -70.0f; z -= 2.0f) {
        glVertex3f(-GRID_EXT, GRID_Y, z); glVertex3f(GRID_EXT, GRID_Y, z);
    }
    glEnd();
    glEnable(GL_LIGHTING);
}

#define NUM_STARS 70
static GLfloat stars[NUM_STARS * 3];
static void build_stars(void) {
    for (int i = 0; i < NUM_STARS; i++) {
        stars[i*3+0] = ((float)(rnd() % 800) - 400.0f) / 20.0f;   /* x +-20 */
        stars[i*3+1] = ((float)(rnd() % 300) + 20.0f) / 20.0f;    /* y 1..16*/
        stars[i*3+2] = -20.0f - (float)(rnd() % 400) / 10.0f;     /* z -20..-60 */
    }
}
static void draw_stars(void) {
    glDisable(GL_LIGHTING); glDisable(GL_FOG); glDisable(GL_DEPTH_TEST);
    glColor3f(0.85f, 0.88f, 1.0f);  /* points are 1 px in libgl; fine for stars */
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, stars);
    glDrawArrays(GL_POINTS, 0, NUM_STARS);
    glDisableClientState(GL_VERTEX_ARRAY);
    glEnable(GL_DEPTH_TEST); glEnable(GL_FOG); glEnable(GL_LIGHTING);
}

static long read_frame_limit(void) {
    int fd = open("/tmp/glrunner.frames", O_RDONLY);
    if (fd < 0) return 0;
    char buf[32];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    long v = strtol(buf, NULL, 10);
    return (v > 0) ? v : 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    long max_frames = read_frame_limit();

    int wid = ag_window_create(90, 55, WIN_W, WIN_H, "Cube Runner 3D",
                               AG_WIN_DEFAULT);
    if (wid < 0) { printf("glrunner: cannot create window\n"); return 1; }
    ag_window_show(wid);

    aglx_context_t *ctx = aglxCreateContext(wid, RUN_W, RUN_H, AGLX_DEFAULT);
    if (!ctx) {
        printf("glrunner: cannot create GL context\n");
        ag_window_destroy(wid);
        return 1;
    }
    aglxMakeCurrent(ctx);
    printf("glrunner: %s | %s\n",
           (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VERSION));

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(60.0, (GLdouble)RUN_W / (GLdouble)RUN_H, 0.5, 90.0);
    glMatrixMode(GL_MODELVIEW);

    glClearColor(0.03f, 0.04f, 0.09f, 1.0f);   /* night sky */
    glShadeModel(GL_SMOOTH);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_NORMALIZE);
    {
        GLfloat pos[4]  = { 0.4f, 1.0f, 0.6f, 0.0f };
        GLfloat dif[4]  = { 1.0f, 0.96f, 0.9f, 1.0f };
        GLfloat amb[4]  = { 0.28f, 0.30f, 0.38f, 1.0f };
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
        glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
        glLightfv(GL_LIGHT0, GL_AMBIENT, amb);
    }
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 18.0f);
    glFogf(GL_FOG_END, 55.0f);
    {
        GLfloat fc[4] = { 0.03f, 0.04f, 0.09f, 1.0f };
        glFogfv(GL_FOG_COLOR, fc);
    }

    compile_cube();
    build_stars();
    reset_game();

    int running = 1, steer = 0;
    long frames = 0;

    while (running) {
        ag_event_t ev;
        while (ag_poll_event(wid, &ev) > 0) {
            switch (ev.type) {
            case AG_EVT_CLOSE_REQ:
                running = 0; break;
            case AG_EVT_KEY_DOWN:
                if (ev.key == 'q' || ev.key == 27) running = 0;
                else if (ev.key == ' ') paused = !paused;
                else if (ev.key == 'r' || ev.key == 'R') {
                    if (crashed) reset_game();
                }
                else if (ev.key == 'a' || ev.key == 'A' || ev.key == 0x100) steer = -1;
                else if (ev.key == 'd' || ev.key == 'D' || ev.key == 0x101) steer = +1;
                break;
            case AG_EVT_KEY_UP:
                if ((ev.key == 'a' || ev.key == 'A' || ev.key == 0x100) && steer < 0) steer = 0;
                else if ((ev.key == 'd' || ev.key == 'D' || ev.key == 0x101) && steer > 0) steer = 0;
                break;
            default: break;
            }
        }
        if (!running) break;

        /* ---- Update ---- */
        if (!paused && !crashed) {
            if (steer != 0) {
                player_x += (float)steer * STEER;
                bank += ((float)steer * 22.0f - bank) * 0.25f;
            } else {
                bank *= 0.85f;
            }
            if (player_x >  X_LIMIT) player_x =  X_LIMIT;
            if (player_x < -X_LIMIT) player_x = -X_LIMIT;

            speed += SPEED_RAMP;
            grid_scroll += speed;

            for (int i = 0; i < NUM_OBST; i++) {
                obst[i].z += speed;
                obst[i].spin += obst[i].sspin;
                if (obst[i].z > 4.0f) {
                    spawn_obstacle(&obst[i], obst[i].z - SPAWN_DEPTH);
                    score++;
                    if (score > best) best = score;
                }
                /* crash check */
                if (!crashed &&
                    obst[i].z > -HIT_Z && obst[i].z < HIT_Z &&
                    obst[i].x > player_x - HIT_X && obst[i].x < player_x + HIT_X &&
                    obst[i].y > -1.6f) {
                    crashed = 1;
                }
            }
        }

        /* Window title carries a live scoreboard. */
        if ((frames & 31) == 0) {
            char title[64];
            if (crashed)
                snprintf(title, sizeof(title),
                         "Cube Runner - CRASH! score %ld (best %ld) - R to restart",
                         score, best);
            else if (paused)
                snprintf(title, sizeof(title), "Cube Runner - PAUSED - score %ld", score);
            else
                snprintf(title, sizeof(title), "Cube Runner - score %ld, best %ld",
                         score, best);
            ag_window_set_title(wid, title);
        }

        /* ---- Draw ---- */
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glLoadIdentity();
        /* Camera: just above and behind the runner cube. */
        gluLookAt(player_x * 0.55, 0.9, 3.6,
                  player_x * 0.8, -0.5, -6.0,
                  0.0, 1.0, 0.0);

        draw_stars();
        draw_grid();

        for (int i = 0; i < NUM_OBST; i++)
            draw_cube_at(obst[i].x, obst[i].y, obst[i].z, obst[i].spin,
                         1.6f, 1.3f, 1.6f,
                         obst[i].r, obst[i].g, obst[i].b);

        /* Player: ejected “ship” cube, tinted cyan, banking in turns. */
        draw_cube_at(player_x, GRID_Y + 0.55f, 0.0f, bank,
                     0.9f, 0.45f, 0.9f,
                     crashed ? 1.0f : 0.30f, crashed ? 0.25f : 0.85f,
                     crashed ? 0.25f : 1.00f);

        glColor3f(0.92f, 0.95f, 1.0f);
        draw_hud();

        aglxSwapBuffers(ctx);

        frames++;
        if (max_frames > 0 && frames >= max_frames) {
            printf("glrunner: rendered %ld frames\n", frames);
            break;
        }
    }

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) printf("glrunner: GL error 0x%04x\n", err);
    else                    printf("glrunner: clean exit, %ld frames\n", frames);

    if (cube_list) glDeleteLists(cube_list, 1);
    aglxDestroyContext(ctx);
    ag_window_destroy(wid);
    return 0;
}
