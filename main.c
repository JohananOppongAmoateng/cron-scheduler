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

struct cron_schedule {
    unsigned char minutes[60];
    unsigned char hours[24];
    unsigned char month_days[31];
    unsigned char months[12];
    unsigned char week_days[8];
    int dom_wildcard;
    int dow_wildcard;
};

static int parse_cron(char fields[5][64], struct cron_schedule* cron) {
    memset(cron, 0, sizeof *cron);
    cron->dom_wildcard = strcmp(fields[2], "*") == 0;
    cron->dow_wildcard = strcmp(fields[4], "*") == 0;

    if (expand_field(fields[0], 0, 59, cron->minutes) != OK ||
        expand_field(fields[1], 0, 23, cron->hours) != OK ||
        expand_field(fields[2], 1, 31, cron->month_days) != OK ||
        expand_field(fields[3], 1, 12, cron->months) != OK ||
        expand_field(fields[4], 0, 7, cron->week_days) != OK) {
        return 0;
    }
    return 1;
}

static int matches_cron(const struct cron_schedule* cron,
                        const struct datetime* date) {
    if (!cron->minutes[date->minute] || !cron->hours[date->hour] ||
        !cron->months[date->month - 1]) {
        return 0;
    }

    int dom_matches = cron->month_days[date->day - 1];
    int weekday = day_of_week(date);
    int dow_matches = cron->week_days[weekday] ||
                      (weekday == 0 && cron->week_days[7]);

    if (!cron->dom_wildcard && !cron->dow_wildcard) {
        return dom_matches || dow_matches;
    }
    if (!cron->dom_wildcard) return dom_matches;
    if (!cron->dow_wildcard) return dow_matches;
    return 1;
}

static int days_in_month(int year, int month) {
    static const int DAYS_PER_MONTH[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month == 2 && is_leap_year(year)) return 29;
    return DAYS_PER_MONTH[month - 1];
}

static int shift_minutes(struct datetime* date, long long offset) {
    long long minute_of_day =
        (long long)date->hour * 60 + date->minute + offset;

    while (minute_of_day < 0) {
        minute_of_day += 24 * 60;
        if (--date->day < 1) {
            if (--date->month < 1) {
                if (--date->year < 1) return 0;
                date->month = 12;
            }
            date->day = days_in_month(date->year, date->month);
        }
    }
    while (minute_of_day >= 24 * 60) {
        minute_of_day -= 24 * 60;
        if (++date->day > days_in_month(date->year, date->month)) {
            date->day = 1;
            if (++date->month > 12) {
                if (++date->year > 9999) return 0;
                date->month = 1;
            }
        }
    }

    date->hour = (int)(minute_of_day / 60);
    date->minute = (int)(minute_of_day % 60);
    return 1;
}

static void add_minute(struct datetime* date) {
    date->second = 0;
    if (++date->minute < 60) return;
    date->minute = 0;
    if (++date->hour < 24) return;
    date->hour = 0;
    if (++date->day <= days_in_month(date->year, date->month)) return;
    date->day = 1;
    if (++date->month <= 12) return;
    date->month = 1;
    date->year++;
}

static int compare_datetime(const struct datetime* left,
                            const struct datetime* right) {
    if (left->year != right->year) return left->year < right->year ? -1 : 1;
    if (left->month != right->month) return left->month < right->month ? -1 : 1;
    if (left->day != right->day) return left->day < right->day ? -1 : 1;
    if (left->hour != right->hour) return left->hour < right->hour ? -1 : 1;
    if (left->minute != right->minute) {
        return left->minute < right->minute ? -1 : 1;
    }
    if (left->second != right->second) {
        return left->second < right->second ? -1 : 1;
    }
    return 0;
}

static int next_run(const struct cron_schedule* cron,
                    const struct datetime* after,
                    struct datetime* next) {
    *next = *after;
    add_minute(next);

    struct datetime limit = *after;
    if (limit.year > 9995) return 0;
    limit.year += 4;
    int last_day = days_in_month(limit.year, limit.month);
    if (limit.day > last_day) limit.day = last_day;

    while (compare_datetime(next, &limit) <= 0) {
        if (matches_cron(cron, next)) return 1;
        add_minute(next);
    }
    return 0;
}

int main(void) {
    char line[512];

    while (fgets(line, sizeof line, stdin)) {
        char fields[5][64];
        char first_separator, second_separator, iso[64], trailing;
        int offset;
        int parsed = sscanf(line,
                            " %63s %63s %63s %63s %63s %c %63s %c %d %c",
                            fields[0], fields[1], fields[2], fields[3], fields[4],
                            &first_separator, iso, &second_separator, &offset,
                            &trailing);
        struct datetime after_utc, after_local, next_local, next_utc;
        struct cron_schedule cron;

        if (parsed != 9 || first_separator != '|' || second_separator != '|' ||
            offset < -24 * 60 || offset > 24 * 60 ||
            !parse_datetime(iso, &after_utc) || !parse_cron(fields, &cron)) {
            printf("NEVER\n");
            continue;
        }

        after_local = after_utc;
        if (!shift_minutes(&after_local, offset) ||
            !next_run(&cron, &after_local, &next_local)) {
            printf("NEVER\n");
            continue;
        }

        next_utc = next_local;
        if (!shift_minutes(&next_utc, -(long long)offset)) {
            printf("NEVER\n");
            continue;
        }
        printf("%04d-%02d-%02dT%02d:%02d:00\n",
               next_utc.year, next_utc.month, next_utc.day,
               next_utc.hour, next_utc.minute);
    }
    return 0;
}
