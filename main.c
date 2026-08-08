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

struct job {
    char id[64];
    char command[256];
    struct cron cron;
    struct datetime next;
    int active;
    int max_overlap;
    int skipped;
};

struct heap {
    size_t* items;
    size_t size, capacity;
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

static void add_seconds(struct datetime* date, int seconds) {
    long long second_of_day = (long long)date->hour * 3600 +
                              date->minute * 60 + date->second + seconds;
    while (second_of_day >= 24 * 3600) {
        second_of_day -= 24 * 3600;
        if (++date->day > days_in_month(date->year, date->month)) {
            date->day = 1;
            if (++date->month > 12) {
                date->month = 1;
                date->year++;
            }
        }
    }
    date->hour = (int)(second_of_day / 3600);
    date->minute = (int)(second_of_day % 3600 / 60);
    date->second = (int)(second_of_day % 60);
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

static int heap_compare(size_t a, size_t b, const struct job* jobs) {
    int by_time = compare_datetime(&jobs[a].next, &jobs[b].next);
    if (by_time) return by_time;
    return a < b ? -1 : a > b;
}

static int heap_push(struct heap* heap, size_t job_index,
                     const struct job* jobs) {
    if (heap->size == heap->capacity) {
        size_t capacity = heap->capacity ? heap->capacity * 2 : 8;
        size_t* resized = realloc(heap->items, capacity * sizeof *heap->items);
        if (!resized) return 0;
        heap->items = resized;
        heap->capacity = capacity;
    }
    size_t index = heap->size++;
    heap->items[index] = job_index;
    while (index) {
        size_t parent = (index - 1) / 2;
        if (heap_compare(heap->items[parent], heap->items[index], jobs) <= 0) {
            break;
        }
        size_t temporary = heap->items[parent];
        heap->items[parent] = heap->items[index];
        heap->items[index] = temporary;
        index = parent;
    }
    return 1;
}

static size_t heap_pop(struct heap* heap, const struct job* jobs) {
    size_t result = heap->items[0];
    heap->items[0] = heap->items[--heap->size];
    size_t index = 0;
    for (;;) {
        size_t left = index * 2 + 1, right = left + 1, smallest = index;
        if (left < heap->size &&
            heap_compare(heap->items[left], heap->items[smallest], jobs) < 0) {
            smallest = left;
        }
        if (right < heap->size &&
            heap_compare(heap->items[right], heap->items[smallest], jobs) < 0) {
            smallest = right;
        }
        if (smallest == index) break;
        size_t temporary = heap->items[index];
        heap->items[index] = heap->items[smallest];
        heap->items[smallest] = temporary;
        index = smallest;
    }
    return result;
}

static int find_job(const struct job* jobs, size_t count, const char* id) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(jobs[i].id, id) == 0) return (int)i;
    }
    return -1;
}

int main(void) {
    char line[512];
    struct datetime now = {0};
    int has_now = 0;
    struct job* jobs = NULL;
    size_t job_count = 0, job_capacity = 0;
    struct heap heap = {0};

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
            char* p = line + 4;
            while (isspace((unsigned char)*p)) p++;
            char* id = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) *p++ = '\0';
            while (isspace((unsigned char)*p)) p++;
            if (!has_now || *id == '\0' || strlen(id) >= 64 || *p != '"') {
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
            while (isspace((unsigned char)*p)) p++;
            char* overlap_text = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) *p++ = '\0';
            while (isspace((unsigned char)*p)) p++;

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
            struct job* job = &jobs[job_count];
            snprintf(job->id, sizeof job->id, "%s", id);
            snprintf(job->command, sizeof job->command, "%s", p);
            if (!parse_number(overlap_text, &job->max_overlap) ||
                job->max_overlap < 0 || *p == '\0' ||
                !parse_cron(cron_text, &job->cron)) {
                printf("ERR\n");
                continue;
            }
            job->active = 0;
            job->skipped = 0;
            if (cron_matches(&job->cron, &now)) {
                job->next = now;
                job->next.second = 0;
            } else if (!next_run(&job->cron, &now, &job->next)) {
                printf("ERR\n");
                continue;
            }
            if (!heap_push(&heap, job_count, jobs)) {
                printf("ERR\n");
                continue;
            }
            job_count++;
        } else if (strncmp(line, "RUN_TICK ", 9) == 0) {
            int duration;
            char trailing;
            if (!has_now ||
                sscanf(line, "RUN_TICK %d %c", &duration, &trailing) != 1 ||
                duration < 0) {
                printf("ERR\n");
                continue;
            }
            while (heap.size &&
                   compare_datetime(&jobs[heap.items[0]].next, &now) <= 0) {
                size_t index = heap_pop(&heap, jobs);
                struct job* job = &jobs[index];
                if (job->active < job->max_overlap) {
                    job->active++;
                    printf("RAN %s\n", job->id);
                } else {
                    job->skipped++;
                    printf("SKIP %s\n", job->id);
                }
                if (next_run(&job->cron, &now, &job->next)) {
                    heap_push(&heap, index, jobs);
                }
            }
            add_seconds(&now, 60);
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
        } else if (strncmp(line, "STATUS ", 7) == 0) {
            char id[64], trailing;
            if (sscanf(line, "STATUS %63s %c", id, &trailing) != 1) {
                printf("ERR\n");
                continue;
            }
            int index = find_job(jobs, job_count, id);
            if (index < 0) {
                printf("ERR\n");
            } else {
                printf("%s active=%d max=%d skipped=%d\n",
                       jobs[index].id, jobs[index].active,
                       jobs[index].max_overlap, jobs[index].skipped);
            }
        } else {
            printf("ERR\n");
        }
    }

    free(heap.items);
    free(jobs);
    return 0;
}
