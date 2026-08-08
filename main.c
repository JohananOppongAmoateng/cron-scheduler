#define _POSIX_C_SOURCE 200809L

#include "cron.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum job_policy {
    POLICY_SKIP,
    POLICY_CATCHUP,
    POLICY_LIMITED
};

struct job {
    char* id;
    char* expression;
    char* command;
    struct cron_schedule schedule;
    enum job_policy policy;
    size_t catchup_limit;
    int max_overlap;
    time_t last_processed;
};

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t reload_requested;
static volatile sig_atomic_t child_changed;

static void handle_signal(int signal_number) {
    if (signal_number == SIGTERM || signal_number == SIGINT) {
        stop_requested = 1;
    } else if (signal_number == SIGHUP) {
        reload_requested = 1;
    } else if (signal_number == SIGCHLD) {
        child_changed = 1;
    }
}

static int execute_sql(sqlite3* db, const char* sql) {
    char* error = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) {
        fprintf(stderr, "sqlite: %s\n", error ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
        return 0;
    }
    return 1;
}

static sqlite3* open_database(const char* path) {
    sqlite3* db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "cannot open database %s: %s\n",
                path, db ? sqlite3_errmsg(db) : "out of memory");
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);

    const char* schema =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS jobs ("
        " id TEXT PRIMARY KEY,"
        " cron TEXT NOT NULL,"
        " policy TEXT NOT NULL CHECK(policy IN ('skip','catchup','limited')),"
        " catchup_limit INTEGER NOT NULL DEFAULT 0 CHECK(catchup_limit >= 0),"
        " max_overlap INTEGER NOT NULL CHECK(max_overlap > 0),"
        " command TEXT NOT NULL,"
        " enabled INTEGER NOT NULL DEFAULT 1,"
        " created_seq INTEGER NOT NULL,"
        " last_processed INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS runs ("
        " run_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " job_id TEXT NOT NULL,"
        " scheduled_at INTEGER NOT NULL,"
        " started_at INTEGER,"
        " finished_at INTEGER,"
        " pid INTEGER,"
        " status TEXT NOT NULL,"
        " exit_code INTEGER,"
        " log_path TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS runs_job_status"
        " ON runs(job_id, status);";
    if (!execute_sql(db, schema)) {
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

static char* duplicate_column(sqlite3_stmt* statement, int column) {
    const unsigned char* text = sqlite3_column_text(statement, column);
    return text ? strdup((const char*)text) : NULL;
}

static void free_jobs(struct job* jobs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(jobs[i].id);
        free(jobs[i].expression);
        free(jobs[i].command);
    }
    free(jobs);
}

static int parse_policy(const char* text, enum job_policy* policy,
                        size_t* limit) {
    if (strcmp(text, "skip") == 0) {
        *policy = POLICY_SKIP;
        *limit = 1;
        return 1;
    }
    if (strcmp(text, "catchup") == 0) {
        *policy = POLICY_CATCHUP;
        *limit = 0;
        return 1;
    }
    if (strncmp(text, "catchup:", 8) == 0) {
        char* end;
        errno = 0;
        unsigned long parsed = strtoul(text + 8, &end, 10);
        if (!errno && text[8] != '\0' && *end == '\0') {
            *policy = POLICY_LIMITED;
            *limit = (size_t)parsed;
            return 1;
        }
    }
    return 0;
}

static const char* policy_name(enum job_policy policy) {
    if (policy == POLICY_SKIP) return "skip";
    if (policy == POLICY_CATCHUP) return "catchup";
    return "limited";
}

static int load_jobs(sqlite3* db, struct job** output, size_t* output_count) {
    const char* sql =
        "SELECT id, cron, command, policy, catchup_limit, max_overlap,"
        " last_processed FROM jobs WHERE enabled=1 ORDER BY created_seq";
    sqlite3_stmt* statement = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) {
        fprintf(stderr, "sqlite: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    struct job* jobs = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        if (count == capacity) {
            size_t new_capacity = capacity ? capacity * 2 : 8;
            struct job* resized = realloc(jobs, new_capacity * sizeof *jobs);
            if (!resized) {
                free_jobs(jobs, count);
                sqlite3_finalize(statement);
                return 0;
            }
            jobs = resized;
            capacity = new_capacity;
        }

        struct job* job = &jobs[count];
        memset(job, 0, sizeof *job);
        job->id = duplicate_column(statement, 0);
        job->expression = duplicate_column(statement, 1);
        job->command = duplicate_column(statement, 2);
        const char* stored_policy =
            (const char*)sqlite3_column_text(statement, 3);
        job->catchup_limit = (size_t)sqlite3_column_int64(statement, 4);
        job->max_overlap = sqlite3_column_int(statement, 5);
        job->last_processed =
            (time_t)sqlite3_column_int64(statement, 6);

        if (!job->id || !job->expression || !job->command ||
            !cron_parse(job->expression, &job->schedule)) {
            fprintf(stderr, "invalid persisted job at row %zu\n", count + 1);
            free_jobs(jobs, count + 1);
            sqlite3_finalize(statement);
            return 0;
        }
        if (strcmp(stored_policy, "skip") == 0) {
            job->policy = POLICY_SKIP;
        } else if (strcmp(stored_policy, "catchup") == 0) {
            job->policy = POLICY_CATCHUP;
        } else {
            job->policy = POLICY_LIMITED;
        }
        count++;
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        fprintf(stderr, "sqlite: %s\n", sqlite3_errmsg(db));
        free_jobs(jobs, count);
        return 0;
    }
    *output = jobs;
    *output_count = count;
    return 1;
}

