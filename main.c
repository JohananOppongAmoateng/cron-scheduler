#include <errno.h>
#include <ctype.h>
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

struct job {
    char id[64];
    char cron_text[256];
    char command[512];
    struct cron_schedule cron;
};

static int parse_cron_text(const char* text, struct cron_schedule* cron) {
    char fields[5][64], trailing;
    if (sscanf(text, " %63s %63s %63s %63s %63s %c",
               fields[0], fields[1], fields[2], fields[3], fields[4],
               &trailing) != 5) {
        return 0;
    }
    return parse_cron(fields, cron);
}

static int find_job(const struct job* jobs, size_t count, const char* id) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(jobs[i].id, id) == 0) return (int)i;
    }
    return -1;
}

static int compare_job_ids(const void* left, const void* right) {
    const struct job* const* a = left;
    const struct job* const* b = right;
    return strcmp((*a)->id, (*b)->id);
}

static void print_next_run(const struct job* job,
                           const struct datetime* now) {
    struct datetime next;
    if (!next_run(&job->cron, now, &next)) {
        printf("NEVER");
        return;
    }
    printf("%04d-%02d-%02dT%02d:%02d:00",
           next.year, next.month, next.day, next.hour, next.minute);
}

int main(void) {
    char line[512];
    struct job* jobs = NULL;
    size_t count = 0;
    size_t capacity = 0;
    struct datetime now;
    parse_datetime("2025-03-17T00:00:00", &now);

    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';

        if (strncmp(line, "ADD", 3) == 0 && isspace((unsigned char)line[3])) {
            char* p = line + 3;
            while (isspace((unsigned char)*p)) p++;
            char* id = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) *p++ = '\0';
            while (isspace((unsigned char)*p)) p++;

            if (*id == '\0' || strlen(id) >= sizeof jobs[0].id || *p != '"') {
                printf("ERR\n");
                continue;
            }
            char* cron_text = ++p;
            char* quote = strchr(p, '"');
            if (!quote) {
                printf("ERR\n");
                continue;
            }
            *quote = '\0';
            p = quote + 1;
            if (*p && !isspace((unsigned char)*p)) {
                printf("ERR\n");
                continue;
            }
            while (isspace((unsigned char)*p)) p++;
            char* command = p;

            struct cron_schedule cron;
            if (*command == '\0' ||
                strlen(cron_text) >= sizeof jobs[0].cron_text ||
                strlen(command) >= sizeof jobs[0].command ||
                !parse_cron_text(cron_text, &cron)) {
                printf("ERR\n");
                continue;
            }

            int index = find_job(jobs, count, id);
            if (index < 0) {
                if (count == capacity) {
                    size_t new_capacity = capacity ? capacity * 2 : 8;
                    struct job* resized =
                        realloc(jobs, new_capacity * sizeof *jobs);
                    if (!resized) {
                        printf("ERR\n");
                        continue;
                    }
                    jobs = resized;
                    capacity = new_capacity;
                }
                index = (int)count++;
            }

            snprintf(jobs[index].id, sizeof jobs[index].id, "%s", id);
            snprintf(jobs[index].cron_text, sizeof jobs[index].cron_text,
                     "%s", cron_text);
            snprintf(jobs[index].command, sizeof jobs[index].command,
                     "%s", command);
            jobs[index].cron = cron;
            printf("OK\n");
            continue;
        }

        if (strcmp(line, "LIST") == 0) {
            struct job** sorted = malloc(count * sizeof *sorted);
            if (!sorted && count != 0) {
                printf("ERR\n");
                continue;
            }
            for (size_t i = 0; i < count; i++) sorted[i] = &jobs[i];
            if (count > 1) {
                qsort(sorted, count, sizeof *sorted, compare_job_ids);
            }
            for (size_t i = 0; i < count; i++) {
                printf("%s|%s|%s|", sorted[i]->id, sorted[i]->cron_text,
                       sorted[i]->command);
                print_next_run(sorted[i], &now);
                printf("\n");
            }
            free(sorted);
            continue;
        }

        char id[64], trailing;
        if (sscanf(line, "REMOVE %63s %c", id, &trailing) == 1) {
            int index = find_job(jobs, count, id);
            if (index < 0) {
                printf("ERR no such job\n");
                continue;
            }
            jobs[index] = jobs[count - 1];
            count--;
            printf("OK\n");
            continue;
        }

        if (sscanf(line, "NEXT %63s %c", id, &trailing) == 1) {
            int index = find_job(jobs, count, id);
            if (index < 0) {
                printf("ERR\n");
                continue;
            }
            print_next_run(&jobs[index], &now);
            printf("\n");
            continue;
        }

        printf("ERR\n");
    }
    free(jobs);
    return 0;
}
