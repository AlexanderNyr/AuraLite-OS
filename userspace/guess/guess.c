/*
 * guess.c — number guessing game for AuraLite OS.
 *
 * The computer picks a random number 1-100 and the player tries to guess it.
 * Feedback is given (higher/lower) until the correct number is found.
 */

#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

static char input[32];

int main(void) {
    puts("=== Number Guessing Game ===");
    puts("I'm thinking of a number 1-100.");
    puts("");

    /* Seed with our PID for variety. */
    srand((unsigned int)getpid());
    int target = rand() % 100 + 1;
    int attempts = 0;

    for (;;) {
        write(1, "Your guess? ", 12);
        int64_t n = read(0, input, sizeof(input) - 1);
        if (n <= 0) continue;
        input[n] = 0;

        int guess = atoi(input);
        if (guess < 1 || guess > 100) {
            puts("Enter a number 1-100!");
            continue;
        }

        attempts++;
        if (guess < target) {
            printf("  %d is too low!\n", guess);
        } else if (guess > target) {
            printf("  %d is too high!\n", guess);
        } else {
            printf("\n  Correct! %d in %d attempts!\n", target, attempts);
            if (attempts <= 7) {
                puts("  Impressive!");
            } else if (attempts <= 15) {
                puts("  Well done!");
            } else {
                puts("  You got there eventually!");
            }
            break;
        }
    }

    puts("\nThanks for playing!");
    return 0;
}
