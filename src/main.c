#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_FIELDS 8
#define MAX_WORKERS 64

typedef struct {
    const char *ptr;
    uint32_t len;
} Slice;

extern int csv_parse_line_asm(
    const char *record,
    size_t record_len,
    uint32_t expected_fields,
    Slice *out
);

typedef struct {
    char path[PATH_MAX];
    const char *mapping;
    size_t size;
    uint32_t expected_fields;
} InputFile;

typedef struct {
    uint32_t file_index;
    uint32_t expected_fields;
    size_t begin;
    size_t end;
} ParseTask;

/* Exactly 64 bytes: one cache line per worker. */
typedef struct {
    uint64_t worker_start_ns;
    uint64_t first_parse_ns;
    uint64_t last_parse_ns;
    uint64_t worker_finish_ns;
    uint64_t valid_rows;
    uint64_t malformed_rows;
    uint64_t tasks_completed;
    unsigned char padding[8];
} WorkerStats;

typedef struct {
    _Atomic uint32_t next_task;
    uint32_t worker_count;
    unsigned char padding[56];
    WorkerStats workers[];
} SharedState;

typedef struct {
    InputFile *files;
    size_t file_count;
    size_t file_capacity;

    ParseTask *tasks;
    size_t task_count;
    size_t task_capacity;
} Workload;

/* ---------------------------------------------------------
   Timing
   --------------------------------------------------------- */

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        perror("clock_gettime");
        _exit(2);
    }

    return (uint64_t)ts.tv_sec * 1000000000ULL
         + (uint64_t)ts.tv_nsec;
}

static double milliseconds(uint64_t start, uint64_t end)
{
    return (double)(end - start) / 1000000.0;
}

/* ---------------------------------------------------------
   CSV file utilities
   --------------------------------------------------------- */

static size_t find_newline(
    const char *data,
    size_t from,
    size_t limit
)
{
    size_t p = from;

    while (p < limit && data[p] != '\n')
        ++p;

    return p;
}

static int has_csv_suffix(const char *name)
{
    size_t n = strlen(name);

    return n >= 4 &&
           memcmp(name + n - 4, ".csv", 4) == 0;
}

/* ---------------------------------------------------------
   Dynamic arrays
   --------------------------------------------------------- */

static int reserve_files(Workload *w, size_t needed)
{
    if (needed <= w->file_capacity)
        return 0;

    size_t cap = w->file_capacity ? w->file_capacity * 2 : 16;

    while (cap < needed)
        cap *= 2;

    InputFile *p = realloc(w->files, cap * sizeof(*p));

    if (!p)
        return -1;

    w->files = p;
    w->file_capacity = cap;

    return 0;
}

static int reserve_tasks(Workload *w, size_t needed)
{
    if (needed <= w->task_capacity)
        return 0;

    size_t cap = w->task_capacity ? w->task_capacity * 2 : 64;

    while (cap < needed)
        cap *= 2;

    ParseTask *p = realloc(w->tasks, cap * sizeof(*p));

    if (!p)
        return -1;

    w->tasks = p;
    w->task_capacity = cap;

    return 0;
}

static int add_task(
    Workload *w,
    uint32_t file_index,
    size_t begin,
    size_t end,
    uint32_t fields
)
{
    if (begin >= end)
        return 0;

    if (reserve_tasks(w, w->task_count + 1) != 0)
        return -1;

    w->tasks[w->task_count++] = (ParseTask) {
        .file_index = file_index,
        .expected_fields = fields,
        .begin = begin,
        .end = end
    };

    return 0;
}

/* ---------------------------------------------------------
   Map CSV and divide it into tasks
   --------------------------------------------------------- */

static int add_csv_file(
    Workload *w,
    const char *path,
    uint32_t expected_fields,
    uint32_t workers
)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    struct stat st;

    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        fprintf(stderr, "Invalid or empty CSV: %s\n", path);
        close(fd);
        return -1;
    }

    size_t size = (size_t)st.st_size;

    const char *mapping = mmap(
        NULL,
        size,
        PROT_READ,
        MAP_PRIVATE,
        fd,
        0
    );

    int map_errno = errno;
    close(fd);

    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap %s: %s\n",
                path, strerror(map_errno));
        return -1;
    }

    /* Validate header syntax and field count. */
    size_t header_end = find_newline(mapping, 0, size);

    Slice header[MAX_FIELDS];

    if (!csv_parse_line_asm(
            mapping,
            header_end,
            expected_fields,
            header
        )) {
        fprintf(stderr, "Malformed header: %s\n", path);
        munmap((void *)mapping, size);
        return -1;
    }

    if (strlen(path) >= PATH_MAX ||
        reserve_files(w, w->file_count + 1) != 0) {
        munmap((void *)mapping, size);
        return -1;
    }

    uint32_t file_index = (uint32_t)w->file_count;

    InputFile *file = &w->files[w->file_count++];

    strcpy(file->path, path);

    file->mapping = mapping;
    file->size = size;
    file->expected_fields = expected_fields;

    size_t data_begin =
        header_end < size ? header_end + 1 : size;

    size_t data_size = size - data_begin;

    /* Header-only files are valid. */
    if (data_size == 0)
        return 0;

    /*
     * Four tasks per worker per file.
     * This gives the scheduler flexibility when files differ in size.
     */
    uint32_t chunks = workers * 4;

    if (chunks < 1)
        chunks = 1;

    if ((size_t)chunks > data_size)
        chunks = (uint32_t)data_size;

    size_t previous = data_begin;

    for (uint32_t i = 1; i <= chunks; ++i) {
        size_t boundary;

        if (i == chunks) {
            boundary = size;
        } else {
            size_t target =
                data_begin
                + (data_size / chunks) * i
                + ((data_size % chunks) * i) / chunks;

            size_t nl = find_newline(mapping, target, size);

            boundary = nl < size ? nl + 1 : size;

            if (boundary < previous)
                boundary = previous;
        }

        if (add_task(
                w,
                file_index,
                previous,
                boundary,
                expected_fields
            ) != 0) {
            return -1;
        }

        previous = boundary;
    }

    return 0;
}

