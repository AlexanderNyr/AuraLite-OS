/*
 * snake.c — terminal Snake game for AuraLite OS.
 *
 * A simplified version that uses ANSI-like text output to render
 * a snake on the serial terminal. Movement via wasd keys.
 *
 * Note: without a timer-based game loop (would need sleep syscall),
 * this is a turn-based snake: each key press advances one step.
 */

#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define W 20
#define H 10

static int sx[64], sy[64];
static int slen;
static int fx, fy;
static int dx, dy;
static int score;
static int dead;
static char grid[H][W + 1];

static void place_food(void) {
    fx = rand() % W;
    fy = rand() % H;
}

static void draw(void) {
    /* Clear screen and draw grid. */
    write(1, "\n\n", 2);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            grid[y][x] = '.';
        }
        grid[y][W] = 0;
    }
    /* Draw food. */
    grid[fy][fx] = '*';
    /* Draw snake. */
    for (int i = 0; i < slen; i++) {
        if (sx[i] >= 0 && sx[i] < W && sy[i] >= 0 && sy[i] < H)
            grid[sy[i]][sx[i]] = (i == 0) ? '@' : 'o';
    }
    /* Print border + grid. */
    write(1, "+--------------------+\n", 23);
    for (int y = 0; y < H; y++) {
        putchar('|');
        write(1, grid[y], W);
        putchar('|');
        putchar('\n');
    }
    write(1, "+--------------------+\n", 23);
    printf("Score: %d  Length: %d\n", score, slen);
}

static void step(void) {
    if (dead) return;

    /* Move body. */
    for (int i = slen - 1; i > 0; i--) {
        sx[i] = sx[i - 1];
        sy[i] = sy[i - 1];
    }
    sx[0] += dx;
    sy[0] += dy;

    /* Wall collision. */
    if (sx[0] < 0 || sx[0] >= W || sy[0] < 0 || sy[0] >= H) {
        dead = 1;
        puts("CRASH! You hit a wall!");
        return;
    }

    /* Self collision. */
    for (int i = 1; i < slen; i++) {
        if (sx[i] == sx[0] && sy[i] == sy[0]) {
            dead = 1;
            puts("CRASH! You ate yourself!");
            return;
        }
    }

    /* Eat food. */
    if (sx[0] == fx && sy[0] == fy) {
        score += 10;
        if (slen < 64) slen++;
        place_food();
    }
}

int main(void) {
    puts("=== AuraLite Snake ===");
    puts("Turn-based snake. Controls: w=up s=down a=left d=right q=quit");

    /* Init. */
    slen = 3;
    sx[0] = W/2; sy[0] = H/2;
    sx[1] = W/2 - 1; sy[1] = H/2;
    sx[2] = W/2 - 2; sy[2] = H/2;
    dx = 1; dy = 0;
    score = 0;
    dead = 0;
    srand((unsigned int)getpid());
    place_food();

    char input[8];
    for (;;) {
        draw();
        if (dead) break;

        write(1, "> ", 2);
        int64_t n = read(0, input, sizeof(input));
        if (n <= 0) continue;

        char c = input[0];
        if (c == 'q') break;
        if (c == 'w' && dy != 1)  { dx = 0; dy = -1; }
        if (c == 's' && dy != -1) { dx = 0; dy = 1; }
        if (c == 'a' && dx != 1)  { dx = -1; dy = 0; }
        if (c == 'd' && dx != -1) { dx = 1; dy = 0; }

        step();
    }

    printf("\nGame Over! Final score: %d\n", score);
    puts("Thanks for playing!");
    return 0;
}
