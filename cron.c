#define _GNU_SOURCE

#include "cron.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_number(const char* text, int* value) {
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

static int expand_part(char* part, int min, int max, unsigned char* values) {
    int first, last, step = 1;
    char* slash = strchr(part, '/');

    if (slash) {
        if (strchr(slash + 1, '/')) return 0;
        *slash++ = '\0';
        if (!parse_number(slash, &step) || step <= 0) return 0;
    }

    if (strcmp(part, "*") == 0) {
        first = min;
        last = max;
    } else if (parse_number(part, &first)) {
        last = slash ? max : first;
    } else {
        char trailing;
        if (sscanf(part, "%d-%d%c", &first, &last, &trailing) != 2 ||
            first > last) {
            return 0;
        }
    }
    if (first < min || last > max) return 0;

    for (int value = first; value <= last; value += step) {
        values[value - min] = 1;
        if (value > last - step) break;
    }
    return 1;
}

static int expand_field(char* field, int min, int max, unsigned char* values) {
    char* part = field;
    if (*part == '\0') return 0;

    for (;;) {
        char* comma = strchr(part, ',');
        if (comma) *comma = '\0';
        if (*part == '\0' || !expand_part(part, min, max, values)) return 0;
        if (!comma) return 1;
        part = comma + 1;
    }
}

int cron_parse(const char* expression, struct cron_schedule* schedule) {
    char fields[5][64], trailing;
    if (sscanf(expression, " %63s %63s %63s %63s %63s %c",
               fields[0], fields[1], fields[2], fields[3], fields[4],
               &trailing) != 5) {
        return 0;
    }

    memset(schedule, 0, sizeof *schedule);
    schedule->month_day_wildcard = strcmp(fields[2], "*") == 0;
    schedule->week_day_wildcard = strcmp(fields[4], "*") == 0;
    return expand_field(fields[0], 0, 59, schedule->minute) &&
           expand_field(fields[1], 0, 23, schedule->hour) &&
           expand_field(fields[2], 1, 31, schedule->month_day) &&
           expand_field(fields[3], 1, 12, schedule->month) &&
           expand_field(fields[4], 0, 7, schedule->week_day);
}

int cron_matches(const struct cron_schedule* schedule, time_t instant) {
    struct tm value;
    if (!gmtime_r(&instant, &value)) return 0;

    if (!schedule->minute[value.tm_min] ||
        !schedule->hour[value.tm_hour] ||
        !schedule->month[value.tm_mon]) {
        return 0;
    }

    int month_day_matches = schedule->month_day[value.tm_mday - 1];
    int week_day_matches = schedule->week_day[value.tm_wday] ||
                           (value.tm_wday == 0 && schedule->week_day[7]);

    if (!schedule->month_day_wildcard && !schedule->week_day_wildcard) {
        return month_day_matches || week_day_matches;
    }
    if (!schedule->month_day_wildcard) return month_day_matches;
    if (!schedule->week_day_wildcard) return week_day_matches;
    return 1;
}

time_t cron_next(const struct cron_schedule* schedule, time_t after) {
    time_t candidate = after - after % 60 + 60;
    const time_t limit = after + (time_t)4 * 366 * 24 * 60 * 60;

    while (candidate <= limit) {
        if (cron_matches(schedule, candidate)) return candidate;
        candidate += 60;
    }
    return (time_t)-1;
}

int parse_iso_utc(const char* text, time_t* result) {
    struct tm value = {0};
    char trailing;
    if (sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d%c",
               &value.tm_year, &value.tm_mon, &value.tm_mday,
               &value.tm_hour, &value.tm_min, &value.tm_sec, &trailing) != 6) {
        return 0;
    }
    value.tm_year -= 1900;
    value.tm_mon -= 1;

    struct tm original = value;
    time_t parsed = timegm(&value);
    struct tm checked;
    if (parsed == (time_t)-1 || !gmtime_r(&parsed, &checked) ||
        checked.tm_year != original.tm_year ||
        checked.tm_mon != original.tm_mon ||
        checked.tm_mday != original.tm_mday ||
        checked.tm_hour != original.tm_hour ||
        checked.tm_min != original.tm_min ||
        checked.tm_sec != original.tm_sec) {
        return 0;
    }
    *result = parsed;
    return 1;
}

void format_iso_utc(time_t instant, char* output, size_t output_size) {
    struct tm value;
    if (!gmtime_r(&instant, &value) ||
        strftime(output, output_size, "%Y-%m-%dT%H:%M:%S", &value) == 0) {
        if (output_size) output[0] = '\0';
    }
}