static int valid_job_id(const char* id) {
    if (!*id) return 0;
    for (const unsigned char* p = (const unsigned char*)id; *p; p++) {
        if (!(('a' <= *p && *p <= 'z') ||
              ('A' <= *p && *p <= 'Z') ||
              ('0' <= *p && *p <= '9') ||
              *p == '_' || *p == '-' || *p == '.')) {
            return 0;
        }
    }
    return 1;
}

static char* join_arguments(int argc, char** argv, int start) {
    size_t length = 1;
    for (int i = start; i < argc; i++) length += strlen(argv[i]) + 1;
    char* joined = malloc(length);
    if (!joined) return NULL;
    joined[0] = '\0';
    for (int i = start; i < argc; i++) {
        if (i > start) strcat(joined, " ");
        strcat(joined, argv[i]);
    }
    return joined;
}

static int add_job(sqlite3* db, int argc, char** argv, int index) {
    if (argc - index < 5) {
        fprintf(stderr,
                "usage: add <id> <cron> <policy> <max_overlap> <command...>\n");
        return 0;
    }

    const char* id = argv[index];
    const char* expression = argv[index + 1];
    const char* policy_text = argv[index + 2];
    char* end;
    errno = 0;
    long max_overlap = strtol(argv[index + 3], &end, 10);
    struct cron_schedule schedule;
    enum job_policy policy;
    size_t catchup_limit;
    char* command = join_arguments(argc, argv, index + 4);

    if (!valid_job_id(id) || !cron_parse(expression, &schedule) ||
        !parse_policy(policy_text, &policy, &catchup_limit) ||
        errno || *end != '\0' || max_overlap <= 0 || max_overlap > INT_MAX ||
        !command || !*command) {
        fprintf(stderr, "invalid job definition\n");
        free(command);
        return 0;
    }

    const char* sql =
        "INSERT INTO jobs"
        " (id,cron,policy,catchup_limit,max_overlap,command,enabled,"
        "  created_seq,last_processed)"
        " VALUES (?1,?2,?3,?4,?5,?6,1,"
        "  COALESCE((SELECT created_seq FROM jobs WHERE id=?1),"
        "           (SELECT COALESCE(MAX(created_seq),0)+1 FROM jobs)),?7)"
        " ON CONFLICT(id) DO UPDATE SET"
        " cron=excluded.cron, policy=excluded.policy,"
        " catchup_limit=excluded.catchup_limit,"
        " max_overlap=excluded.max_overlap, command=excluded.command,"
        " enabled=1, last_processed=excluded.last_processed";
    sqlite3_stmt* statement = NULL;
    int ok = sqlite3_prepare_v2(db, sql, -1, &statement, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, expression, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, policy_name(policy), -1,
                          SQLITE_STATIC);
        sqlite3_bind_int64(statement, 4, (sqlite3_int64)catchup_limit);
        sqlite3_bind_int(statement, 5, (int)max_overlap);
        sqlite3_bind_text(statement, 6, command, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 7, (sqlite3_int64)time(NULL));
        ok = sqlite3_step(statement) == SQLITE_DONE;
    }
    if (!ok) fprintf(stderr, "sqlite: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(statement);
    free(command);
    return ok;
}

