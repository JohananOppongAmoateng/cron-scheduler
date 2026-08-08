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

static void substitute_aliases(char* field, const char* names[], int count, int first_value) {
    for (int i = 0; i < count; i++) {
        char* match;
        while ((match = strstr(field, names[i])) != NULL) {
            char value[3];
            snprintf(value, sizeof value, "%d", first_value + i);
            size_t value_len = strlen(value);
            memmove(match + value_len, match + 3, strlen(match + 3) + 1);
            memcpy(match, value, value_len);
        }
    }
}

static int tokenize(char* line, char fields[5][32]) {
    static const char* MONTHS[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };
    static const char* DAYS[] = {
        "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
    };

    for (char* p = line; *p; p++) *p = (char)toupper((unsigned char)*p);
    while (*line == ' ' || *line == '\t') line++;
    char* e = line + strlen(line);
    while (e > line && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n')) *--e = 0;
    for (int i = 0; ALIASES[i][0]; i++) {
        if (strcmp(line, ALIASES[i][0]) == 0) { strcpy(line, ALIASES[i][1]); break; }
    }
    int n = 0; char* tok = strtok(line, " \t");
    while (tok && n < 5) {
        strncpy(fields[n], tok, 31);
        fields[n++][31] = '\0';
        tok = strtok(NULL, " \t");
    }
    if (n != 5) return -1;
    substitute_aliases(fields[3], MONTHS, 12, 1);
    substitute_aliases(fields[4], DAYS, 7, 0);
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
