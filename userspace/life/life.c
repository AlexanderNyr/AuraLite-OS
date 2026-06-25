/* life — Conway's Game of Life in CLI. */
#include "unistd.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

#define WIDTH  30
#define HEIGHT 15

static char grid1[HEIGHT][WIDTH];
static char grid2[HEIGHT][WIDTH];

static int count_neighbors(int r, int c) {
    int count = 0;
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nr = (r + dr + HEIGHT) % HEIGHT;
            int nc = (c + dc + WIDTH) % WIDTH;
            if (grid1[nr][nc] == '#') count++;
        }
    }
    return count;
}

int main(void) {
    puts("\n--- Conway's Game of Life (AuraLite Edition) ---\n");
    memset(grid1, ' ', sizeof(grid1));
    /* Add a glider and some random blocks */
    grid1[1][2] = '#';
    grid1[2][3] = '#';
    grid1[3][1] = '#'; grid1[3][2] = '#'; grid1[3][3] = '#';

    grid1[7][8] = '#'; grid1[7][9] = '#'; grid1[8][8] = '#'; grid1[8][9] = '#';

    for (int gen = 0; gen < 10; gen++) {
        printf("\n=== Generation %d ===\n", gen + 1);
        for (int r = 0; r < HEIGHT; r++) {
            char line[WIDTH + 1];
            for (int c = 0; c < WIDTH; c++) line[c] = grid1[r][c];
            line[WIDTH] = '\0';
            printf(" | %s |\n", line);
        }

        /* Calculate next gen */
        memset(grid2, ' ', sizeof(grid2));
        for (int r = 0; r < HEIGHT; r++) {
            for (int c = 0; c < WIDTH; c++) {
                int n = count_neighbors(r, c);
                if (grid1[r][c] == '#') {
                    if (n == 2 || n == 3) grid2[r][c] = '#';
                } else {
                    if (n == 3) grid2[r][c] = '#';
                }
            }
        }
        memcpy(grid1, grid2, sizeof(grid1));
        for (volatile int s = 0; s < 2000000; s++) {}
    }
    puts("\nSimulation complete.\n");
    return 0;
}