/* ---------------------------------------------------------
   Discover input files
   --------------------------------------------------------- */

static int discover_directory(
    Workload *w,
    const char *root,
    const char *subdir,
    uint32_t fields,
    uint32_t workers
)
{
    char directory[PATH_MAX];

    int n = snprintf(
        directory,
        sizeof(directory),
        "%s/%s",
        root,
        subdir
    );

    if (n < 0 || (size_t)n >= sizeof(directory))
        return -1;

    DIR *dir = opendir(directory);

    if (!dir) {
        fprintf(stderr, "opendir %s: %s\n",
                directory, strerror(errno));
        return -1;
    }

    struct dirent *entry;
    int result = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' ||
            !has_csv_suffix(entry->d_name))
            continue;

        char path[PATH_MAX];

        n = snprintf(
            path,
            sizeof(path),
            "%s/%s",
            directory,
            entry->d_name
        );

        if (n < 0 || (size_t)n >= sizeof(path)) {
            result = -1;
            break;
        }

        struct stat st;

        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        if (add_csv_file(w, path, fields, workers) != 0) {
            result = -1;
            break;
        }
    }

    closedir(dir);

    return result;
}

static int discover_root_file(
    Workload *w,
    const char *root,
    const char *name,
    uint32_t fields,
    uint32_t workers
)
{
    char path[PATH_MAX];

    int n = snprintf(
        path,
        sizeof(path),
        "%s/%s",
        root,
        name
    );

    if (n < 0 || (size_t)n >= sizeof(path))
        return -1;

    return add_csv_file(w, path, fields, workers);
}

/* ---------------------------------------------------------
   Worker
   --------------------------------------------------------- */

static void worker_loop(
    uint32_t worker_id,
    SharedState *shared,
    const Workload *workload
)
{
    WorkerStats *stats = &shared->workers[worker_id];

    stats->worker_start_ns = monotonic_ns();

    for (;;) {
        uint32_t task_id = atomic_fetch_add_explicit(
            &shared->next_task,
            1,
            memory_order_relaxed
        );

        if ((size_t)task_id >= workload->task_count)
            break;

        const ParseTask *task = &workload->tasks[task_id];

        const InputFile *file =
            &workload->files[task->file_index];

        size_t cursor = task->begin;

        Slice fields[MAX_FIELDS];

        int saw_record = 0;

        while (cursor < task->end) {
            size_t line_end = find_newline(
                file->mapping,
                cursor,
                task->end
            );

            size_t line_len = line_end - cursor;

            /*
             * One timing call before the worker's first
             * parser invocation, not on every record.
             */
            if (!stats->first_parse_ns)
                stats->first_parse_ns = monotonic_ns();

            if (csv_parse_line_asm(
                    file->mapping + cursor,
                    line_len,
                    task->expected_fields,
                    fields
                )) {
                ++stats->valid_rows;
            } else {
                ++stats->malformed_rows;
            }

            saw_record = 1;

            cursor = line_end;

            if (cursor < task->end &&
                file->mapping[cursor] == '\n') {
                ++cursor;
            }
        }

        if (saw_record)
            stats->last_parse_ns = monotonic_ns();

        ++stats->tasks_completed;
    }

    stats->worker_finish_ns = monotonic_ns();
}

/* ---------------------------------------------------------
   Cleanup and configuration
   --------------------------------------------------------- */

static void release_workload(Workload *w)
{
    for (size_t i = 0; i < w->file_count; ++i) {
        munmap(
            (void *)w->files[i].mapping,
            w->files[i].size
        );
    }

    free(w->files);
    free(w->tasks);
}

static uint32_t get_worker_count(void)
{
    const char *env = getenv("NCPU");

    long n = env
        ? strtol(env, NULL, 10)
        : sysconf(_SC_NPROCESSORS_ONLN);

    if (n < 1)
        n = 1;

    if (n > MAX_WORKERS)
        n = MAX_WORKERS;

    return (uint32_t)n;
}

/* ---------------------------------------------------------
   Main
   --------------------------------------------------------- */

