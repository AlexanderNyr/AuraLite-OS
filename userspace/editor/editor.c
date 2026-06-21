/*
 * editor.c — simple line-based text editor for AuraLite OS.
 *
 * Commands:
 *   :w <filename>  - write buffer to file (not yet supported; prints to stdout)
 *   :q             - quit
 *   :p             - print all lines
 *   :d <n>         - delete line n
 *   Just type text to append lines.
 */

#include "unistd.h"
#include "string.h"
#include "stdio.h"

#define MAX_LINES 64
#define LINE_LEN  120

static char lines[MAX_LINES][LINE_LEN];
static int line_count = 0;
static char input[LINE_LEN];

static void cmd_print(void) {
    for (int i = 0; i < line_count; i++) {
        printf("%3d | %s\n", i + 1, lines[i]);
    }
}

static void cmd_delete(int n) {
    if (n < 1 || n > line_count) {
        puts("Invalid line number");
        return;
    }
    for (int i = n - 1; i < line_count - 1; i++) {
        strcpy(lines[i], lines[i + 1]);
    }
    line_count--;
    printf("Deleted line %d\n", n);
}

int main(void) {
    puts("AuraLite Text Editor");
    puts("Type text to add lines. Commands start with ':'.");
    puts("  :p    print all lines");
    puts("  :d N  delete line N");
    puts("  :q    quit");

    for (;;) {
        printf("[%d] > ", line_count + 1);
        int64_t n = read(0, input, sizeof(input) - 1);
        if (n <= 0) continue;
        input[n] = 0;
        if (n > 0 && input[n-1] == '\n') input[n-1] = 0;

        if (input[0] == ':') {
            if (input[1] == 'q') break;
            if (input[1] == 'p') { cmd_print(); continue; }
            if (input[1] == 'd') {
                int ln = 0;
                for (int i = 3; input[i] >= '0' && input[i] <= '9'; i++)
                    ln = ln * 10 + (input[i] - '0');
                cmd_delete(ln);
                continue;
            }
            puts("Unknown command. :p :d N :q");
            continue;
        }

        if (input[0] == 0) continue;
        if (line_count < MAX_LINES) {
            strcpy(lines[line_count], input);
            line_count++;
        } else {
            puts("Buffer full!");
        }
    }

    puts("\n--- Final document ---");
    cmd_print();
    puts("--- End ---");
    puts("Goodbye from editor!");
    return 0;
}