static int list_jobs(sqlite3* db) {
    const char* sql =
        "SELECT id,cron,"
        " CASE policy WHEN 'limited' THEN 'catchup:'||catchup_limit"
        " ELSE policy END,max_overlap,command,last_processed"
        " FROM jobs ORDER BY created_seq";
    sqlite3_stmt* statement = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) {
        fprintf(stderr, "sqlite: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    while (sqlite3_step(statement) == SQLITE_ROW) {
        char iso[32];
        format_iso_utc((time_t)sqlite3_column_int64(statement, 5),
                       iso, sizeof iso);
        printf("%s | %s | %s | max=%d | %s | last=%s\n",
               sqlite3_column_text(statement, 0),
               sqlite3_column_text(statement, 1),
               sqlite3_column_text(statement, 2),
               sqlite3_column_int(statement, 3),
               sqlite3_column_text(statement, 4), iso);
    }
    sqlite3_finalize(statement);
    return 1;
}

static int remove_job(sqlite3* db, const char* id) {
    sqlite3_stmt* statement = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM jobs WHERE id=?1", -1,
                           &statement, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(statement, 1, id, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(statement) == SQLITE_DONE &&
             sqlite3_changes(db) > 0;
    sqlite3_finalize(statement);
    if (!ok) fprintf(stderr, "no such job: %s\n", id);
    return ok;
}

static int list_runs(sqlite3* db, const char* id) {
    const char* sql_all =
        "SELECT run_id,job_id,scheduled_at,status,exit_code,log_path"
        " FROM runs ORDER BY run_id";
    const char* sql_one =
        "SELECT run_id,job_id,scheduled_at,status,exit_code,log_path"
        " FROM runs WHERE job_id=?1 ORDER BY run_id";
    sqlite3_stmt* statement = NULL;
    if (sqlite3_prepare_v2(db, id ? sql_one : sql_all, -1,
                           &statement, NULL) != SQLITE_OK) {
        return 0;
    }
    if (id) sqlite3_bind_text(statement, 1, id, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        char iso[32];
        format_iso_utc((time_t)sqlite3_column_int64(statement, 2),
                       iso, sizeof iso);
        printf("%lld | %s | %s | %s",
               sqlite3_column_int64(statement, 0),
               sqlite3_column_text(statement, 1), iso,
               sqlite3_column_text(statement, 3));
        if (sqlite3_column_type(statement, 4) != SQLITE_NULL) {
            printf(" | exit=%d", sqlite3_column_int(statement, 4));
        }
        if (sqlite3_column_type(statement, 5) != SQLITE_NULL) {
            printf(" | %s", sqlite3_column_text(statement, 5));
        }
        printf("\n");
    }
    sqlite3_finalize(statement);
    return 1;
}

static int update_finished_run(sqlite3* db, pid_t pid, int status) {
    const char* sql =
        "UPDATE runs SET status=?1,exit_code=?2,finished_at=?3"
        " WHERE pid=?4 AND status='running'";
    sqlite3_stmt* statement = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) {
        return 0;
    }
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                                     : 128 + WTERMSIG(status);
    sqlite3_bind_text(statement, 1, exit_code == 0 ? "succeeded" : "failed",
                      -1, SQLITE_STATIC);
    sqlite3_bind_int(statement, 2, exit_code);
    sqlite3_bind_int64(statement, 3, (sqlite3_int64)time(NULL));
    sqlite3_bind_int64(statement, 4, (sqlite3_int64)pid);
    int ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return ok;
}

static void reap_children(sqlite3* db, int blocking) {
    int status;
    pid_t pid;
    int options = blocking ? 0 : WNOHANG;
    while ((pid = waitpid(-1, &status, options)) > 0) {
        update_finished_run(db, pid, status);
        options = blocking ? 0 : WNOHANG;
    }
    child_changed = 0;
}

