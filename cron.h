#ifndef CRON_H
#define CRON_H

#include <stddef.h>
#include <time.h>

struct cron_schedule {
    unsigned char minute[60];
    unsigned char hour[24];
    unsigned char month_day[31];
    unsigned char month[12];
    unsigned char week_day[8];
    int month_day_wildcard;
    int week_day_wildcard;
};

int cron_parse(const char* expression, struct cron_schedule* schedule);
int cron_matches(const struct cron_schedule* schedule, time_t instant);
time_t cron_next(const struct cron_schedule* schedule, time_t after);
int parse_iso_utc(const char* text, time_t* result);
void format_iso_utc(time_t instant, char* output, size_t output_size);

#endif
