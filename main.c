#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct datetime {
    int year, month, day, hour, minute, second;
};

struct cron {
    unsigned char minute[60], hour[24], day[31], month[12], weekday[8];
    int day_wildcard, weekday_wildcard;
};

enum policy {
    POLICY_SKIP,
    POLICY_CATCHUP,
    POLICY_LIMITED
};

struct job {
    char id[64];
    char cron_text[256];
    char command[256];
    struct cron cron;
    enum policy policy;
    size_t catchup_limit;
    int active;
    int max_overlap;
    struct datetime added_at;
    struct datetime last_processed;
    int has_last_processed;
};

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

static int parse_cron(const char* text, struct cron* cron) {
    char fields[5][64], trailing;
    if (sscanf(text, " %63s %63s %63s %63s %63s %c",
               fields[0], fields[1], fields[2], fields[3], fields[4],
               &trailing) != 5) {
        return 0;
    }
    memset(cron, 0, sizeof *cron);
    cron->day_wildcard = strcmp(fields[2], "*") == 0;
    cron->weekday_wildcard = strcmp(fields[4], "*") == 0;
    return expand_field(fields[0], 0, 59, cron->minute) &&
           expand_field(fields[1], 0, 23, cron->hour) &&
           expand_field(fields[2], 1, 31, cron->day) &&
           expand_field(fields[3], 1, 12, cron->month) &&
           expand_field(fields[4], 0, 7, cron->weekday);
}

static int is_leap(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int days_in_month(int year, int month) {
    static const int days[] =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && is_leap(year) ? 29 : days[month - 1];
}

static int parse_digits(const char* text, int count) {
    int value = 0;
    for (int i = 0; i < count; i++) {
        if (!isdigit((unsigned char)text[i])) return -1;
        value = value * 10 + text[i] - '0';
    }
    return value;
}

static int parse_datetime(const char* text, struct datetime* date) {
    if (strlen(text) != 19 || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return 0;
    }
    date->year = parse_digits(text, 4);
    date->month = parse_digits(text + 5, 2);
    date->day = parse_digits(text + 8, 2);
    date->hour = parse_digits(text + 11, 2);
    date->minute = parse_digits(text + 14, 2);
    date->second = parse_digits(text + 17, 2);
    return date->year >= 1 && date->month >= 1 && date->month <= 12 &&
           date->day >= 1 &&
           date->day <= days_in_month(date->year, date->month) &&
           date->hour >= 0 && date->hour < 24 &&
           date->minute >= 0 && date->minute < 60 &&
           date->second >= 0 && date->second < 60;
}

static int compare_datetime(const struct datetime* a,
                            const struct datetime* b) {
    const int left[] =
        {a->year, a->month, a->day, a->hour, a->minute, a->second};
    const int right[] =
        {b->year, b->month, b->day, b->hour, b->minute, b->second};
    for (int i = 0; i < 6; i++) {
        if (left[i] != right[i]) return left[i] < right[i] ? -1 : 1;
    }
    return 0;
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

static int weekday(const struct datetime* date) {
    static const int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int year = date->year - (date->month < 3);
    return (year + year / 4 - year / 100 + year / 400 +
            offsets[date->month - 1] + date->day) % 7;
}

static int cron_matches(const struct cron* cron, const struct datetime* date) {
    if (!cron->minute[date->minute] || !cron->hour[date->hour] ||
        !cron->month[date->month - 1]) {
        return 0;
    }
    int day_matches = cron->day[date->day - 1];
    int day_of_week = weekday(date);
    int weekday_matches = cron->weekday[day_of_week] ||
                          (day_of_week == 0 && cron->weekday[7]);
    if (!cron->day_wildcard && !cron->weekday_wildcard) {
        return day_matches || weekday_matches;
    }
    if (!cron->day_wildcard) return day_matches;
    if (!cron->weekday_wildcard) return weekday_matches;
    return 1;
}

static int next_run(const struct cron* cron, const struct datetime* after,
                    struct datetime* next) {
    *next = *after;
    add_minute(next);
    int end_year = after->year + 4;
    while (next->year <= end_year) {
        if (cron_matches(cron, next)) return 1;
        add_minute(next);
    }
    return 0;
}

static int find_job(const struct job* jobs, size_t count, const char* id) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(jobs[i].id, id) == 0) return (int)i;
    }
    return -1;
}

