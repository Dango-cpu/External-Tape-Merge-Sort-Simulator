/*
 * External sort and k-way merging benchmark (C11).
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -Wpedantic -O2 external_sort_kway_merge_fixed.c -lm -o external_sort
 *
 * Run the benchmark (default):
 *   ./external_sort
 *   ./external_sort --benchmark
 *
 * Run the built-in correctness tests:
 *   ./external_sort --test
 *
 * Sort a whitespace-separated integer file:
 *   ./external_sort input.txt output.txt [M [K]]
 *
 * M is the maximum number of integers held in the in-memory chunk.
 * K is the maximum number of sorted runs merged at once and must be >= 2.
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_M 1000
#define DEFAULT_K 2
#define TOKEN_BUFFER_SIZE 128

/* Counters describe bytes read/written by the sorting algorithm, not filesystem
 * metadata, directory operations, or the benchmark's separate input generator. */
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

static bool run_list_push_owned(RunList *list, char *path) {
    char **new_items;
    size_t new_capacity;

    if (list == NULL || path == NULL) return false;
    if (list->count == list->capacity) {
        new_capacity = (list->capacity == 0) ? 8 : list->capacity * 2;
        if (new_capacity < list->capacity ||
            new_capacity > ((size_t)-1) / sizeof(*list->items)) {
            return false;
        }
        new_items = (char **)realloc(list->items,
                                     new_capacity * sizeof(*list->items));
        if (new_items == NULL) return false;
        list->items = new_items;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = path;
    return true;
}

static void run_list_destroy(RunList *list, bool remove_files) {
    size_t i;
    if (list == NULL) return;
    for (i = 0; i < list->count; ++i) {
        if (list->items[i] != NULL) {
            if (remove_files) (void)remove(list->items[i]);
            free(list->items[i]);
        }
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static char *make_temp_path(const char *output_file, size_t pass, size_t index) {
    int needed;
    char *path;
    unsigned long long serial = ++g_temp_counter;
    unsigned long long timestamp = (unsigned long long)time(NULL);

    needed = snprintf(NULL, 0, "%s.extsort_%llu_%llu_%zu_%zu.tmp",
                      output_file, timestamp, serial, pass, index);
    if (needed < 0) return NULL;
    path = (char *)malloc((size_t)needed + 1);
    if (path == NULL) return NULL;
    if (snprintf(path, (size_t)needed + 1,
                 "%s.extsort_%llu_%llu_%zu_%zu.tmp",
                 output_file, timestamp, serial, pass, index) != needed) {
        free(path);
        return NULL;
    }
    return path;
}

/* Returns 1 for an integer, 0 for clean EOF, and -1 for malformed input/I/O. */
static int read_next_int(FILE *stream, int *value, IOStats *stats) {
    int c;
    char token[TOKEN_BUFFER_SIZE];
    size_t length = 0;
    bool too_long = false;
    char *end = NULL;
    long parsed;

    if (stream == NULL || value == NULL) return -1;

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
        if (length + 1 < sizeof(token)) {
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
    /* Relational comparison avoids signed overflow for INT_MIN/INT_MAX. */
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

/* Generate sorted runs, each holding at most M integer values. */
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
        fclose(input);
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
            char *run_path = make_temp_path(output_file, 0, runs->count);
            FILE *run_file;
            size_t i;
            bool write_ok = true;
            if (run_path == NULL) {
                fprintf(stderr, "Error: cannot allocate a temporary run path.\n");
                goto cleanup;
            }
            run_file = fopen(run_path, "w");
            if (run_file == NULL) {
                fprintf(stderr, "Error: cannot create temporary run '%s': %s\n",
                        run_path, strerror(errno));
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
                (void)remove(run_path);
                free(run_path);
                goto cleanup;
            }
            if (!run_list_push_owned(runs, run_path)) {
                fprintf(stderr, "Error: cannot grow the temporary-run list.\n");
                (void)remove(run_path);
                free(run_path);
                goto cleanup;
            }
        }
        if (count < M) break; /* EOF was reached before the buffer filled. */
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

/* Merge a group of sorted runs into output_file without deleting the inputs. */
static bool merge_runs(char *const input_runs[], size_t num_runs,
                       const char *output_file, IOStats *stats) {
    FILE **input_files = NULL;
    MinHeap *heap = NULL;
    FILE *output = NULL;
    bool ok = false;
    size_t i;

    if (num_runs == 0) return false;
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

    output = fopen(output_file, "w");
    if (output == NULL) {
        fprintf(stderr, "Error: cannot create output file '%s': %s\n",
                output_file, strerror(errno));
        goto cleanup;
    }

    while (heap->size > 0) {
        HeapNode smallest = heap_extract_min(heap);
        int next_value;
        int status;
        if (!write_int(output, smallest.value, stats)) {
            fprintf(stderr, "Error: failed while writing output file '%s'.\n", output_file);
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
        fprintf(stderr, "Error: failed to finish writing '%s'.\n", output_file);
        goto cleanup;
    }
    output = NULL;
    ok = true;

cleanup:
    if (output != NULL && fclose(output) != 0) ok = false;
    if (input_files != NULL) {
        for (i = 0; i < num_runs; ++i) {
            if (input_files[i] != NULL && fclose(input_files[i]) != 0) ok = false;
        }
    }
    if (!ok) (void)remove(output_file);
    free(input_files);
    heap_destroy(heap);
    return ok;
}

static bool create_empty_output(const char *output_file) {
    FILE *output = fopen(output_file, "w");
    if (output == NULL) {
        fprintf(stderr, "Error: cannot create output file '%s': %s\n",
                output_file, strerror(errno));
        return false;
    }
    if (fclose(output) != 0) {
        fprintf(stderr, "Error: failed to close output file '%s'.\n", output_file);
        return false;
    }
    return true;
}

static bool move_single_run_to_output(const char *run_path, const char *output_file) {
    if (remove(output_file) != 0 && errno != ENOENT) {
        fprintf(stderr, "Error: cannot replace output file '%s': %s\n",
                output_file, strerror(errno));
        return false;
    }
    if (rename(run_path, output_file) != 0) {
        fprintf(stderr, "Error: cannot move the final run to '%s': %s\n",
                output_file, strerror(errno));
        return false;
    }
    return true;
}

/* Returns 0 on success, -1 on failure. */
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
    stats->total_passes = 1; /* Initial run generation/input scan. */

    if (current.count == 0) {
        bool ok = create_empty_output(output_file);
        run_list_destroy(&current, false);
        return ok ? 0 : -1;
    }
    if (current.count == 1) {
        bool ok = move_single_run_to_output(current.items[0], output_file);
        if (ok) {
            free(current.items[0]); /* The file was renamed; release its old path string. */
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
            if (!merge_runs(current.items, current.count, output_file, stats)) {
                run_list_destroy(&current, true);
                return -1;
            }
            run_list_destroy(&current, true);
            stats->merge_passes++;
            stats->total_passes++;
            return 0;
        }

        for (i = 0; i < current.count; i += (size_t)K) {
            size_t group_count = current.count - i;
            if (group_count > (size_t)K) group_count = (size_t)K;

            /* A lone run need not be read and rewritten; carry it to next pass. */
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
                char *new_path = make_temp_path(output_file,
                                                stats->merge_passes + 1,
                                                next.count);
                if (new_path == NULL) {
                    fprintf(stderr, "Error: cannot allocate a merge-run path.\n");
                    run_list_destroy(&next, true);
                    run_list_destroy(&current, true);
                    return -1;
                }
                if (!merge_runs(&current.items[i], group_count, new_path, stats)) {
                    free(new_path);
                    run_list_destroy(&next, true);
                    run_list_destroy(&current, true);
                    return -1;
                }
                if (!run_list_push_owned(&next, new_path)) {
                    fprintf(stderr, "Error: cannot grow the next-run list.\n");
                    (void)remove(new_path);
                    free(new_path);
                    run_list_destroy(&next, true);
                    run_list_destroy(&current, true);
                    return -1;
                }
                {
                    size_t j;
                    for (j = 0; j < group_count; ++j) {
                        (void)remove(current.items[i + j]);
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

    /* Normally the loop ends through a final merge; keep a safe fallback. */
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

static bool generate_random_file(const char *filename, int count) {
    FILE *file = fopen(filename, "w");
    int i;
    if (file == NULL) {
        fprintf(stderr, "Error: cannot create benchmark input '%s': %s\n",
                filename, strerror(errno));
        return false;
    }
    srand(42); /* Same prefix data for each N makes size comparisons repeatable. */
    for (i = 0; i < count; ++i) {
        if (fprintf(file, "%d\n", rand() % 1000000) < 0) {
            fprintf(stderr, "Error: failed while generating benchmark input.\n");
            fclose(file);
            return false;
        }
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "Error: failed to finish benchmark input '%s'.\n", filename);
        return false;
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

static int run_benchmark(void) {
    const int M = DEFAULT_M;
    const int file_sizes[] = {5000, 20000, 100000};
    const int k_values[] = {2, 3, 5, 10};
    const char *input_file = "benchmark_input.txt";
    const char *output_file = "benchmark_output.txt";
    size_t s;

    printf("===============================================================================================\n");
    printf("              EXTERNAL SORT BENCHMARK: FILE SIZE, K-WAY MERGE, PASSES AND I/O                  \n");
    printf("===============================================================================================\n");

    for (s = 0; s < sizeof(file_sizes) / sizeof(file_sizes[0]); ++s) {
        int N = file_sizes[s];
        size_t k_idx;
        printf("\n>>> Data size N = %d records (RAM buffer M = %d)\n", N, M);
        printf("---------------------------------------------------------------------------------------------------------------\n");
        printf("| K | Initial runs | Actual passes | Theoretical passes | Bytes read | Bytes written | Total I/O (MiB) | Time (ms) |\n");
        printf("---------------------------------------------------------------------------------------------------------------\n");

        if (!generate_random_file(input_file, N)) return 1;
        for (k_idx = 0; k_idx < sizeof(k_values) / sizeof(k_values[0]); ++k_idx) {
            int K = k_values[k_idx];
            IOStats stats;
            clock_t start = clock();
            clock_t end;
            double time_ms;
            double total_io_mb;
            size_t theoretical;

            if (external_sort(input_file, output_file, M, K, &stats) != 0) {
                (void)remove(input_file);
                (void)remove(output_file);
                return 1;
            }
            end = clock();
            time_ms = ((double)(end - start) / (double)CLOCKS_PER_SEC) * 1000.0;
            total_io_mb = (double)(stats.bytes_read + stats.bytes_written) / (1024.0 * 1024.0);
            theoretical = theoretical_total_passes(stats.initial_runs, (size_t)K);

            printf("| %-1d | %-12zu | %-13zu | %-18zu | %-10llu | %-13llu | %-14.3f | %-9.2f |\n",
                   K, stats.initial_runs, stats.total_passes, theoretical,
                   stats.bytes_read, stats.bytes_written, total_io_mb, time_ms);
            if (remove(output_file) != 0 && errno != ENOENT) {
                fprintf(stderr, "Warning: could not remove benchmark output '%s'.\n", output_file);
            }
        }
        (void)remove(input_file);
    }
    printf("===============================================================================================\n");
    return 0;
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
    char input_path[160];
    char output_path[160];
    FILE *input;
    IOStats stats;
    int result;
    bool passed;
    unsigned long long stamp = (unsigned long long)time(NULL);

    (void)snprintf(input_path, sizeof(input_path), ".extsort_test_%llu_%zu_input.txt",
                   stamp, number);
    (void)snprintf(output_path, sizeof(output_path), ".extsort_test_%llu_%zu_output.txt",
                   stamp, number);
    input = fopen(input_path, "w");
    if (input == NULL) {
        fprintf(stderr, "FAIL: %s (cannot create test input)\n", test->name);
        return false;
    }
    {
        bool write_ok = fputs(test->input_text, input) != EOF;
        if (fclose(input) != 0) write_ok = false;
        if (!write_ok) {
            (void)remove(input_path);
            fprintf(stderr, "FAIL: %s (cannot write test input)\n", test->name);
            return false;
        }
    }
    (void)remove(output_path);
    result = external_sort(input_path, output_path, test->M, test->K, &stats);
    if (test->should_succeed) {
        passed = result == 0 && output_matches(output_path, test->expected, test->expected_count);
    } else {
        passed = result != 0;
    }
    (void)remove(input_path);
    (void)remove(output_path);
    if (passed) printf("[PASS] %s\n", test->name);
    else printf("[FAIL] %s\n", test->name);
    return passed;
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
        {"invalid merge order is rejected", "1 2 3", 2, 1, NULL, 0, false}
    };
    size_t i;
    size_t passed = 0;
    size_t total = sizeof(cases) / sizeof(cases[0]);

    printf("Running %zu built-in tests...\n", total);
    for (i = 0; i < total; ++i) {
        if (run_one_test(&cases[i], i)) passed++;
    }
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
            "  %s                         Run the default benchmark\n"
            "  %s --benchmark             Run the default benchmark\n"
            "  %s --test                  Run built-in correctness tests\n"
            "  %s INPUT OUTPUT [M [K]]   Sort integers in INPUT into OUTPUT\n",
            program, program, program, program);
}

int main(int argc, char **argv) {
    if (argc == 1) return run_benchmark();
    if (argc == 2 && strcmp(argv[1], "--benchmark") == 0) return run_benchmark();
    if (argc == 2 && strcmp(argv[1], "--test") == 0) return run_tests();

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
