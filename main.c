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

struct datetime {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
};

static int parse_digits(const char* text, int length) {
    int value = 0;
    for (int i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') return -1;
        value = value * 10 + text[i] - '0';
    }
    return value;
}

static int is_leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int parse_datetime(const char* text, struct datetime* date) {
    static const int DAYS_PER_MONTH[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    if (strlen(text) != 19 ||
        text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':') {
        return 0;
    }

    date->year = parse_digits(text, 4);
    date->month = parse_digits(text + 5, 2);
    date->day = parse_digits(text + 8, 2);
    date->hour = parse_digits(text + 11, 2);
    date->minute = parse_digits(text + 14, 2);
    date->second = parse_digits(text + 17, 2);

    if (date->year < 1 || date->month < 1 || date->month > 12 ||
        date->hour < 0 || date->hour > 23 ||
        date->minute < 0 || date->minute > 59 ||
        date->second < 0 || date->second > 59) {
        return 0;
    }

    int days = DAYS_PER_MONTH[date->month - 1];
    if (date->month == 2 && is_leap_year(date->year)) days++;
    return date->day >= 1 && date->day <= days;
}

static int day_of_week(const struct datetime* date) {
    static const int OFFSETS[] = {
        0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4
    };
    int year = date->year;
    if (date->month < 3) year--;
    return (year + year / 4 - year / 100 + year / 400 +
            OFFSETS[date->month - 1] + date->day) % 7;
}

static int matches_cron(char fields[5][64], const struct datetime* date) {
    unsigned char minutes[60] = {0};
    unsigned char hours[24] = {0};
    unsigned char month_days[31] = {0};
    unsigned char months[12] = {0};
    unsigned char week_days[8] = {0};
    int dom_wildcard = strcmp(fields[2], "*") == 0;
    int dow_wildcard = strcmp(fields[4], "*") == 0;

    if (expand_field(fields[0], 0, 59, minutes) != OK ||
        expand_field(fields[1], 0, 23, hours) != OK ||
        expand_field(fields[2], 1, 31, month_days) != OK ||
        expand_field(fields[3], 1, 12, months) != OK ||
        expand_field(fields[4], 0, 7, week_days) != OK) {
        return 0;
    }

    if (!minutes[date->minute] || !hours[date->hour] ||
        !months[date->month - 1]) {
        return 0;
    }

    int dom_matches = month_days[date->day - 1];
    int weekday = day_of_week(date);
    int dow_matches = week_days[weekday] || (weekday == 0 && week_days[7]);

    if (!dom_wildcard && !dow_wildcard) return dom_matches || dow_matches;
    if (!dom_wildcard) return dom_matches;
    if (!dow_wildcard) return dow_matches;
    return 1;
}

int main(void) {
    char line[512];

    while (fgets(line, sizeof line, stdin)) {
        char fields[5][64];
        char separator, iso[64], trailing;
        int parsed = sscanf(line, " %63s %63s %63s %63s %63s %c %63s %c",
                            fields[0], fields[1], fields[2], fields[3], fields[4],
                            &separator, iso, &trailing);
        struct datetime date;

        if (parsed != 7 || separator != '|' || !parse_datetime(iso, &date)) {
            printf("NO\n");
            continue;
        }
        printf("%s\n", matches_cron(fields, &date) ? "YES" : "NO");
    }
    return 0;
}