static void print_datetime(const struct datetime* date) {
    printf("%04d-%02d-%02dT%02d:%02d:%02d",
           date->year, date->month, date->day,
           date->hour, date->minute, date->second);
}

static int parse_job_args(char* text, const struct datetime* now,
                          struct job* job) {
    char* p = text;
    while (isspace((unsigned char)*p)) p++;
    char* id = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    if (*p) *p++ = '\0';
    while (isspace((unsigned char)*p)) p++;
    if (*id == '\0' || strlen(id) >= sizeof job->id || *p != '"') return 0;

    char* cron_text = ++p;
    char* quote = strchr(p, '"');
    if (!quote) return 0;
    *quote = '\0';
    p = quote + 1;
    while (isspace((unsigned char)*p)) p++;

    char* policy_text = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    if (*p) *p++ = '\0';
    while (isspace((unsigned char)*p)) p++;

    char* overlap_text = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    if (*p) *p++ = '\0';
    while (isspace((unsigned char)*p)) p++;

    if (strcmp(policy_text, "skip") == 0) {
        job->policy = POLICY_SKIP;
        job->catchup_limit = 1;
    } else if (strcmp(policy_text, "catchup") == 0) {
        job->policy = POLICY_CATCHUP;
        job->catchup_limit = 0;
    } else if (strncmp(policy_text, "catchup:", 8) == 0) {
        int limit;
        if (!parse_number(policy_text + 8, &limit) || limit < 0) return 0;
        job->policy = POLICY_LIMITED;
        job->catchup_limit = (size_t)limit;
    } else {
        return 0;
    }

    if (!parse_number(overlap_text, &job->max_overlap) ||
        job->max_overlap <= 0 || *p == '\0' ||
        strlen(cron_text) >= sizeof job->cron_text ||
        strlen(p) >= sizeof job->command ||
        !parse_cron(cron_text, &job->cron)) {
        return 0;
    }

    snprintf(job->id, sizeof job->id, "%s", id);
    snprintf(job->cron_text, sizeof job->cron_text, "%s", cron_text);
    snprintf(job->command, sizeof job->command, "%s", p);
    job->active = 0;
    job->added_at = *now;
    job->has_last_processed = 0;
    return 1;
}

static int append_datetime(struct datetime** dates, size_t* count,
                           size_t* capacity, const struct datetime* date) {
    if (*count == *capacity) {
        size_t new_capacity = *capacity ? *capacity * 2 : 8;
        struct datetime* resized =
            realloc(*dates, new_capacity * sizeof **dates);
        if (!resized) return 0;
        *dates = resized;
        *capacity = new_capacity;
    }
    (*dates)[(*count)++] = *date;
    return 1;
}

static int compare_job_pointers(const void* left, const void* right) {
    const struct job* const* a = left;
    const struct job* const* b = right;
    return strcmp((*a)->id, (*b)->id);
}

