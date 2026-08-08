#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char* ALIASES[][2] = {
    {"@YEARLY","0 0 1 1 *"}, {"@ANNUALLY","0 0 1 1 *"},
    {"@MONTHLY","0 0 1 * *"}, {"@WEEKLY","0 0 * * 0"},
    {"@DAILY","0 0 * * *"},   {"@MIDNIGHT","0 0 * * *"},
    {"@HOURLY","0 * * * *"},  {NULL, NULL}
};

static int tokenize(char* line, char fields[5][32]) {
    for (char* p = line; *p; p++) *p = (char)toupper((unsigned char)*p);
    while (*line == ' ' || *line == '\t') line++;
    char* e = line + strlen(line);
    while (e > line && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n')) *--e = 0;
    for (int i = 0; ALIASES[i][0]; i++) {
        if (strcmp(line, ALIASES[i][0]) == 0) { strcpy(line, ALIASES[i][1]); break; }
    }
    int n = 0; char* tok = strtok(line, " \t");
    while (tok && n < 5) { strncpy(fields[n++], tok, 31); tok = strtok(NULL, " \t"); }
    if (n != 5) return -1;
    // TODO: substitute MONTH aliases (JAN..DEC -> 1..12) inside fields[3]
    //       and DOW aliases (SUN..SAT -> 0..6) inside fields[4].
    return 0;
}

int main(void) {
    char line[256];
    int first = 1;
    while (fgets(line, sizeof line, stdin)) {
        char copy[256]; strncpy(copy, line, 255);
        char* p = copy; while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == '\n') continue;
        if (!first) printf("\n");
        first = 0;
        char fields[5][32];
        if (tokenize(copy, fields) < 0) { printf("ERR expected 5 fields\n"); continue; }
        for (int i = 0; i < 5; i++) printf("%s\n", fields[i]);
    }
    return 0;
}
