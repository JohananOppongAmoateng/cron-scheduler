#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum result {
    OK,
    OUT_OF_RANGE,
    INVALID_STEP,
    INVALID_RANGE,
    INVALID_FIELD
};

static int parse_int(const char* text, int* value) {
    char* end;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno || *text == '\0' || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        return 0;
    }
    *value = (int)parsed;
    return 1;
}

static enum result add_part(char* part, int min, int max, unsigned char* matches) {
    int start, end, step = 1;
    char* slash = strchr(part, '/');

    if (slash) {
        if (strchr(slash + 1, '/')) return INVALID_FIELD;
        *slash++ = '\0';
        if (!parse_int(slash, &step) || step <= 0) return INVALID_STEP;
    }

    if (strcmp(part, "*") == 0) {
        start = min;
        end = max;
    } else if (parse_int(part, &start)) {
        end = slash ? max : start;
    } else {
        char trailing;
        if (sscanf(part, "%d-%d%c", &start, &end, &trailing) != 2) {
            return INVALID_FIELD;
        }
        if (start > end) return INVALID_RANGE;
    }

    if (start < min || start > max || end < min || end > max) {
        return OUT_OF_RANGE;
    }

    for (long long value = start; value <= end; value += step) {
        matches[value - min] = 1;
    }
    return OK;
}

static enum result expand_field(char* field, int min, int max,
                                unsigned char* matches) {
    char* part = field;

    if (*part == '\0') return INVALID_FIELD;
    for (;;) {
        char* comma = strchr(part, ',');
        if (comma) *comma = '\0';
        if (*part == '\0') return INVALID_FIELD;

        enum result result = add_part(part, min, max, matches);
        if (result != OK) return result;

        if (!comma) break;
        part = comma + 1;
    }
    return OK;
}

static void print_error(enum result result) {
    switch (result) {
        case OUT_OF_RANGE: printf("ERR out of range\n"); break;
        case INVALID_STEP: printf("ERR invalid step\n"); break;
        case INVALID_RANGE: printf("ERR invalid range\n"); break;
        default: printf("ERR invalid field\n"); break;
    }
}

int main(void) {
    char line[512];

    while (fgets(line, sizeof line, stdin)) {
        char field[256], trailing;
        int min, max;
        if (sscanf(line, " %255s %d %d %c", field, &min, &max, &trailing) != 3) {
            printf("ERR invalid field\n");
            continue;
        }
        if (min > max) {
            printf("ERR invalid range\n");
            continue;
        }

        size_t count = (size_t)((long long)max - min + 1);
        unsigned char* matches = calloc(count, sizeof *matches);
        if (!matches) {
            printf("ERR out of range\n");
            continue;
        }

        enum result result = expand_field(field, min, max, matches);
        if (result != OK) {
            print_error(result);
            free(matches);
            continue;
        }

        int first = 1;
        for (int value = min;; value++) {
            if (matches[value - min]) {
                printf("%s%d", first ? "" : ",", value);
                first = 0;
            }
            if (value == max) break;
        }
        printf("\n");
        free(matches);
    }
    return 0;
}
