/*
 * External sort and grouped k-way merging benchmark (C11).
 *
 * Build:
 *    GCC:   gcc -std=c11 -Wall -Wextra -Wpedantic -O2 stress_test_week4_fixed.c -o external_sort
 *    Clang: clang -std=c11 -Wall -Wextra -Wpedantic -O2 stress_test_week4_fixed.c -o external_sort
 *    MSVC (not yet verified): cl /std:c11 /W4 /O2 stress_test_week4_fixed.c /Fe:external_sort.exe
 *
 * Usage:
 *    ./external_sort                      Run benchmark table (N = 5k, 20k, 100k, 1,000k)
 *    ./external_sort --benchmark          Run benchmark table (N = 5k, 20k, 100k, 1,000k)
 *    ./external_sort --test               Run built-in correctness tests
 *    ./external_sort --stress             Run heavy failure path & stress tests
 *    ./external_sort INPUT OUTPUT [M [K]] Sort integers from INPUT into OUTPUT
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32) && defined(__unix__)
#include <signal.h>
#include <sys/resource.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define DEFAULT_M 1000
#define DEFAULT_K 2
#define TOKEN_BUFFER_SIZE 128

/* Logical text bytes consumed/emitted by the program, not device-level physical I/O.
 * Text-mode newline translation may make on-disk byte counts platform-dependent.
 */
typedef struct {
    unsigned long long bytes_read;
    unsigned long long bytes_written;
    size_t initial_runs;
    size_t merge_passes;
    size_t total_passes;
} IOStats;

typedef struct {
    int value;
    size_t run_id;
} HeapNode;

typedef struct {
    HeapNode *items;
    size_t size;
    size_t capacity;
} MinHeap;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} RunList;

static unsigned long long g_temp_counter = 0;

/*
 * Create a unique temporary file in the same directory as base_path.
 * fopen(..., "wx") uses C11 exclusive creation: an existing file is never
 * truncated. If another process wins a name race, retry with a new suffix.
 * Keeping the file beside the destination also keeps final rename/replace on
 * the same filesystem, which is required for atomic replacement on POSIX.
 */
static char *create_temp_file_near(const char *base_path,
                                   size_t pass,
                                   size_t index,
                                   FILE **file_out) {
    const char *last_sep;
    size_t directory_length;
    const char *separator;
    size_t attempt;

    if (base_path == NULL || file_out == NULL) return NULL;
    *file_out = NULL;

    last_sep = strrchr(base_path, '/');
#ifdef _WIN32
    {
        const char *backslash = strrchr(base_path, '\\');
        if (backslash != NULL && (last_sep == NULL || backslash > last_sep))
            last_sep = backslash;
    }
#endif
    if (last_sep != NULL) {
        directory_length = (size_t)(last_sep - base_path) + 1;
        if (directory_length > (size_t)INT_MAX) return NULL;
        separator = "";
    } else {
        directory_length = 0;
#ifdef _WIN32
        separator = ".\\";
#else
        separator = "./";
#endif
    }

    for (attempt = 0; attempt < 10000; ++attempt) {
        struct timespec now;
        unsigned long long serial = ++g_temp_counter;
        unsigned long long seconds;
        unsigned long long nanoseconds;
        int needed;
        char *path;
        FILE *stream;

        if (timespec_get(&now, TIME_UTC) == TIME_UTC) {
            seconds = (unsigned long long)now.tv_sec;
            nanoseconds = (unsigned long long)now.tv_nsec;
        } else {
            seconds = (unsigned long long)time(NULL);
            nanoseconds = 0;
        }

        needed = snprintf(NULL, 0,
                          "%.*s%s.extsort_%llu_%llu_%llu_%zu_%zu.tmp",
                          (int)directory_length, base_path, separator,
                          seconds, nanoseconds, serial, pass, index);
        if (needed < 0) return NULL;
        path = (char *)malloc((size_t)needed + 1);
        if (path == NULL) return NULL;
        if (snprintf(path, (size_t)needed + 1,
                     "%.*s%s.extsort_%llu_%llu_%llu_%zu_%zu.tmp",
                     (int)directory_length, base_path, separator,
                     seconds, nanoseconds, serial, pass, index) != needed) {
            free(path);
            return NULL;
        }

        stream = fopen(path, "wx");
        if (stream != NULL) {
            *file_out = stream;
            return path;
        }
        {
            int saved_errno = errno;
            free(path);
            if (saved_errno != EEXIST) {
                errno = saved_errno;
                return NULL;
            }
        }
    }
    errno = EEXIST;
    return NULL;
}

