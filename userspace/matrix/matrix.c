/* matrix — Digital rain simulation in CLI. */
#include "unistd.h"
#include "stdio.h"
#include "stdlib.h"

int main(void) {
    puts("\n\033[32m[MATRIX] Initialising Digital Rain...\033[0m\n");
    char cols[40];
    for (int i = 0; i < 40; i++) cols[i] = ' ';

    /* Run 25 iterations of digital rain */
    for (int r = 0; r < 25; r++) {
        for (int i = 0; i < 40; i++) {
            if (rand() % 4 == 0) {
                cols[i] = 33 + (rand() % 90); /* Printable ASCII */
            } else if (rand() % 5 == 0) {
                cols[i] = ' ';
            }
        }
        cols[39] = '\0';
        printf("  \033[32m%s\033[0m\n", cols);
        /* Cheap sleep */
        for (volatile int s = 0; s < 1000000; s++) {}
    }
    puts("\n\033[32m[MATRIX] Simulation complete.\033[0m\n");
    return 0;
}