static void reconcile_running(sqlite3* db) {
    sqlite3_stmt* query = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT DISTINCT pid FROM runs"
            " WHERE status='running' AND pid IS NOT NULL",
            -1, &query, NULL) != SQLITE_OK) {
        return;
    }
    while (sqlite3_step(query) == SQLITE_ROW) {
        pid_t pid = (pid_t)sqlite3_column_int64(query, 0);
        if (kill(pid, 0) < 0 && errno == ESRCH) {
            sqlite3_stmt* update = NULL;
            if (sqlite3_prepare_v2(db,
                    "UPDATE runs SET status='lost',finished_at=?1"
                    " WHERE pid=?2 AND status='running'",
                    -1, &update, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(update, 1, (sqlite3_int64)time(NULL));
                sqlite3_bind_int64(update, 2, (sqlite3_int64)pid);
                sqlite3_step(update);
            }
            sqlite3_finalize(update);
        }
    }
    sqlite3_finalize(query);
}

static int active_runs(sqlite3* db, const char* job_id) {
    sqlite3_stmt* statement = NULL;
    int count = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM runs"
            " WHERE job_id=?1 AND status='running'",
            -1, &statement, NULL) == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, job_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(statement) == SQLITE_ROW) {
            count = sqlite3_column_int(statement, 0);
        }
    }
    sqlite3_finalize(statement);
    return count;
}

static int record_skipped(sqlite3* db, const char* job_id,
                          time_t scheduled) {
    const char* sql =
        "INSERT INTO runs(job_id,scheduled_at,finished_at,status)"
        " VALUES(?1,?2,?3,'overlap_skipped')";
    sqlite3_stmt* statement = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(statement, 1, job_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)scheduled);
    sqlite3_bind_int64(statement, 3, (sqlite3_int64)time(NULL));
    int ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return ok;
}

static int launch_job(sqlite3* db, const struct job* job, time_t scheduled) {
    if (mkdir("logs", 0755) < 0 && errno != EEXIST) {
        perror("mkdir logs");
        return 0;
    }

    char log_path[512];
    snprintf(log_path, sizeof log_path, "logs/%s-%lld.log",
             job->id, (long long)scheduled);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 0;
    }
    if (pid == 0) {
        int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd < 0) _exit(126);
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
        execl("/bin/sh", "sh", "-c", job->command, (char*)NULL);
        _exit(127);
    }

    const char* sql =
        "INSERT INTO runs"
        " (job_id,scheduled_at,started_at,pid,status,log_path)"
        " VALUES(?1,?2,?3,?4,'running',?5)";
    sqlite3_stmt* statement = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) {
        kill(pid, SIGTERM);
        return 0;
    }
    sqlite3_bind_text(statement, 1, job->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)scheduled);
    sqlite3_bind_int64(statement, 3, (sqlite3_int64)time(NULL));
    sqlite3_bind_int64(statement, 4, (sqlite3_int64)pid);
    sqlite3_bind_text(statement, 5, log_path, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!ok) kill(pid, SIGTERM);
    return ok;
}

static int append_occurrence(time_t** values, size_t* count,
                             size_t* capacity, time_t value) {
    if (*count == *capacity) {
        size_t new_capacity = *capacity ? *capacity * 2 : 16;
        time_t* resized = realloc(*values, new_capacity * sizeof **values);
        if (!resized) return 0;
        *values = resized;
        *capacity = new_capacity;
    }
    (*values)[(*count)++] = value;
    return 1;
}

