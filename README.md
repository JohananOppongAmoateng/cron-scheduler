# Cron Scheduler

A small Linux/POSIX cron daemon written in C. It stores jobs and run history in
SQLite, executes commands through `/bin/sh -c`, captures output, supports missed
job policies, and prevents excessive overlap.

This began as the
[Build a Cron Scheduler from Scratch](https://shipthatcode.com/courses/build-cron-scheduler)
course project. The final version is a real daemon rather than the course's
stdin simulation, so the earlier lesson fixtures no longer target this binary.

## Requirements

- A C11 compiler
- `make`
- SQLite 3 development headers and library

On Arch Linux:

```sh
sudo pacman -S base-devel sqlite
```

On Ubuntu/Debian:

```sh
sudo apt install build-essential libsqlite3-dev sqlite3
```

## Build

```sh
make
```

## Add and inspect jobs

Commands use UTC. Cron supports wildcards, steps, ranges, comma lists, and
classic day-of-month/day-of-week OR semantics.

```sh
./cron-scheduler add backup "0 3 * * *" catchup 1 \
  "pg_dump prod > /var/backups/prod.sql"

./cron-scheduler add heartbeat "*/5 * * * *" skip 1 \
  "curl -fsS https://example.com/heartbeat"

./cron-scheduler list
./cron-scheduler runs
./cron-scheduler runs backup
./cron-scheduler remove heartbeat
```

Policies:

- `skip`: run only the latest missed occurrence.
- `catchup`: run every missed occurrence.
- `catchup:N`: run only the latest `N` missed occurrences.

`max_overlap` limits concurrent processes for one job. Instances exceeding the
limit are recorded as `overlap_skipped`.

Use another database with the global option:

```sh
./cron-scheduler --db /var/lib/cron-scheduler/jobs.db list
```

## Run

Run continuously in the foreground:

```sh
./cron-scheduler daemon
```

Process everything currently due, wait for launched commands, and exit:

```sh
./cron-scheduler run-once
```

The daemon checks the next firing at least once per minute. `SIGHUP` wakes it
and reloads jobs from SQLite. `SIGTERM` or `SIGINT` stops new launches, waits
for child processes, records their results, and exits.

Example systemd service:

```ini
[Unit]
Description=Cron Scheduler
After=local-fs.target

[Service]
Type=simple
WorkingDirectory=/opt/cron-scheduler
ExecStart=/opt/cron-scheduler/cron-scheduler --db /var/lib/cron-scheduler/jobs.db daemon
Restart=on-failure
User=cron-scheduler

[Install]
WantedBy=multi-user.target
```

## Logs and persistence

SQLite is initialized automatically. The `jobs` table stores definitions and
the last processed occurrence; the `runs` table stores scheduled times, PIDs,
statuses, exit codes, and log paths. Child stdout and stderr are combined under
`logs/<job>-<scheduled-epoch>.log`.

Commands are trusted operator configuration and run through `/bin/sh -c`, like
traditional cron. Do not allow untrusted users to write the database or submit
job commands. Run the daemon as a dedicated, minimally privileged account.