int main(void)
{
    const uint64_t main_start_ns = monotonic_ns();

    const char *data_dir = getenv("DATA_DIR");

    if (!data_dir || !*data_dir)
        data_dir = "/data";

    uint32_t workers = get_worker_count();

    Workload workload = {0};

    /*
     * Challenge input schemas:
     *
     * events:    7 fields
     * clicks:    4 fields
     * links:     2 fields
     * products:  2 fields
     * campaigns: 2 fields
     */

    if (discover_directory(
            &workload, data_dir, "events", 7, workers
        ) != 0 ||
        discover_directory(
            &workload, data_dir, "clicks", 4, workers
        ) != 0 ||
        discover_directory(
            &workload, data_dir, "links", 2, workers
        ) != 0 ||
        discover_root_file(
            &workload, data_dir, "products.csv", 2, workers
        ) != 0 ||
        discover_root_file(
            &workload, data_dir, "campaigns.csv", 2, workers
        ) != 0) {

        release_workload(&workload);
        return 1;
    }

    const uint64_t discovery_done_ns = monotonic_ns();

    if (workload.task_count == 0) {
        fprintf(stderr, "No data records found.\n");
        release_workload(&workload);
        return 1;
    }

    size_t shared_size =
        sizeof(SharedState) + workers * sizeof(WorkerStats);

    SharedState *shared = mmap(
        NULL,
        shared_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS,
        -1,
        0
    );

    if (shared == MAP_FAILED) {
        perror("mmap shared state");
        release_workload(&workload);
        return 1;
    }

    memset(shared, 0, shared_size);

    atomic_init(&shared->next_task, 0);

    shared->worker_count = workers;

    pid_t pids[MAX_WORKERS];

    uint32_t child_count = 0;

    /*
     * Fork N-1 children.
     * The parent itself acts as worker 0.
     */
    for (uint32_t i = 1; i < workers; ++i) {
        pid_t pid = fork();

        if (pid == 0) {
            worker_loop(i, shared, &workload);
            _exit(0);
        }

        if (pid < 0) {
            fprintf(
                stderr,
                "fork worker %u failed: %s\n",
                i,
                strerror(errno)
            );
            break;
        }

        pids[child_count++] = pid;
    }

    worker_loop(0, shared, &workload);

    int failed = 0;

    for (uint32_t i = 0; i < child_count; ++i) {
        int status = 0;
        pid_t waited;

        do {
            waited = waitpid(pids[i], &status, 0);
        } while (waited < 0 && errno == EINTR);

        if (waited < 0 ||
            !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
            failed = 1;
        }
    }

    const uint64_t parse_complete_ns = monotonic_ns();

    uint64_t first_parse_ns = 0;
    uint64_t last_parse_ns = 0;
    uint64_t first_worker_ns = 0;

    uint64_t valid_rows = 0;
    uint64_t malformed_rows = 0;
    uint64_t total_tasks = 0;

    for (uint32_t i = 0; i <= child_count; ++i) {
        WorkerStats *s = &shared->workers[i];

        if (s->worker_start_ns &&
            (!first_worker_ns ||
             s->worker_start_ns < first_worker_ns)) {
            first_worker_ns = s->worker_start_ns;
        }

        if (s->first_parse_ns &&
            (!first_parse_ns ||
             s->first_parse_ns < first_parse_ns)) {
            first_parse_ns = s->first_parse_ns;
        }

        if (s->last_parse_ns > last_parse_ns)
            last_parse_ns = s->last_parse_ns;

        valid_rows += s->valid_rows;
        malformed_rows += s->malformed_rows;
        total_tasks += s->tasks_completed;
    }

    printf("=== CSV PARSER BENCHMARK ===\n");

    printf("Input directory:          %s\n", data_dir);
    printf("Workers requested:        %u\n", workers);
    printf("Workers started:          %u\n", child_count + 1);
    printf("Files mapped:             %zu\n", workload.file_count);
    printf("Parse tasks:              %zu\n", workload.task_count);
    printf("Tasks completed:          %llu\n",
           (unsigned long long)total_tasks);

    printf("Valid rows:               %llu\n",
           (unsigned long long)valid_rows);

    printf("Malformed rows:           %llu\n",
           (unsigned long long)malformed_rows);

    printf("\n=== WALL-CLOCK PHASES ===\n");

    printf("Main entry -> discovery:  %.3f ms\n",
           milliseconds(main_start_ns, discovery_done_ns));

    if (first_worker_ns) {
        printf("First worker start:       %.3f ms after main entry\n",
               milliseconds(main_start_ns, first_worker_ns));
    }

    if (first_parse_ns) {
        printf("First record parse:       %.3f ms after main entry\n",
               milliseconds(main_start_ns, first_parse_ns));
    }

    if (first_parse_ns && last_parse_ns) {
        printf("First -> last parse:      %.3f ms\n",
               milliseconds(first_parse_ns, last_parse_ns));
    }

    printf("Main entry -> all done:   %.3f ms\n",
           milliseconds(main_start_ns, parse_complete_ns));

    printf("\n");
    printf("Parser benchmark only: no deduplication, aggregation, or Parquet output yet.\n");

    munmap(shared, shared_size);

    release_workload(&workload);

    return failed ? 1 : 0;
}
