/* doom/doomgeneric_auralite.c — the AuraLite backend for doomgeneric.
 *
 * DOOM_PLAN.md phase D2.
 *
 * doomgeneric asks a platform for six functions; this file is all six, over
 * libauragui. Everything else -- the engine, the renderer, the WAD loader --
 * is unmodified upstream source, fetched at build time and never vendored
 * into this repository (see the licensing note below).
 *
 * ---------------------------------------------------------------------
 * LICENSING, because it decided the shape of this port
 *
 * DOOM's source is GPL-2.0. AuraLite is Apache-2.0, and the FSF considers
 * the two incompatible: Apache-2.0's patent-termination and indemnity
 * clauses count as "further restrictions" that GPLv2 §6 forbids. So the
 * DOOM sources are NOT vendored here. `make doom` downloads doomgeneric
 * into build/, builds it there, and the repository ships only this file --
 * AuraLite's own platform layer, under AuraLite's own licence.
 *
 * A user who runs `make doom` produces a GPL-2.0 binary on their own
 * machine, which is entirely permitted; what AuraLite never does is
 * distribute one.
 * ---------------------------------------------------------------------
 *
 * The framebuffer is the easy half: doomgeneric renders into DG_ScreenBuffer
 * as packed 32-bit XRGB8888, and ag_blit() consumes exactly that, so
 * DG_DrawFrame is a single call with no per-pixel conversion. That is not
 * luck -- it is the same format the OpenGL stack already presents with.
 *
 * Input is the half with the actual work: DOOM has its own key encoding
 * (doomkeys.h), and it wants a QUEUE of transitions rather than the
 * current keyboard state.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "auragui.h"
#include "doomgeneric.h"
#include "doomkeys.h"

/* Window size: doomgeneric's default 640x400 is DOOM's 320x200 doubled,
 * which is the right thing on a 1024x768 desktop. */
#define DOOM_W DOOMGENERIC_RESX
#define DOOM_H DOOMGENERIC_RESY

static int   doom_wid = -1;
static int   have_window;
static uint64_t start_ms;

/* ---- time ---------------------------------------------------------------
 *
 * CLOCK_MONOTONIC, not the wall clock: DOOM's timing must not jump if the
 * date is set, and a backwards step would make the game logic run its
 * catch-up loop for as long as the jump lasted.
 */
static uint64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ---- the key queue ------------------------------------------------------
 *
 * DG_GetKey() is polled until it returns 0, and each call must hand back one
 * PRESS or RELEASE transition. The compositor delivers the same thing as
 * AG_EVT_KEY_DOWN/UP events, so this is a small ring buffer between the two
 * rather than any kind of state tracking.
 *
 * It has to be a queue and not a single slot: a player turning while firing
 * generates several transitions inside one frame, and dropping the extras
 * makes movement feel like it is sticking.
 */
#define KEYQ_SIZE 32

typedef struct {
    int           pressed;
    unsigned char key;
} keyevent_t;

static keyevent_t keyq[KEYQ_SIZE];
static int keyq_head, keyq_tail;

static void keyq_push(int pressed, unsigned char key) {
    int next = (keyq_head + 1) % KEYQ_SIZE;
    if (next == keyq_tail) {
        /* Full. Drop the OLDEST rather than the newest: under a flood, the
         * most recent transitions are the ones that still describe what the
         * player is doing. Dropping a release would be worse -- the key
         * would stay stuck down. */
        keyq_tail = (keyq_tail + 1) % KEYQ_SIZE;
    }
    keyq[keyq_head].pressed = pressed;
    keyq[keyq_head].key     = key;
    keyq_head = next;
}

static int keyq_pop(int *pressed, unsigned char *key) {
    if (keyq_head == keyq_tail) return 0;
    *pressed = keyq[keyq_tail].pressed;
    *key     = keyq[keyq_tail].key;
    keyq_tail = (keyq_tail + 1) % KEYQ_SIZE;
    return 1;
}

/* ---- key translation ----------------------------------------------------
 *
 * AuraLite's keycodes (drivers/keyboard/keyboard.h) to DOOM's
 * (doomkeys.h). Printable ASCII passes through unchanged in both
 * directions, so only the non-printable keys need a table.
 *
 * The mapping is deliberately explicit rather than arithmetic: DOOM's
 * codes are a historical layout (0xad for up-arrow, 0xa2 for use), and
 * anything clever here would be wrong in a way that only shows up as a
 * control that does not respond.
 */
#define KB_KEY_LEFT   0x100
#define KB_KEY_RIGHT  0x101
#define KB_KEY_UP     0x102
#define KB_KEY_DOWN   0x103
#define KB_KEY_CTRL   0x122
#define KB_KEY_ALT    0x123
#define KB_KEY_SHIFT  0x121
#define KB_KEY_ESC    0x1B
#define KB_KEY_ENTER  '\n'
#define KB_KEY_TAB    '\t'
#define KB_KEY_SPACE  ' '
#define KB_KEY_BKSP   '\b'