static bool run_list_push_owned(RunList *list, char *path) {
    char **new_items;
    size_t new_capacity;

    if (list == NULL || path == NULL) return false;
    if (list->count == list->capacity) {
        new_capacity = (list->capacity == 0) ? 8 : list->capacity * 2;
        if (new_capacity < list->capacity ||
            new_capacity > ((size_t)-1) / sizeof(*list->items))
            return false;
        new_items = (char **)realloc(list->items, new_capacity * sizeof(*list->items));
        if (new_items == NULL) return false;
        list->items = new_items;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = path;
    return true;
}

static bool remove_owned_file(const char *path, const char *purpose) {
    if (path == NULL) return true;
    if (remove(path) == 0 || errno == ENOENT) return true;
    fprintf(stderr, "Warning: cannot remove %s '%s': %s\n",
            purpose != NULL ? purpose : "temporary file", path, strerror(errno));
    return false;
}

/* Replace destination only after source is complete. Both paths are created
 * in the same directory, so this operation is atomic on POSIX filesystems. */
static bool replace_file(const char *source, const char *destination) {
#ifdef _WIN32
    if (MoveFileExA(source, destination,
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
        return true;
    fprintf(stderr, "Error: cannot replace output file '%s' (Windows error %lu).\n",
            destination, (unsigned long)GetLastError());
    return false;
#else
    if (rename(source, destination) == 0) return true;
    fprintf(stderr, "Error: cannot replace output file '%s': %s\n",
            destination, strerror(errno));
    return false;
#endif
}

static void run_list_destroy(RunList *list, bool remove_files) {
    size_t i;
    if (list == NULL) return;
    for (i = 0; i < list->count; ++i) {
        if (list->items[i] != NULL) {
            if (remove_files)
                (void)remove_owned_file(list->items[i], "temporary run");
            free(list->items[i]);
        }
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int read_next_int(FILE *stream, int *value, IOStats *stats) {
    int c;
    char token[TOKEN_BUFFER_SIZE];
    size_t length = 0;
    bool too_long = false;
    bool contains_nul = false;
    char *end = NULL;
    long parsed;

    if (stream == NULL || value == NULL) return -1;
    if (ferror(stream)) {
        fprintf(stderr, "Error: failed while reading a data file.\n");
        return -1;
    }
    /* The previous token may already have consumed EOF; avoid probing again. */
    if (feof(stream)) return 0;

    do {
        c = fgetc(stream);
        if (c != EOF && stats != NULL) stats->bytes_read++;
    } while (c != EOF && isspace((unsigned char)c));

    if (c == EOF) {
        if (ferror(stream)) {
            fprintf(stderr, "Error: failed while reading a data file.\n");
            return -1;
        }
        return 0;
    }

    while (c != EOF && !isspace((unsigned char)c)) {
        if (c == '\0') {
            contains_nul = true;
        } else if (length + 1 < sizeof(token)) {
            token[length++] = (char)c;
        } else {
            too_long = true;
        }
        c = fgetc(stream);
        if (c != EOF && stats != NULL) stats->bytes_read++;
    }
    if (c == EOF && ferror(stream)) {
        fprintf(stderr, "Error: failed while reading a data file.\n");
        return -1;
    }
    token[length] = '\0';

    if (contains_nul) {
        fprintf(stderr, "Error: integer token contains a NUL byte.\n");
        return -1;
    }
    if (too_long) {
        fprintf(stderr, "Error: integer token is too long.\n");
        return -1;
    }
    errno = 0;
    parsed = strtol(token, &end, 10);
    if (errno == ERANGE || end == token || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        fprintf(stderr, "Error: invalid integer token '%s'.\n", token);
        return -1;
    }

    *value = (int)parsed;
    return 1;
}

static int compare_ints(const void *left, const void *right) {
    int a = *(const int *)left;
    int b = *(const int *)right;
    return (a > b) - (a < b);
}

static MinHeap *heap_create(size_t capacity) {
    MinHeap *heap;
    if (capacity == 0 || capacity > ((size_t)-1) / sizeof(HeapNode)) return NULL;
    heap = (MinHeap *)malloc(sizeof(*heap));
    if (heap == NULL) return NULL;
    heap->items = (HeapNode *)malloc(capacity * sizeof(*heap->items));
    if (heap->items == NULL) {
        free(heap);
        return NULL;
    }
    heap->size = 0;
    heap->capacity = capacity;
    return heap;
}

static bool node_less(HeapNode a, HeapNode b) {
    if (a.value != b.value) return a.value < b.value;
    return a.run_id < b.run_id;
}

static bool heap_insert(MinHeap *heap, HeapNode node) {
    size_t i;
    if (heap == NULL || heap->size >= heap->capacity) return false;
    i = heap->size++;
    heap->items[i] = node;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!node_less(heap->items[i], heap->items[parent])) break;
        {
            HeapNode temp = heap->items[i];
            heap->items[i] = heap->items[parent];
            heap->items[parent] = temp;
        }
        i = parent;
    }
    return true;
}

static void heapify_down(MinHeap *heap, size_t i) {
    for (;;) {
        size_t smallest = i;
        size_t left = 2 * i + 1;
        size_t right = left + 1;
        if (left < heap->size && node_less(heap->items[left], heap->items[smallest]))
            smallest = left;
        if (right < heap->size && node_less(heap->items[right], heap->items[smallest]))
            smallest = right;
        if (smallest == i) break;
        {
            HeapNode temp = heap->items[i];
            heap->items[i] = heap->items[smallest];
            heap->items[smallest] = temp;
        }
        i = smallest;
    }
}

static HeapNode heap_extract_min(MinHeap *heap) {
    HeapNode result = {0, 0};
    if (heap == NULL || heap->size == 0) return result;
    result = heap->items[0];
    heap->size--;
    if (heap->size > 0) {
        heap->items[0] = heap->items[heap->size];
        heapify_down(heap, 0);
    }
    return result;
}

static void heap_destroy(MinHeap *heap) {
    if (heap != NULL) {
        free(heap->items);
        free(heap);
    }
}

static bool write_int(FILE *stream, int value, IOStats *stats) {
    char line[64];
    int length = snprintf(line, sizeof(line), "%d\n", value);
    if (length < 0 || (size_t)length >= sizeof(line)) return false;
    if (fputs(line, stream) == EOF) return false;
    if (stats != NULL) stats->bytes_written += (unsigned long long)length;
    return true;
}

static bool create_initial_runs(const char *input_file, const char *output_file,
                                size_t M, RunList *runs, IOStats *stats) {
    FILE *input = NULL;
    int *buffer = NULL;
    bool ok = false;

    if (M == 0 || M > ((size_t)-1) / sizeof(*buffer)) {
        fprintf(stderr, "Error: M is outside the supported range.\n");
        return false;
    }
    input = fopen(input_file, "r");
    if (input == NULL) {
        fprintf(stderr, "Error: cannot open input file '%s': %s\n",
                input_file, strerror(errno));
        return false;
    }
    buffer = (int *)malloc(M * sizeof(*buffer));
    if (buffer == NULL) {
        fprintf(stderr, "Error: cannot allocate the M-sized RAM buffer.\n");
        (void)fclose(input);
        return false;
    }

    for (;;) {
        size_t count = 0;
        int status;
        while (count < M) {
            status = read_next_int(input, &buffer[count], stats);
            if (status < 0) goto cleanup;
            if (status == 0) break;
            count++;
        }
        if (count == 0) break;

        qsort(buffer, count, sizeof(*buffer), compare_ints);
        {
            FILE *run_file = NULL;
            char *run_path = create_temp_file_near(output_file, 0, runs->count,
                                                   &run_file);
            size_t i;
            bool write_ok = true;
            if (run_path == NULL || run_file == NULL) {
                fprintf(stderr, "Error: cannot create a unique temporary run: %s\n",
                        strerror(errno));
                free(run_path);
                goto cleanup;
            }
            for (i = 0; i < count; ++i) {
                if (!write_int(run_file, buffer[i], stats)) {
                    write_ok = false;
                    break;
                }
            }
            if (fclose(run_file) != 0) write_ok = false;
            if (!write_ok) {
                fprintf(stderr, "Error: failed to write a temporary run.\n");
                (void)remove_owned_file(run_path, "temporary run");
                free(run_path);
                goto cleanup;
            }
            if (!run_list_push_owned(runs, run_path)) {
                fprintf(stderr, "Error: cannot grow the temporary-run list.\n");
                (void)remove_owned_file(run_path, "temporary run");
                free(run_path);
                goto cleanup;
            }
        }
        if (count < M) break;
    }
    ok = true;

cleanup:
    free(buffer);
    if (fclose(input) != 0) {
        fprintf(stderr, "Error: failed to close input file '%s'.\n", input_file);
        ok = false;
    }
    return ok;
}

static bool merge_runs(char *const input_runs[], size_t num_runs,
                       const char *temp_base, size_t pass, size_t index,
                       IOStats *stats, char **output_run_path) {
    FILE **input_files = NULL;
    FILE *output = NULL;
    MinHeap *heap = NULL;
    char *temp_path = NULL;
    bool ok = false;
    size_t i;

    if (output_run_path == NULL || num_runs == 0) return false;
    *output_run_path = NULL;
    if (num_runs > ((size_t)-1) / sizeof(*input_files)) return false;
    input_files = (FILE **)calloc(num_runs, sizeof(*input_files));
    if (input_files == NULL) {
        fprintf(stderr, "Error: cannot allocate merge input handles.\n");
        goto cleanup;
    }
    heap = heap_create(num_runs);
    if (heap == NULL) {
        fprintf(stderr, "Error: cannot allocate the merge min-heap.\n");
        goto cleanup;
    }

    for (i = 0; i < num_runs; ++i) {
        int value;
        int status;
        input_files[i] = fopen(input_runs[i], "r");
        if (input_files[i] == NULL) {
            fprintf(stderr, "Error: cannot open temporary run '%s': %s\n",
                    input_runs[i], strerror(errno));
            goto cleanup;
        }
        status = read_next_int(input_files[i], &value, stats);
        if (status < 0) goto cleanup;
        if (status > 0 && !heap_insert(heap, (HeapNode){value, i})) {
            fprintf(stderr, "Error: merge heap capacity was exceeded.\n");
            goto cleanup;
        }
    }

    temp_path = create_temp_file_near(temp_base, pass, index, &output);
    if (temp_path == NULL || output == NULL) {
        fprintf(stderr, "Error: cannot create a unique merge output file: %s\n",
                strerror(errno));
        goto cleanup;
    }

    while (heap->size > 0) {
        HeapNode smallest = heap_extract_min(heap);
        int next_value;
        int status;
        if (!write_int(output, smallest.value, stats)) {
            fprintf(stderr, "Error: failed while writing merge temporary file '%s'.\n",
                    temp_path);
            goto cleanup;
        }
        status = read_next_int(input_files[smallest.run_id], &next_value, stats);
        if (status < 0) goto cleanup;
        if (status > 0 && !heap_insert(heap, (HeapNode){next_value, smallest.run_id})) {
            fprintf(stderr, "Error: merge heap capacity was exceeded.\n");
            goto cleanup;
        }
    }

    if (fclose(output) != 0) {
        output = NULL;
        fprintf(stderr, "Error: failed to finish merge temporary file '%s'.\n", temp_path);
        goto cleanup;
    }
    output = NULL;
    ok = true;

cleanup:
    if (output != NULL) {
        if (fclose(output) != 0) ok = false;
    }
    if (input_files != NULL) {
        for (i = 0; i < num_runs; ++i) {
            if (input_files[i] != NULL && fclose(input_files[i]) != 0) {
                fprintf(stderr, "Error: failed to close temporary input run '%s'.\n",
                        input_runs[i]);
                ok = false;
            }
        }
    }
    if (!ok && temp_path != NULL)
        (void)remove_owned_file(temp_path, "incomplete merge file");
    free(input_files);
    heap_destroy(heap);

    if (ok) *output_run_path = temp_path;
    else free(temp_path);
    return ok;
}

static bool create_empty_output(const char *output_file) {
    FILE *output = NULL;
    char *temp_path = create_temp_file_near(output_file, 0, 0, &output);
    bool ok;
    if (temp_path == NULL || output == NULL) {
        fprintf(stderr, "Error: cannot create temporary output beside '%s': %s\n",
                output_file, strerror(errno));
        free(temp_path);
        return false;
    }
    ok = fclose(output) == 0;
    if (!ok) fprintf(stderr, "Error: failed to finish temporary empty output.\n");
    if (ok) ok = replace_file(temp_path, output_file);
    if (!ok) (void)remove_owned_file(temp_path, "temporary output");
    free(temp_path);
    return ok;
}

static bool move_single_run_to_output(const char *run_path, const char *output_file) {
    /* rename/MoveFileEx replaces only after the completed run already exists. */
    return replace_file(run_path, output_file);
}

static int external_sort(const char *input_file, const char *output_file,
                          int M, int K, IOStats *stats) {
    RunList current = {0};

    if (stats == NULL || input_file == NULL || output_file == NULL ||
        M <= 0 || K < 2) {
        fprintf(stderr, "Error: input/output paths must be supplied, M must be > 0, and K must be >= 2.\n");
        return -1;
    }
    memset(stats, 0, sizeof(*stats));

    if (!create_initial_runs(input_file, output_file, (size_t)M, &current, stats)) {
        run_list_destroy(&current, true);
        return -1;
    }
    stats->initial_runs = current.count;
    stats->total_passes = 1;

    if (current.count == 0) {
        bool ok = create_empty_output(output_file);
        run_list_destroy(&current, false);
        return ok ? 0 : -1;
    }
    if (current.count == 1) {
        bool ok = move_single_run_to_output(current.items[0], output_file);
        if (ok) {
            free(current.items[0]);
            current.items[0] = NULL;
        }
        run_list_destroy(&current, !ok);
        return ok ? 0 : -1;
    }

    while (current.count > 1) {
        RunList next = {0};
        size_t i;
        bool final_pass = current.count <= (size_t)K;

        if (final_pass) {
            char *final_path = NULL;
            bool merged = merge_runs(current.items, current.count, output_file,
                                     stats->merge_passes + 1, 0, stats, &final_path);
            if (merged) merged = replace_file(final_path, output_file);
            if (!merged && final_path != NULL)
                (void)remove_owned_file(final_path, "final temporary output");
            free(final_path);
            run_list_destroy(&current, true);
            if (!merged) return -1;
            stats->merge_passes++;
            stats->total_passes++;
            return 0;
        }

        for (i = 0; i < current.count; i += (size_t)K) {
            size_t group_count = current.count - i;
            if (group_count > (size_t)K) group_count = (size_t)K;

            if (group_count == 1) {
                char *path = current.items[i];
                if (!run_list_push_owned(&next, path)) {
                    fprintf(stderr, "Error: cannot grow the next-run list.\n");
                    run_list_destroy(&next, true);
                    run_list_destroy(&current, true);
                    return -1;
                }
                current.items[i] = NULL;
            } else {
                char *new_path = NULL;
                if (!merge_runs(&current.items[i], group_count, output_file,
                                stats->merge_passes + 1, next.count,
                                stats, &new_path)) {
                    run_list_destroy(&next, true);
                    run_list_destroy(&current, true);
                    return -1;
                }
                if (!run_list_push_owned(&next, new_path)) {
                    fprintf(stderr, "Error: cannot grow the next-run list.\n");
                    (void)remove_owned_file(new_path, "new merge run");
                    free(new_path);
                    run_list_destroy(&next, true);
                    run_list_destroy(&current, true);
                    return -1;
                }
                {
                    size_t j;
                    for (j = 0; j < group_count; ++j) {
                        (void)remove_owned_file(current.items[i + j], "consumed input run");
                        free(current.items[i + j]);
                        current.items[i + j] = NULL;
                    }
                }
            }
        }

        run_list_destroy(&current, true);
        current = next;
        stats->merge_passes++;
        stats->total_passes++;
    }

    if (current.count == 1) {
        bool ok = move_single_run_to_output(current.items[0], output_file);
        if (ok) {
            free(current.items[0]);
            current.items[0] = NULL;
        }
        run_list_destroy(&current, !ok);
        return ok ? 0 : -1;
    }
    run_list_destroy(&current, true);
    return create_empty_output(output_file) ? 0 : -1;
}

static bool generate_random_file(FILE *file, int count) {
    int i;
    if (file == NULL) return false;
    srand(42);
    for (i = 0; i < count; ++i) {
        if (fprintf(file, "%d\n", rand() % 1000000) < 0) {
            fprintf(stderr, "Error: failed while generating benchmark input.\n");
            return false;
        }
    }
    return true;
}

static size_t theoretical_total_passes(size_t initial_runs, size_t K) {
    size_t levels = 0;
    if (initial_runs == 0) return 1;
    while (initial_runs > 1) {
        initial_runs = initial_runs / K + (initial_runs % K != 0);
        levels++;
    }
    return 1 + levels;
}

/* Benchmark files are uniquely created and owned by this invocation. */
static int run_benchmark(void) {
    const int M = DEFAULT_M;
    const int file_sizes[] = {5000, 20000, 100000, 1000000};
    const int k_values[] = {2, 3, 5, 10};
    char *input_file = NULL;
    char *output_file = NULL;
    FILE *input_stream = NULL;
    FILE *output_reservation = NULL;
    size_t s;
    int exit_code = 1;

    input_file = create_temp_file_near("benchmark_input", 0, 0, &input_stream);
    if (input_file == NULL || input_stream == NULL) {
        fprintf(stderr, "Error: cannot create unique benchmark input: %s\n", strerror(errno));
        free(input_file);
        return 1;
    }
    if (fclose(input_stream) != 0) {
        fprintf(stderr, "Error: cannot close benchmark input reservation.\n");
        (void)remove_owned_file(input_file, "benchmark input");
        free(input_file);
        return 1;
    }
    input_stream = NULL;

    output_file = create_temp_file_near("benchmark_output", 0, 0, &output_reservation);
    if (output_file == NULL || output_reservation == NULL) {
        fprintf(stderr, "Error: cannot reserve unique benchmark output: %s\n", strerror(errno));
        (void)remove_owned_file(input_file, "benchmark input");
        free(input_file);
        free(output_file);
        return 1;
    }
    if (fclose(output_reservation) != 0) {
        fprintf(stderr, "Error: cannot close benchmark output reservation.\n");
        (void)remove_owned_file(input_file, "benchmark input");
        (void)remove_owned_file(output_file, "benchmark output");
        free(input_file);
        free(output_file);
        return 1;
    }

    printf("===============================================================================================\n");
    printf("              EXTERNAL SORT BENCHMARK: FILE SIZE, K-WAY MERGE, PASSES AND I/O                  \n");
    printf("===============================================================================================\n");

    for (s = 0; s < sizeof(file_sizes) / sizeof(file_sizes[0]); ++s) {
        int N = file_sizes[s];
        size_t k_idx;
        printf("\n>>> Data size N = %d records (RAM buffer M = %d)\n", N, M);
        printf("---------------------------------------------------------------------------------------------------------------\n");
        printf("| K | Initial runs | Actual passes | Theoretical passes | Bytes read | Bytes written | Total I/O (MiB) | CPU time (ms) |\n");
        printf("---------------------------------------------------------------------------------------------------------------\n");

        input_stream = fopen(input_file, "w");
        if (input_stream == NULL) {
            fprintf(stderr, "Error: cannot open owned benchmark input '%s': %s\n",
                    input_file, strerror(errno));
            goto cleanup;
        }
        if (!generate_random_file(input_stream, N)) {
            (void)fclose(input_stream);
            input_stream = NULL;
            goto cleanup;
        }
        if (fclose(input_stream) != 0) {
            input_stream = NULL;
            fprintf(stderr, "Error: cannot finish benchmark input.\n");
            goto cleanup;
        }
        input_stream = NULL;

        for (k_idx = 0; k_idx < sizeof(k_values) / sizeof(k_values[0]); ++k_idx) {
            int K = k_values[k_idx];
            IOStats stats;
            clock_t start = clock();
            clock_t end;
            double time_ms;
            double total_io_mb;
            size_t theoretical;

            if (external_sort(input_file, output_file, M, K, &stats) != 0) {
                fprintf(stderr, "Error: benchmark sort failed for N=%d, K=%d.\n", N, K);
                goto cleanup;
            }
            end = clock();
            if (start == (clock_t)-1 || end == (clock_t)-1) time_ms = -1.0;
            else time_ms = ((double)(end - start) / (double)CLOCKS_PER_SEC) * 1000.0;
            total_io_mb = (double)(stats.bytes_read + stats.bytes_written) / (1024.0 * 1024.0);
            theoretical = theoretical_total_passes(stats.initial_runs, (size_t)K);

            printf("| %-1d | %-12zu | %-13zu | %-18zu | %-10llu | %-13llu | %-14.3f | %-9.2f |\n",
                   K, stats.initial_runs, stats.total_passes, theoretical,
                   stats.bytes_read, stats.bytes_written, total_io_mb, time_ms);
        }
    }
    printf("===============================================================================================\n");
    exit_code = 0;

cleanup:
    if (input_stream != NULL && fclose(input_stream) != 0)
        fprintf(stderr, "Warning: failed to close benchmark input stream.\n");
    (void)remove_owned_file(input_file, "benchmark input");
    (void)remove_owned_file(output_file, "benchmark output");
    free(input_file);
    free(output_file);
    return exit_code;
}

static bool file_matches_text(const char *filename, const char *expected_text);

/* Stress tests create unique fixtures and remove only files owned by this run. */
#if !defined(_WIN32) && defined(__unix__) && defined(RLIMIT_FSIZE) && defined(SIGXFSZ)
/* Return 1 on pass, 0 on failure, and -1 when this platform cannot run the check. */
static int stress_final_write_failure_preserves_output(void) {
    char *input_path = NULL;
    char *output_path = NULL;
    FILE *input = NULL;
    FILE *output = NULL;
    struct rlimit original_limit;
    struct rlimit limited_limit;
    void (*original_handler)(int) = SIG_DFL;
    bool handler_changed = false;
    bool limit_changed = false;
    bool setup_ok = true;
    bool preserved = false;
    int sort_result = 0;
    int i;
    IOStats stats;

    input_path = create_temp_file_near("stress_final_write_input", 0, 0, &input);
    if (input_path == NULL || input == NULL) setup_ok = false;
    if (setup_ok) {
        /* Each 100-record run stays below 512 bytes; the merged result exceeds it. */
        for (i = 0; i < 300; ++i) {
            if (fprintf(input, "%d\n", i) < 0) {
                setup_ok = false;
                break;
            }
        }
        if (fclose(input) != 0) setup_ok = false;
        input = NULL;
    }

    output_path = create_temp_file_near("stress_final_write_output", 0, 0, &output);
    if (output_path == NULL || output == NULL) setup_ok = false;
    if (setup_ok) {
        if (fputs("PRESERVE_FINAL_OUTPUT\n", output) == EOF) setup_ok = false;
        if (fclose(output) != 0) setup_ok = false;
        output = NULL;
    }

    if (!setup_ok) goto cleanup;
    if (getrlimit(RLIMIT_FSIZE, &original_limit) != 0) goto unsupported;
    if (original_limit.rlim_max != RLIM_INFINITY && original_limit.rlim_max < (rlim_t)512)
        goto unsupported;

    limited_limit = original_limit;
    limited_limit.rlim_cur = (rlim_t)512;
    original_handler = signal(SIGXFSZ, SIG_IGN);
    if (original_handler == SIG_ERR) goto cleanup;
    handler_changed = true;
    if (setrlimit(RLIMIT_FSIZE, &limited_limit) != 0) goto cleanup;
    limit_changed = true;

    (void)fflush(stdout);
    sort_result = external_sort(input_path, output_path, 100, 3, &stats);

    if (setrlimit(RLIMIT_FSIZE, &original_limit) != 0) {
        fprintf(stderr, "Error: could not restore the original file-size limit.\n");
        goto cleanup;
    }
    limit_changed = false;
    if (signal(SIGXFSZ, original_handler) == SIG_ERR) goto cleanup;
    handler_changed = false;

    if (sort_result == 0 || stats.initial_runs != 3) goto cleanup;
    preserved = file_matches_text(output_path, "PRESERVE_FINAL_OUTPUT\n");
    goto cleanup;

unsupported:
    fprintf(stderr, " -> Status: SKIP (RLIMIT_FSIZE cannot be set to the required test limit)\n");
    if (input != NULL) { (void)fclose(input); input = NULL; }
    if (output != NULL) { (void)fclose(output); output = NULL; }
    (void)remove_owned_file(input_path, "stress input");
    (void)remove_owned_file(output_path, "stress output");
    free(input_path);
    free(output_path);
    return -1;

cleanup:
    if (limit_changed && setrlimit(RLIMIT_FSIZE, &original_limit) != 0)
        fprintf(stderr, "Error: could not restore the original file-size limit.\n");
    if (handler_changed && signal(SIGXFSZ, original_handler) == SIG_ERR)
        fprintf(stderr, "Error: could not restore the original SIGXFSZ handler.\n");
    if (input != NULL) (void)fclose(input);
    if (output != NULL) (void)fclose(output);
    (void)remove_owned_file(input_path, "stress input");
    (void)remove_owned_file(output_path, "stress output");
    free(input_path);
    free(output_path);
    return preserved ? 1 : 0;
}
#endif

static int run_stress_test(void) {
    int failures = 0;
    char *malformed_in = NULL, *malformed_out = NULL;
    char *overlong_in = NULL, *overlong_out = NULL;
    char *nul_in = NULL, *nul_out = NULL;
    char *valid_in = NULL;
    char *preserve_in = NULL, *preserve_out = NULL;
    FILE *file = NULL;
    IOStats stats;
    int result;
    int total_tests = 5;
    size_t i;

    printf("===============================================================================================\n");
    printf("                         EXTERNAL SORT HEAVY STRESS TEST SUITE                                 \n");
    printf("===============================================================================================\n");

    printf("\n[STRESS TEST 1] Malformed Integer Token Rejection\n");
    printf("-----------------------------------------------------------------------------------------------\n");
    malformed_in = create_temp_file_near("stress_malformed_input", 0, 0, &file);
    if (malformed_in != NULL && file != NULL) {
        bool ok = fputs("100\n \t\r\n 200 \n invalid_string_token \n 300\n", file) != EOF;
        if (fclose(file) != 0) ok = false;
        file = NULL;
        malformed_out = create_temp_file_near("stress_malformed_output", 0, 0, &file);
        if (file != NULL) { if (fclose(file) != 0) ok = false; file = NULL; }
        result = (ok && malformed_out != NULL) ? external_sort(malformed_in, malformed_out, 5, 2, &stats) : 0;
        if (ok && malformed_out != NULL && result != 0) printf(" -> Status: PASS (malformed token rejected)\n");
        else { printf(" -> Status: FAIL (fixture creation or rejection failed)\n"); failures++; }
    } else { printf(" -> Status: FAIL (cannot create fixture)\n"); failures++; }

    printf("\n[STRESS TEST 2] Overlong Integer Token Rejection\n");
    printf("-----------------------------------------------------------------------------------------------\n");
    overlong_in = create_temp_file_near("stress_overlong_input", 0, 0, &file);
    if (overlong_in != NULL && file != NULL) {
        bool ok = true;
        for (i = 0; i < TOKEN_BUFFER_SIZE + 20; ++i)
            if (fputc('9', file) == EOF) ok = false;
        if (fputc('\n', file) == EOF) ok = false;
        if (fclose(file) != 0) ok = false;
        file = NULL;
        overlong_out = create_temp_file_near("stress_overlong_output", 0, 0, &file);
        if (file != NULL) { if (fclose(file) != 0) ok = false; file = NULL; }
        result = (ok && overlong_out != NULL) ? external_sort(overlong_in, overlong_out, 5, 2, &stats) : 0;
        if (ok && overlong_out != NULL && result != 0) printf(" -> Status: PASS (overlong token rejected)\n");
        else { printf(" -> Status: FAIL (fixture creation or rejection failed)\n"); failures++; }
    } else { printf(" -> Status: FAIL (cannot create fixture)\n"); failures++; }

    printf("\n[STRESS TEST 3] Embedded NUL Byte Rejection\n");
    printf("-----------------------------------------------------------------------------------------------\n");
    nul_in = create_temp_file_near("stress_nul_input", 0, 0, &file);
    if (nul_in != NULL && file != NULL) {
        bool ok = fputs("12", file) != EOF && fputc('\0', file) != EOF &&
                  fputs("junk\n4\n", file) != EOF;
        if (fclose(file) != 0) ok = false;
        file = NULL;
        nul_out = create_temp_file_near("stress_nul_output", 0, 0, &file);
        if (file != NULL) { if (fclose(file) != 0) ok = false; file = NULL; }
        result = (ok && nul_out != NULL) ? external_sort(nul_in, nul_out, 1, 2, &stats) : 0;
        if (ok && nul_out != NULL && result != 0) printf(" -> Status: PASS (binary-corrupted token rejected)\n");
        else { printf(" -> Status: FAIL (fixture creation or rejection failed)\n"); failures++; }
    } else { printf(" -> Status: FAIL (cannot create fixture)\n"); failures++; }

    printf("\n[STRESS TEST 4] Final Output Destination Failure\n");
    printf("-----------------------------------------------------------------------------------------------\n");
    valid_in = create_temp_file_near("stress_valid_input", 0, 0, &file);
    if (valid_in != NULL && file != NULL) {
        bool ok = fputs("10\n20\n30\n40\n50\n", file) != EOF;
        if (fclose(file) != 0) ok = false;
        file = NULL;
        /* Existing directory: temp runs can be created beside it, but it cannot be replaced. */
        result = ok ? external_sort(valid_in, ".", 2, 2, &stats) : 0;
        if (ok && result != 0 && stats.initial_runs == 3)
            printf(" -> Status: PASS (merge completed to a temp file; replacing a directory was rejected)\n");
        else { printf(" -> Status: FAIL (expected destination failure was not detected)\n"); failures++; }
    } else { printf(" -> Status: FAIL (cannot create fixture)\n"); failures++; }

    printf("\n[STRESS TEST 5] Preserve Existing Destination After Input Failure\n");
    printf("-----------------------------------------------------------------------------------------------\n");
    preserve_in = create_temp_file_near("stress_preserve_input", 0, 0, &file);
    if (preserve_in != NULL && file != NULL) {
        bool ok = fputs("10\n20\ninvalid\n30\n", file) != EOF;
        if (fclose(file) != 0) ok = false;
        file = NULL;
        preserve_out = create_temp_file_near("stress_preserve_output", 0, 0, &file);
        if (preserve_out != NULL && file != NULL) {
            if (fputs("PRESERVE_ME\n", file) == EOF) ok = false;
            if (fclose(file) != 0) ok = false;
            file = NULL;
        } else ok = false;
        result = ok ? external_sort(preserve_in, preserve_out, 1, 2, &stats) : 0;
        if (ok && result != 0) {
            bool preserved = file_matches_text(preserve_out, "PRESERVE_ME\n");
            if (preserved) printf(" -> Status: PASS (pre-existing destination contents preserved)\n");
            else { printf(" -> Status: FAIL (existing destination changed after failed input)\n"); failures++; }
        } else { printf(" -> Status: FAIL (could not trigger malformed-input failure)\n"); failures++; }
    } else { printf(" -> Status: FAIL (cannot create fixture)\n"); failures++; }

#if !defined(_WIN32) && defined(__unix__) && defined(RLIMIT_FSIZE) && defined(SIGXFSZ)
    total_tests++;
    printf("\n[STRESS TEST 6] Preserve Existing Destination After Final Merge Write Failure\n");
    printf("-----------------------------------------------------------------------------------------------\n");
    {
        int write_failure_result = stress_final_write_failure_preserves_output();
        if (write_failure_result > 0) {
            printf(" -> Status: PASS (final merge write failed; existing destination was preserved)\n");
        } else if (write_failure_result == 0) {
            printf(" -> Status: FAIL (destination was not preserved after final write failure)\n");
            failures++;
        } else {
            total_tests--;
            printf(" -> Status: SKIP (file-size limit test is unavailable on this system)\n");
        }
    }
#endif

    if (file != NULL) (void)fclose(file);
    (void)remove_owned_file(malformed_in, "stress input");
    (void)remove_owned_file(malformed_out, "stress output");
    (void)remove_owned_file(overlong_in, "stress input");
    (void)remove_owned_file(overlong_out, "stress output");
    (void)remove_owned_file(nul_in, "stress input");
    (void)remove_owned_file(nul_out, "stress output");
    (void)remove_owned_file(valid_in, "stress input");
    (void)remove_owned_file(preserve_in, "stress input");
    (void)remove_owned_file(preserve_out, "stress output");
    free(malformed_in); free(malformed_out);
    free(overlong_in); free(overlong_out);
    free(nul_in); free(nul_out); free(valid_in);
    free(preserve_in); free(preserve_out);

    printf("===============================================================================================\n");
    if (failures == 0) {
        printf("                             STRESS TEST SUITE PASSED (%d/%d)                                  \n", total_tests, total_tests);
        return 0;
    }
    printf("                             STRESS TEST SUITE FAILED (%d failure(s) / %d tests)               \n", failures, total_tests);
    return 1;
}

static bool file_matches_text(const char *filename, const char *expected_text) {
    FILE *file;
    size_t i;
    bool matches = true;
    int c;
    if (filename == NULL || expected_text == NULL) return false;
    file = fopen(filename, "rb");
    if (file == NULL) return false;
    for (i = 0; expected_text[i] != '\0'; ++i) {
        c = fgetc(file);
        if (c == EOF || c != (unsigned char)expected_text[i]) {
            matches = false;
            break;
        }
    }
    if (matches && (fgetc(file) != EOF || ferror(file))) matches = false;
    if (fclose(file) != 0) matches = false;
    return matches;
}

static bool output_matches(const char *filename, const int *expected, size_t expected_count) {
    FILE *file = fopen(filename, "r");
    IOStats ignored = {0};
    size_t index = 0;
    bool matches = true;
    if (file == NULL) return false;
    for (;;) {
        int value;
        int status = read_next_int(file, &value, &ignored);
        if (status < 0) {
            matches = false;
            break;
        }
        if (status == 0) break;
        if (index >= expected_count || value != expected[index]) {
            matches = false;
            break;
        }
        index++;
    }
    if (index != expected_count) matches = false;
    if (fclose(file) != 0) matches = false;
    return matches;
}

typedef struct {
    const char *name;
    const char *input_text;
    int M;
    int K;
    const int *expected;
    size_t expected_count;
    bool should_succeed;
} TestCase;

static bool run_one_test(const TestCase *test, size_t number) {
    char *input_path = NULL;
    char *output_path = NULL;
    FILE *input = NULL;
    FILE *output_reservation = NULL;
    IOStats stats;
    int result;
    bool passed;

    input_path = create_temp_file_near("extsort_test_input", 0, number, &input);
    if (input_path == NULL || input == NULL) {
        fprintf(stderr, "FAIL: %s (cannot create unique test input)\n", test->name);
        free(input_path);
        return false;
    }
    {
        bool write_ok = fputs(test->input_text, input) != EOF;
        if (fclose(input) != 0) write_ok = false;
        input = NULL;
        if (!write_ok) {
            (void)remove_owned_file(input_path, "test input");
            free(input_path);
            fprintf(stderr, "FAIL: %s (cannot write test input)\n", test->name);
            return false;
        }
    }

    output_path = create_temp_file_near("extsort_test_output", 0, number,
                                        &output_reservation);
    if (output_path == NULL || output_reservation == NULL) {
        (void)remove_owned_file(input_path, "test input");
        free(input_path);
        free(output_path);
        fprintf(stderr, "FAIL: %s (cannot reserve unique test output)\n", test->name);
        return false;
    }
    if (fclose(output_reservation) != 0) {
        output_reservation = NULL;
        (void)remove_owned_file(input_path, "test input");
        (void)remove_owned_file(output_path, "test output");
        free(input_path); free(output_path);
        fprintf(stderr, "FAIL: %s (cannot close test output reservation)\n", test->name);
        return false;
    }
    output_reservation = NULL;

    result = external_sort(input_path, output_path, test->M, test->K, &stats);
    if (test->should_succeed)
        passed = result == 0 && output_matches(output_path, test->expected, test->expected_count);
    else
        passed = result != 0;

    (void)remove_owned_file(input_path, "test input");
    (void)remove_owned_file(output_path, "test output");
    free(input_path); free(output_path);

    if (passed) printf("[PASS] %s\n", test->name);
    else printf("[FAIL] %s\n", test->name);
    return passed;
}

static bool run_embedded_nul_test(void) {
    char *input_path = NULL;
    char *output_path = NULL;
    FILE *input = NULL;
    FILE *output_reservation = NULL;
    IOStats stats;
    bool write_ok;
    bool passed;
    int result;

    input_path = create_temp_file_near("extsort_test_nul_input", 0, 0, &input);
    if (input_path == NULL || input == NULL) {
        fprintf(stderr, "FAIL: embedded NUL byte is rejected (cannot create input)\n");
        free(input_path);
        return false;
    }
    write_ok = fputs("12", input) != EOF && fputc('\0', input) != EOF &&
               fputs("junk\n4\n", input) != EOF;
    if (fclose(input) != 0) write_ok = false;
    input = NULL;
    if (!write_ok) {
        (void)remove_owned_file(input_path, "test input");
        free(input_path);
        fprintf(stderr, "FAIL: embedded NUL byte is rejected (cannot write input)\n");
        return false;
    }
    output_path = create_temp_file_near("extsort_test_nul_output", 0, 0,
                                        &output_reservation);
    if (output_path == NULL || output_reservation == NULL) {
        (void)remove_owned_file(input_path, "test input");
        free(input_path); free(output_path);
        fprintf(stderr, "FAIL: embedded NUL byte is rejected (cannot reserve output)\n");
        return false;
    }
    if (fclose(output_reservation) != 0) {
        (void)remove_owned_file(input_path, "test input");
        (void)remove_owned_file(output_path, "test output");
        free(input_path); free(output_path);
        return false;
    }
    result = external_sort(input_path, output_path, 1, 2, &stats);
    passed = result != 0;
    (void)remove_owned_file(input_path, "test input");
    (void)remove_owned_file(output_path, "test output");
    free(input_path); free(output_path);
    if (passed) printf("[PASS] embedded NUL byte is rejected\n");
    else printf("[FAIL] embedded NUL byte is rejected\n");
    return passed;
}

static bool run_output_preservation_test(void) {
    char *input_path = NULL;
    char *output_path = NULL;
    FILE *input = NULL;
    FILE *output = NULL;
    IOStats stats;
    bool setup_ok = true;
    bool preserved = false;
    int result;

    input_path = create_temp_file_near("extsort_preserve_input", 0, 0, &input);
    if (input_path == NULL || input == NULL) setup_ok = false;
    if (setup_ok) {
        if (fputs("10\n20\ninvalid\n30\n", input) == EOF) setup_ok = false;
        if (fclose(input) != 0) setup_ok = false;
        input = NULL;
    }
    output_path = create_temp_file_near("extsort_preserve_output", 0, 0, &output);
    if (output_path == NULL || output == NULL) setup_ok = false;
    if (setup_ok) {
        if (fputs("ORIGINAL_OUTPUT\n", output) == EOF) setup_ok = false;
        if (fclose(output) != 0) setup_ok = false;
        output = NULL;
    }
    if (input != NULL) (void)fclose(input);
    if (output != NULL) (void)fclose(output);

    result = setup_ok ? external_sort(input_path, output_path, 1, 2, &stats) : 0;
    if (setup_ok && result != 0)
        preserved = file_matches_text(output_path, "ORIGINAL_OUTPUT\n");
    (void)remove_owned_file(input_path, "test input");
    (void)remove_owned_file(output_path, "test output");
    free(input_path); free(output_path);
    if (preserved) printf("[PASS] failed input does not overwrite existing destination\n");
    else printf("[FAIL] failed input does not overwrite existing destination\n");
    return preserved;
}

static int run_tests(void) {
    static const int small_expected[] = {-5, 0, 7};
    static const int sorted_expected[] = {1, 2, 3, 4, 5};
    static const int reverse_expected[] = {1, 2, 3, 4, 5, 6};
    static const int duplicate_expected[] = {-1, 2, 2, 2, 9, 9};
    static const int negative_expected[] = {-10, -1, 0, 0, 3, 8};
    static const int partial_expected[] = {1, 2, 3, 4, 5, 6, 7};
    static const int multiple_pass_expected[] = {-4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    static const int extremes_expected[] = {INT_MIN, -1, 0, 1, INT_MAX};

    static const TestCase cases[] = {
        {"empty input", "", 3, 2, NULL, 0, true},
        {"input smaller than M", "7\n-5\n0\n", 10, 2, small_expected, 3, true},
        {"already sorted input", "1\n2\n3\n4\n5\n", 2, 2, sorted_expected, 5, true},
        {"reverse-sorted input", "6\n5\n4\n3\n2\n1\n", 2, 3, reverse_expected, 6, true},
        {"duplicate values", "9\n2\n2\n-1\n9\n2\n", 2, 2, duplicate_expected, 6, true},
        {"negative values and zeros", "0 -10 8 0 -1 3\n", 3, 2, negative_expected, 6, true},
        {"incomplete final chunk", "7 1 6 2 5 3 4", 3, 2, partial_expected, 7, true},
        {"multiple merge passes", "12 -4 7 1 0 11 3 8 -2 9 6 -1 4 10 2 5 -3", 2, 3, multiple_pass_expected, 17, true},
        {"INT_MIN/INT_MAX comparator safety", "2147483647 -2147483648 0 1 -1", 2, 2, extremes_expected, 5, true},
        {"malformed integer is rejected", "1\nnot_an_integer\n3\n", 2, 2, NULL, 0, false},
        {"invalid merge order K=1 rejected", "1 2 3", 2, 1, NULL, 0, false}
    };

    size_t i;
    size_t passed = 0;
    size_t total = sizeof(cases) / sizeof(cases[0]) + 2;

    printf("Running %zu built-in tests...\n", total);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (run_one_test(&cases[i], i)) passed++;
    }
    if (run_embedded_nul_test()) passed++;
    if (run_output_preservation_test()) passed++;
    printf("Test summary: %zu/%zu passed.\n", passed, total);
    return passed == total ? 0 : 1;
}

static bool parse_positive_int(const char *text, int *value) {
    char *end = NULL;
    long parsed;
    if (text == NULL || value == NULL || *text == '\0') return false;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed <= 0 || parsed > INT_MAX)
        return false;
    *value = (int)parsed;
    return true;
}

static void print_usage(const char *program) {
    fprintf(stderr,
            "Usage:\n"
            "  %s                        Run benchmark (N = 5k, 20k, 100k, 1,000k)\n"
            "  %s --benchmark              Run benchmark (N = 5k, 20k, 100k, 1,000k)\n"
            "  %s --test                   Run built-in correctness tests\n"
            "  %s --stress                 Run heavy stress tests (failure paths & malformed input)\n"
            "  %s INPUT OUTPUT [M [K]]       Sort integers in INPUT into OUTPUT\n",
            program, program, program, program, program);
}

int main(int argc, char **argv) {
    if (argc == 1) return run_benchmark();
    if (argc == 2 && strcmp(argv[1], "--benchmark") == 0) return run_benchmark();
    if (argc == 2 && strcmp(argv[1], "--test") == 0)      return run_tests();
    if (argc == 2 && strcmp(argv[1], "--stress") == 0)    return run_stress_test();

    if (argc >= 3 && argc <= 5) {
        int M = DEFAULT_M;
        int K = DEFAULT_K;
        IOStats stats;
        if (argc >= 4 && !parse_positive_int(argv[3], &M)) {
            fprintf(stderr, "Error: M must be a positive integer.\n");
            return 2;
        }
        if (argc >= 5 && !parse_positive_int(argv[4], &K)) {
            fprintf(stderr, "Error: K must be a positive integer.\n");
            return 2;
        }
        if (K < 2) {
            fprintf(stderr, "Error: K must be at least 2 to guarantee progress.\n");
            return 2;
        }
        if (external_sort(argv[1], argv[2], M, K, &stats) != 0) return 1;
        printf("Sorting completed successfully.\n");
        printf("Initial runs: %zu\n", stats.initial_runs);
        printf("Merge passes: %zu\n", stats.merge_passes);
        printf("Total processing passes: %zu\n", stats.total_passes);
        printf("Bytes read: %llu\n", stats.bytes_read);
        printf("Bytes written: %llu\n", stats.bytes_written);
        printf("Total data I/O: %llu bytes (%.3f MiB)\n",
               stats.bytes_read + stats.bytes_written,
               (double)(stats.bytes_read + stats.bytes_written) / (1024.0 * 1024.0));
        return 0;
    }

    print_usage(argv[0]);
    return 2;
}