static int update_last_processed(sqlite3* db, const char* id, time_t value) {
    sqlite3_stmt* statement = NULL;
    if (sqlite3_prepare_v2(db,
            "UPDATE jobs SET last_processed=?1 WHERE id=?2",
            -1, &statement, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)value);
    sqlite3_bind_text(statement, 2, id, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return ok;
}

static int process_due_jobs(sqlite3* db, time_t now) {
    struct job* jobs = NULL;
    size_t job_count = 0;
    if (!execute_sql(db, "BEGIN IMMEDIATE")) return 0;
    if (!load_jobs(db, &jobs, &job_count)) {
        execute_sql(db, "ROLLBACK");
        return 0;
    }

    int ok = 1;
    for (size_t i = 0; i < job_count && ok; i++) {
        struct job* job = &jobs[i];
        time_t cursor = job->last_processed;
        time_t occurrence;
        time_t* occurrences = NULL;
        size_t count = 0, capacity = 0;

        while ((occurrence = cron_next(&job->schedule, cursor)) != (time_t)-1 &&
               occurrence <= now) {
            if (!append_occurrence(&occurrences, &count, &capacity,
                                   occurrence)) {
                ok = 0;
                break;
            }
            cursor = occurrence;
        }
        if (!ok || count == 0) {
            free(occurrences);
            continue;
        }

        size_t first = 0;
        if (job->policy == POLICY_SKIP) {
            first = count - 1;
        } else if (job->policy == POLICY_LIMITED &&
                   count > job->catchup_limit) {
            first = count - job->catchup_limit;
        }

        int active = active_runs(db, job->id);
        for (size_t j = first; j < count && ok; j++) {
            char iso[32];
            format_iso_utc(occurrences[j], iso, sizeof iso);
            if (active >= job->max_overlap) {
                ok = record_skipped(db, job->id, occurrences[j]);
                if (ok) {
                    printf("SKIP %s for %s\n", job->id, iso);
                    fflush(stdout);
                }
            } else {
                ok = launch_job(db, job, occurrences[j]);
                if (ok) {
                    active++;
                    printf("RAN %s for %s\n", job->id, iso);
                    fflush(stdout);
                }
            }
        }
        if (ok) {
            ok = update_last_processed(db, job->id, occurrences[count - 1]);
        }
        free(occurrences);
    }

    if (ok) ok = execute_sql(db, "COMMIT");
    else execute_sql(db, "ROLLBACK");
    free_jobs(jobs, job_count);
    return ok;
}

static unsigned int next_sleep_seconds(sqlite3* db, time_t now) {
    struct job* jobs = NULL;
    size_t count = 0;
    if (!load_jobs(db, &jobs, &count)) return 1;
    time_t earliest = now + 60;
    for (size_t i = 0; i < count; i++) {
        time_t next = cron_next(&jobs[i].schedule, now);
        if (next != (time_t)-1 && next < earliest) earliest = next;
    }
    free_jobs(jobs, count);
    time_t difference = earliest - time(NULL);
    if (difference < 1) return 1;
    if (difference > 60) return 60;
    return (unsigned int)difference;
}

static int run_scheduler(sqlite3* db, int once) {
    reconcile_running(db);
    reap_children(db, 0);
    if (!process_due_jobs(db, time(NULL))) return 0;
    if (once) {
        reap_children(db, 1);
        return 1;
    }

    while (!stop_requested) {
        unsigned int delay = next_sleep_seconds(db, time(NULL));
        struct timespec sleep_time = {(time_t)delay, 0};
        while (!stop_requested && !reload_requested && !child_changed &&
               nanosleep(&sleep_time, &sleep_time) < 0 && errno == EINTR) {
        }
        reload_requested = 0;
        reap_children(db, 0);
        reconcile_running(db);
        if (!stop_requested && !process_due_jobs(db, time(NULL))) return 0;
    }

    fprintf(stderr, "draining active jobs...\n");
    reap_children(db, 1);
    return 1;
}

static void install_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGCHLD, &action, NULL);
}

static void print_usage(const char* program) {
    fprintf(stderr,
        "usage: %s [--db PATH] COMMAND [ARGS]\n"
        "commands:\n"
        "  daemon\n"
        "  run-once\n"
        "  add ID CRON POLICY MAX_OVERLAP COMMAND...\n"
        "  list\n"
        "  remove ID\n"
        "  runs [ID]\n",
        program);
}

int main(int argc, char** argv) {
    const char* database_path = "scheduler.db";
    int index = 1;
    if (index < argc && strcmp(argv[index], "--db") == 0) {
        if (++index >= argc) {
            print_usage(argv[0]);
            return 2;
        }
        database_path = argv[index++];
    }
    if (index >= argc) {
        print_usage(argv[0]);
        return 2;
    }

    sqlite3* db = open_database(database_path);
    if (!db) return 1;
    const char* command = argv[index++];
    int ok;

    if (strcmp(command, "add") == 0) {
        ok = add_job(db, argc, argv, index);
    } else if (strcmp(command, "list") == 0 && index == argc) {
        ok = list_jobs(db);
    } else if (strcmp(command, "remove") == 0 && index + 1 == argc) {
        ok = remove_job(db, argv[index]);
    } else if (strcmp(command, "runs") == 0 && index + 1 >= argc) {
        ok = list_runs(db, index < argc ? argv[index] : NULL);
    } else if ((strcmp(command, "daemon") == 0 ||
                strcmp(command, "run-once") == 0) && index == argc) {
        install_signal_handlers();
        ok = run_scheduler(db, strcmp(command, "run-once") == 0);
    } else {
        print_usage(argv[0]);
        ok = 0;
    }

    sqlite3_close(db);
    return ok ? 0 : 1;
}
