/*
 * calc.c — interactive calculator for AuraLite OS.
 *
 * Supports +, -, *, /, %, parentheses, and negative numbers.
 * Uses a recursive-descent parser with proper operator precedence.
 */

#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

static const char *expr_pos;

static void skip_ws(void) {
    while (*expr_pos == ' ' || *expr_pos == '\t') expr_pos++;
}

static long parse_expr(void);

static long parse_number(void) {
    skip_ws();
    long val = 0;
    int neg = 0;
    if (*expr_pos == '-') { neg = 1; expr_pos++; }
    while (*expr_pos >= '0' && *expr_pos <= '9') {
        val = val * 10 + (*expr_pos - '0');
        expr_pos++;
    }
    return neg ? -val : val;
}

static long parse_factor(void) {
    skip_ws();
    if (*expr_pos == '(') {
        expr_pos++;
        long val = parse_expr();
        skip_ws();
        if (*expr_pos == ')') expr_pos++;
        return val;
    }
    return parse_number();
}

static long parse_term(void) {
    long val = parse_factor();
    skip_ws();
    while (*expr_pos == '*' || *expr_pos == '/' || *expr_pos == '%') {
        char op = *expr_pos++;
        long rhs = parse_factor();
        if (op == '*') val *= rhs;
        else if (op == '/') { if (rhs == 0) { puts("Error: division by zero"); return 0; } val /= rhs; }
        else { if (rhs == 0) { puts("Error: modulo by zero"); return 0; } val %= rhs; }
        skip_ws();
    }
    return val;
}

static long parse_expr(void) {
    long val = parse_term();
    skip_ws();
    while (*expr_pos == '+' || *expr_pos == '-') {
        char op = *expr_pos++;
        long rhs = parse_term();
        if (op == '+') val += rhs;
        else val -= rhs;
        skip_ws();
    }
    return val;
}

static char input[256];

static void show_help(void) {
    puts("AuraLite Calculator");
    puts("Enter an expression (e.g. 2+3*4) or 'quit'");
    puts("Supports: + - * / % ( )");
}

int main(void) {
    show_help();
    for (;;) {
        write(1, "calc> ", 6);
        int64_t n = read(0, input, sizeof(input) - 1);
        if (n <= 0) continue;
        input[n] = 0;

        /* Trim trailing newline. */
        if (n > 0 && input[n-1] == '\n') input[n-1] = 0;

        if (strcmp(input, "quit") == 0 || strcmp(input, "q") == 0) break;
        if (strcmp(input, "help") == 0 || strcmp(input, "h") == 0) {
            show_help();
            continue;
        }
        if (input[0] == 0) continue;

        expr_pos = input;
        long result = parse_expr();
        printf("= %ld\n", result);
    }
    puts("Goodbye from calc!");
    return 0;
}