static unsigned char translate_key(uint32_t ag_key) {
    switch (ag_key) {
    case KB_KEY_LEFT:   return KEY_LEFTARROW;
    case KB_KEY_RIGHT:  return KEY_RIGHTARROW;
    case KB_KEY_UP:     return KEY_UPARROW;
    case KB_KEY_DOWN:   return KEY_DOWNARROW;
    /* Ctrl fires and space uses -- the vanilla defaults. */
    case KB_KEY_CTRL:   return KEY_FIRE;
    case KB_KEY_SPACE:  return KEY_USE;
    /* Alt strafes, Shift runs. */
    case KB_KEY_ALT:    return KEY_LALT;
    case KB_KEY_SHIFT:  return KEY_RSHIFT;
    case KB_KEY_ESC:    return KEY_ESCAPE;
    case KB_KEY_ENTER:  return KEY_ENTER;
    case KB_KEY_TAB:    return KEY_TAB;
    case KB_KEY_BKSP:   return KEY_BACKSPACE;
    default: break;
    }

    /* Printable ASCII, lowercased: DOOM compares against lowercase letters
     * for the cheat codes and menu accelerators. */
    if (ag_key >= 32 && ag_key < 127) {
        unsigned char c = (unsigned char)ag_key;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        return c;
    }
    return 0;   /* unmapped: swallowed rather than sent as a wrong key */
}

/* ---- the six platform hooks --------------------------------------------- */

void DG_Init(void) {
    doom_wid = ag_window_create(60, 40, DOOM_W, DOOM_H, "DOOM",
                                AG_WIN_HAS_TITLE | AG_WIN_HAS_CLOSE |
                                AG_WIN_MOVABLE);
    if (doom_wid < 0) {
        /* Without a window there is nothing to draw into. Say so and stop,
         * rather than running the whole engine and rendering to nowhere. */
        printf("doom: could not create a window (%d)\n", doom_wid);
        exit(1);
    }
    ag_window_show(doom_wid);
    have_window = 1;
    start_ms = now_ms();
    printf("DOOM-WINDOW-CREATED %dx%d\n", DOOM_W, DOOM_H);
    fflush(stdout);
}

void DG_DrawFrame(void) {
    if (!have_window) return;

    /* DG_ScreenBuffer is already packed XRGB8888 at exactly this size, which
     * is what ag_blit() wants -- stride 0 means "tightly packed". No
     * per-pixel loop, no format conversion. */
    ag_blit(doom_wid, 0, 0, DOOM_W, DOOM_H, (const uint32_t *)DG_ScreenBuffer, 0);
    ag_render_now();

    /* Drain input here rather than in DG_GetKey: the engine calls GetKey
     * repeatedly until it returns 0, and polling the compositor inside that
     * loop would mix newly-arrived events into a drain that is supposed to
     * be draining a snapshot. */
    ag_event_t e;
    while (ag_poll_event(doom_wid, &e) > 0) {
        switch (e.type) {
        case AG_EVT_KEY_DOWN: {
            unsigned char k = translate_key(e.key);
            if (k) keyq_push(1, k);
            break;
        }
        case AG_EVT_KEY_UP: {
            unsigned char k = translate_key(e.key);
            if (k) keyq_push(0, k);
            break;
        }
        case AG_EVT_CLOSE_REQ:
            /* Closing the window quits the game. Going through exit()
             * rather than _exit() lets the engine's atexit handlers run,
             * which is what writes the config back out. */
            printf("DOOM-CLOSED\n");
            fflush(stdout);
            exit(0);
            break;
        default:
            break;
        }
    }
}

void DG_SleepMs(uint32_t ms) {
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void) {
    return (uint32_t)(now_ms() - start_ms);
}

int DG_GetKey(int *pressed, unsigned char *key) {
    return keyq_pop(pressed, key);
}

void DG_SetWindowTitle(const char *title) {
    if (have_window && title) ag_window_set_title(doom_wid, title);
}

/* ---- entry point --------------------------------------------------------
 *
 * Each doomgeneric backend supplies its own main(); ours adds a default
 * IWAD path, because AuraLite has no working directory convention that
 * would let the engine find one on its own.
 *
 * The engine searches for an IWAD relative to the current directory and
 * through environment variables that AuraLite does not set, so without this
 * it exits with "Game mode indeterminate" -- technically correct and
 * completely unhelpful to someone who just typed `run doom`.
 *
 * The default path is on /fat rather than in the initrd, and that is forced
 * rather than chosen: the BIOS loader reserves an 8 MiB slot for
 * initrd.tar (tools/mkisoimage_dual.sh checks it and fails the build), the
 * initrd is already 7.6 MiB, and the smallest Freedoom IWAD is 22 MiB. So
 * the WAD travels on a separate FAT32 disk that the kernel mounts at /fat.
 * `make run-doom` attaches it.
 */
int main(int argc, char **argv) {
    /* If no -iwad was given, point at the initrd copy. argv is rebuilt
     * rather than modified in place: the engine keeps pointers into it. */
    static char *newargv[8];
    int has_iwad = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-iwad") == 0) { has_iwad = 1; break; }
    }

    if (!has_iwad && argc < 6) {
        int n = 0;
        newargv[n++] = argv[0];
        for (int i = 1; i < argc; i++) newargv[n++] = argv[i];
        newargv[n++] = (char *)"-iwad";
        newargv[n++] = (char *)"/fat/doom/freedoom1.wad";
        newargv[n]   = NULL;
        argv = newargv;
        argc = n;
    }

    doomgeneric_Create(argc, argv);

    for (;;) {
        doomgeneric_Tick();
    }
    return 0;
}