int main(void) {
    char line[512];
    struct datetime now = {0};
    int has_now = 0;
    struct job* jobs = NULL;
    size_t job_count = 0, job_capacity = 0;

    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';

        if (strncmp(line, "NOW ", 4) == 0) {
            char iso[64], trailing;
            if (sscanf(line, "NOW %63s %c", iso, &trailing) != 1 ||
                !parse_datetime(iso, &now)) {
                printf("ERR\n");
            } else {
                has_now = 1;
            }
        } else if (strncmp(line, "ADD ", 4) == 0) {
            if (!has_now) {
                printf("ERR\n");
                continue;
            }

            if (job_count == job_capacity) {
                size_t capacity = job_capacity ? job_capacity * 2 : 8;
                struct job* resized =
                    realloc(jobs, capacity * sizeof *jobs);
                if (!resized) {
                    printf("ERR\n");
                    continue;
                }
                jobs = resized;
                job_capacity = capacity;
            }
            if (!parse_job_args(line + 4, &now, &jobs[job_count])) {
                printf("ERR\n");
                continue;
            }
            job_count++;
        } else if (strcmp(line, "TICK") == 0) {
            if (!has_now) {
                printf("ERR\n");
                continue;
            }

            for (size_t i = 0; i < job_count; i++) {
                struct job* job = &jobs[i];
                struct datetime cursor = job->has_last_processed
                                             ? job->last_processed
                                             : job->added_at;
                struct datetime occurrence;
                struct datetime* occurrences = NULL;
                size_t count = 0, capacity = 0;

                while (next_run(&job->cron, &cursor, &occurrence) &&
                       compare_datetime(&occurrence, &now) <= 0) {
                    if (!append_datetime(&occurrences, &count, &capacity,
                                         &occurrence)) {
                        free(occurrences);
                        free(jobs);
                        return 1;
                    }
                    cursor = occurrence;
                }

                size_t first = 0;
                if (job->policy == POLICY_SKIP && count > 1) {
                    first = count - 1;
                } else if (job->policy == POLICY_LIMITED &&
                           count > job->catchup_limit) {
                    first = count - job->catchup_limit;
                }

                for (size_t j = first; j < count; j++) {
                    if (job->active < job->max_overlap) {
                        job->active++;
                        printf("RAN %s for ", job->id);
                    } else {
                        printf("SKIP %s for ", job->id);
                    }
                    print_datetime(&occurrences[j]);
                    printf("\n");
                }
                if (first < count) {
                    job->last_processed = occurrences[count - 1];
                    job->has_last_processed = 1;
                }
                free(occurrences);
            }
            printf("OK\n");
        } else if (strncmp(line, "COMPLETE ", 9) == 0) {
            char id[64], trailing;
            if (sscanf(line, "COMPLETE %63s %c", id, &trailing) != 1) {
                printf("ERR\n");
                continue;
            }
            int index = find_job(jobs, job_count, id);
            if (index < 0) {
                printf("ERR\n");
            } else if (jobs[index].active > 0) {
                jobs[index].active--;
            }
        } else if (strcmp(line, "PERSIST") == 0) {
            struct job** sorted = malloc(job_count * sizeof *sorted);
            if (!sorted && job_count) {
                printf("ERR\n");
                continue;
            }
            for (size_t i = 0; i < job_count; i++) sorted[i] = &jobs[i];
            if (job_count > 1) {
                qsort(sorted, job_count, sizeof *sorted,
                      compare_job_pointers);
            }
            for (size_t i = 0; i < job_count; i++) {
                struct datetime next;
                printf("{\"id\":\"%s\",\"cron\":\"%s\",\"last_run\":",
                       sorted[i]->id, sorted[i]->cron_text);
                if (sorted[i]->has_last_processed) {
                    printf("\"");
                    print_datetime(&sorted[i]->last_processed);
                    printf("\"");
                } else {
                    printf("null");
                }
                printf(",\"next_run\":\"");
                if (next_run(&sorted[i]->cron, &now, &next)) {
                    print_datetime(&next);
                } else {
                    printf("NEVER");
                }
                printf("\"}\n");
            }
            free(sorted);
        } else if (strncmp(line, "RELOAD ", 7) == 0) {
            int reload_count;
            char trailing;
            if (!has_now ||
                sscanf(line, "RELOAD %d %c", &reload_count, &trailing) != 1 ||
                reload_count < 0) {
                printf("ERR\n");
                continue;
            }

            free(jobs);
            jobs = NULL;
            job_count = 0;
            job_capacity = (size_t)reload_count;
            if (job_capacity) {
                jobs = malloc(job_capacity * sizeof *jobs);
                if (!jobs) return 1;
            }

            int valid = 1;
            for (int i = 0; i < reload_count; i++) {
                if (!fgets(line, sizeof line, stdin)) {
                    valid = 0;
                    break;
                }
                line[strcspn(line, "\r\n")] = '\0';
                if (!parse_job_args(line, &now, &jobs[job_count])) {
                    valid = 0;
                } else {
                    job_count++;
                }
            }
            if (!valid) printf("ERR\n");
        } else {
            printf("ERR\n");
        }
    }

    free(jobs);
    return 0;
}
