#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_M 3
#define DEFAULT_K 2
#define TEST_INPUT_FILE "test_input.txt"
#define TEST_OUTPUT_FILE "test_output.txt"

typedef struct {
    int value;
    size_t run_id;
} HeapNode;

typedef struct {
    HeapNode *array;
    size_t size;
    size_t capacity;
} MinHeap;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} RunList;

typedef struct {
    char **paths;
    size_t count;
    size_t capacity;
} PathPool;

typedef struct {
    uint64_t bytes_read;
    uint64_t bytes_written;
    size_t initial_runs;
    size_t merge_passes;
} SortStats;

static int compare_ints(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);
}

static int read_next_int(FILE *file, int *value, uint64_t *bytes_read)
{
    int c;
    char token[128];
    size_t length = 0;
    bool too_long = false;
    char *end;
    long parsed;

    do {
        c = fgetc(file);
        if (c != EOF) {
            (*bytes_read)++;
        }
    } while (c != EOF && isspace((unsigned char)c));

    if (c == EOF) {
        return ferror(file) ? -1 : 0;
    }

    while (c != EOF && !isspace((unsigned char)c)) {
        if (length + 1 < sizeof(token)) {
            token[length++] = (char)c;
        } else {
            too_long = true;
        }
        c = fgetc(file);
        if (c != EOF) {
            (*bytes_read)++;
        }
    }

    if (c == EOF && ferror(file)) {
        return -1;
    }
    if (too_long || length == 0) {
        return -1;
    }

    token[length] = '\0';
    errno = 0;
    parsed = strtol(token, &end, 10);
    if (errno == ERANGE || end == token || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        return -1;
    }

    *value = (int)parsed;
    return 1;
}

static int write_int_line(FILE *file, int value, uint64_t *bytes_written)
{
    char line[64];
    int length = snprintf(line, sizeof(line), "%d\n", value);
    size_t written;

    if (length < 0 || (size_t)length >= sizeof(line)) {
        return 0;
    }

    written = fwrite(line, 1, (size_t)length, file);
    *bytes_written += (uint64_t)written;
    return written == (size_t)length;
}

static MinHeap *create_heap(size_t capacity)
{
    MinHeap *heap = malloc(sizeof(*heap));

    if (heap == NULL) {
        return NULL;
    }

    heap->array = NULL;
    heap->size = 0;
    heap->capacity = capacity;

    if (capacity > 0) {
        heap->array = malloc(capacity * sizeof(*heap->array));
        if (heap->array == NULL) {
            free(heap);
            return NULL;
        }
    }

    return heap;
}

static void swap_nodes(HeapNode *a, HeapNode *b)
{
    HeapNode temp = *a;
    *a = *b;
    *b = temp;
}

static void min_heapify(MinHeap *heap, size_t index)
{
    while (index < heap->size) {
        size_t smallest = index;
        size_t left = 2 * index + 1;
        size_t right = 2 * index + 2;

        if (left < heap->size &&
            heap->array[left].value < heap->array[smallest].value) {
            smallest = left;
        }
        if (right < heap->size &&
            heap->array[right].value < heap->array[smallest].value) {
            smallest = right;
        }
        if (smallest == index) {
            break;
        }
        swap_nodes(&heap->array[index], &heap->array[smallest]);
        index = smallest;
    }
}

static int insert_heap(MinHeap *heap, int value, size_t run_id)
{
    size_t index;

    if (heap == NULL || heap->size >= heap->capacity) {
        return 0;
    }

    index = heap->size++;
    heap->array[index].value = value;
    heap->array[index].run_id = run_id;

    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (heap->array[parent].value <= heap->array[index].value) {
            break;
        }
        swap_nodes(&heap->array[parent], &heap->array[index]);
        index = parent;
    }

    return 1;
}

static HeapNode extract_min(MinHeap *heap)
{
    HeapNode minimum = heap->array[0];
    heap->size--;

    if (heap->size > 0) {
        heap->array[0] = heap->array[heap->size];
        min_heapify(heap, 0);
    }

    return minimum;
}

static void free_heap(MinHeap *heap)
{
    if (heap != NULL) {
        free(heap->array);
        free(heap);
    }
}

static int append_reference(RunList *list, char *path)
{
    char **new_items;
    size_t new_capacity;

    if (list->count == list->capacity) {
        new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        if (new_capacity < list->capacity ||
            new_capacity > SIZE_MAX / sizeof(*list->items)) {
            return 0;
        }
        new_items = realloc(list->items, new_capacity * sizeof(*list->items));
        if (new_items == NULL) {
            return 0;
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count++] = path;
    return 1;
}

static char *create_temp_path(PathPool *pool, const char *output_file,
                              size_t generation, size_t index)
{
    int required;
    char *path;
    char **new_paths;
    size_t new_capacity;

    required = snprintf(NULL, 0, "%s.run_p%zu_%zu.tmp",
                        output_file, generation, index);
    if (required < 0) {
        return NULL;
    }

    path = malloc((size_t)required + 1);
    if (path == NULL) {
        return NULL;
    }
    snprintf(path, (size_t)required + 1, "%s.run_p%zu_%zu.tmp",
             output_file, generation, index);

    if (pool->count == pool->capacity) {
        new_capacity = pool->capacity == 0 ? 8 : pool->capacity * 2;
        if (new_capacity < pool->capacity ||
            new_capacity > SIZE_MAX / sizeof(*pool->paths)) {
            free(path);
            return NULL;
        }
        new_paths = realloc(pool->paths, new_capacity * sizeof(*pool->paths));
        if (new_paths == NULL) {
            free(path);
            return NULL;
        }
        pool->paths = new_paths;
        pool->capacity = new_capacity;
    }

    pool->paths[pool->count++] = path;
    return path;
}

static void cleanup_path_pool(PathPool *pool)
{
    size_t i;

    for (i = 0; i < pool->count; i++) {
        remove(pool->paths[i]);
        free(pool->paths[i]);
    }
    free(pool->paths);
    pool->paths = NULL;
    pool->count = 0;
    pool->capacity = 0;
}

static int write_sorted_run(const char *path, const int *values, size_t count,
                            SortStats *stats)
{
    FILE *out = fopen(path, "wb");
    size_t i;
    int success = 1;

    if (out == NULL) {
        perror(path);
        return 0;
    }

    for (i = 0; i < count; i++) {
        if (!write_int_line(out, values[i], &stats->bytes_written)) {
            fprintf(stderr, "Error writing temporary run: %s\n", path);
            success = 0;
            break;
        }
    }

    if (fclose(out) != 0) {
        perror(path);
        success = 0;
    }
    return success;
}

static int create_initial_runs(const char *input_file, size_t memory_limit,
                               const char *output_file, RunList *runs,
                               PathPool *pool, SortStats *stats)
{
    FILE *in = fopen(input_file, "rb");
    int *buffer;
    int success = 1;

    if (in == NULL) {
        perror(input_file);
        return 0;
    }

    buffer = malloc(memory_limit * sizeof(*buffer));
    if (buffer == NULL) {
        fprintf(stderr, "Unable to allocate the in-memory chunk buffer.\n");
        fclose(in);
        return 0;
    }

    while (success) {
        size_t items_read = 0;
        int result;

        while (items_read < memory_limit) {
            result = read_next_int(in, &buffer[items_read], &stats->bytes_read);
            if (result == 1) {
                items_read++;
            } else if (result == 0) {
                break;
            } else {
                fprintf(stderr,
                        "Invalid integer or read error in input file: %s\n",
                        input_file);
                success = 0;
                break;
            }
        }

        if (!success || items_read == 0) {
            break;
        }

        qsort(buffer, items_read, sizeof(*buffer), compare_ints);

        {
            char *path = create_temp_path(pool, output_file, 0, runs->count);
            if (path == NULL || !write_sorted_run(path, buffer, items_read, stats) ||
                !append_reference(runs, path)) {
                fprintf(stderr, "Unable to create an initial sorted run.\n");
                success = 0;
                break;
            }
        }
    }

    free(buffer);
    if (fclose(in) != 0) {
        perror(input_file);
        success = 0;
    }
    return success;
}

static int merge_k_runs(char *const input_runs[], size_t num_runs,
                        const char *output_file, SortStats *stats)
{
    FILE **in_files = NULL;
    FILE *out = NULL;
    MinHeap *heap = NULL;
    size_t i;
    int success = 1;

    in_files = calloc(num_runs, sizeof(*in_files));
    heap = create_heap(num_runs);
    if (in_files == NULL || heap == NULL) {
        fprintf(stderr, "Unable to allocate memory for k-way merge.\n");
        success = 0;
        goto cleanup;
    }

    for (i = 0; i < num_runs; i++) {
        int value;
        int result;

        in_files[i] = fopen(input_runs[i], "rb");
        if (in_files[i] == NULL) {
            perror(input_runs[i]);
            success = 0;
            goto cleanup;
        }

        result = read_next_int(in_files[i], &value, &stats->bytes_read);
        if (result == 1) {
            if (!insert_heap(heap, value, i)) {
                fprintf(stderr, "Min-heap capacity exceeded.\n");
                success = 0;
                goto cleanup;
            }
        } else if (result < 0) {
            fprintf(stderr, "Invalid data or read error in run: %s\n", input_runs[i]);
            success = 0;
            goto cleanup;
        }
    }

    out = fopen(output_file, "wb");
    if (out == NULL) {
        perror(output_file);
        success = 0;
        goto cleanup;
    }

    while (heap->size > 0) {
        HeapNode minimum = extract_min(heap);
        int next_value;
        int result;

        if (!write_int_line(out, minimum.value, &stats->bytes_written)) {
            fprintf(stderr, "Error writing merged run: %s\n", output_file);
            success = 0;
            goto cleanup;
        }

        result = read_next_int(in_files[minimum.run_id], &next_value,
                               &stats->bytes_read);
        if (result == 1) {
            if (!insert_heap(heap, next_value, minimum.run_id)) {
                fprintf(stderr, "Min-heap capacity exceeded.\n");
                success = 0;
                goto cleanup;
            }
        } else if (result < 0) {
            fprintf(stderr, "Invalid data or read error in run: %s\n",
                    input_runs[minimum.run_id]);
            success = 0;
            goto cleanup;
        }
    }

cleanup:
    if (out != NULL && fclose(out) != 0) {
        perror(output_file);
        success = 0;
    }
    if (in_files != NULL) {
        for (i = 0; i < num_runs; i++) {
            if (in_files[i] != NULL && fclose(in_files[i]) != 0) {
                perror(input_runs[i]);
                success = 0;
            }
        }
    }
    free(in_files);
    free_heap(heap);

    if (!success) {
        remove(output_file);
    }
    return success;
}

static int external_sort(const char *input_file, const char *output_file,
                         size_t memory_limit, size_t merge_order,
                         SortStats *stats)
{
    RunList current = {NULL, 0, 0};
    PathPool pool = {NULL, 0, 0};
    int success = 1;

    memset(stats, 0, sizeof(*stats));

    if (memory_limit == 0 || merge_order < 2 ||
        memory_limit > SIZE_MAX / sizeof(int) ||
        merge_order > SIZE_MAX / sizeof(HeapNode)) {
        fprintf(stderr, "M must be positive and K must be at least 2.\n");
        return 0;
    }

    if (!create_initial_runs(input_file, memory_limit, output_file,
                             &current, &pool, stats)) {
        success = 0;
        goto cleanup;
    }
    stats->initial_runs = current.count;

    while (current.count > 1) {
        RunList next = {NULL, 0, 0};
        size_t start = 0;
        size_t next_generation = stats->merge_passes + 1;

        while (start < current.count) {
            size_t remaining = current.count - start;
            size_t group_count = remaining < merge_order ? remaining : merge_order;
            char *path = create_temp_path(&pool, output_file,
                                          next_generation, next.count);
            size_t j;

            if (path == NULL ||
                !merge_k_runs(&current.items[start], group_count, path, stats)) {
                success = 0;
                break;
            }
            if (!append_reference(&next, path)) {
                fprintf(stderr, "Unable to store merged run metadata.\n");
                success = 0;
                break;
            }

            for (j = 0; j < group_count; j++) {
                remove(current.items[start + j]);
            }
            start += group_count;
        }

        if (!success) {
            free(next.items);
            break;
        }

        free(current.items);
        current = next;
        stats->merge_passes++;
    }

    if (success && current.count == 0) {
        FILE *empty_output = fopen(output_file, "wb");
        if (empty_output == NULL) {
            perror(output_file);
            success = 0;
        } else if (fclose(empty_output) != 0) {
            perror(output_file);
            success = 0;
        }
    } else if (success && current.count == 1) {
        remove(output_file);
        if (rename(current.items[0], output_file) != 0) {
            perror("Unable to move final sorted run to output file");
            success = 0;
        }
    }

cleanup:
    free(current.items);
    cleanup_path_pool(&pool);
    return success;
}

static int parse_positive_size(const char *text, size_t *value)
{
    char *end;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
        parsed > (unsigned long long)SIZE_MAX) {
        return 0;
    }

    *value = (size_t)parsed;
    return 1;
}

static size_t total_passes(const SortStats *stats)
{
    return (stats->initial_runs > 0 ? 1U : 0U) + stats->merge_passes;
}

static void print_stats(const SortStats *stats)
{
    printf("Initial runs: %zu\n", stats->initial_runs);
    printf("Merge passes: %zu\n", stats->merge_passes);
    printf("Total passes (run generation + merging): %zu\n", total_passes(stats));
    printf("Bytes read:  %" PRIu64 "\n", stats->bytes_read);
    printf("Bytes written: %" PRIu64 "\n", stats->bytes_written);
    printf("Total data I/O: %" PRIu64 " bytes\n",
           stats->bytes_read + stats->bytes_written);
}

static int read_and_check_output(const char *path, const int *expected,
                                 size_t expected_count)
{
    FILE *file = fopen(path, "rb");
    size_t index = 0;
    int success = 1;
    uint64_t ignored_bytes = 0;

    if (file == NULL) {
        perror(path);
        return 0;
    }

    printf("Output: [ ");
    for (;;) {
        int value;
        int result = read_next_int(file, &value, &ignored_bytes);
        if (result == 0) {
            break;
        }
        if (result < 0) {
            fprintf(stderr, "Invalid output file: %s\n", path);
            success = 0;
            break;
        }

        printf("%d ", value);
        if (index >= expected_count || value != expected[index]) {
            success = 0;
        }
        index++;
    }
    printf("]\n");

    if (index != expected_count) {
        success = 0;
    }
    if (fclose(file) != 0) {
        perror(path);
        success = 0;
    }
    return success;
}

static int run_test_case(const char *test_name, const int *data, size_t size,
                         size_t memory_limit, size_t merge_order)
{
    FILE *input;
    int *expected = NULL;
    size_t i;
    int success = 1;
    SortStats stats;

    printf("=== Test: %s ===\n", test_name);
    input = fopen(TEST_INPUT_FILE, "wb");
    if (input == NULL) {
        perror(TEST_INPUT_FILE);
        return 0;
    }

    printf("Input:  [ ");
    for (i = 0; i < size; i++) {
        if (fprintf(input, "%d\n", data[i]) < 0) {
            success = 0;
        }
        printf("%d ", data[i]);
    }
    printf("]\n");
    if (fclose(input) != 0) {
        perror(TEST_INPUT_FILE);
        success = 0;
    }

    if (!success) {
        remove(TEST_INPUT_FILE);
        return 0;
    }

    if (size > 0) {
        expected = malloc(size * sizeof(*expected));
        if (expected == NULL) {
            fprintf(stderr, "Unable to allocate expected test output.\n");
            remove(TEST_INPUT_FILE);
            return 0;
        }
        memcpy(expected, data, size * sizeof(*expected));
        qsort(expected, size, sizeof(*expected), compare_ints);
    }

    success = external_sort(TEST_INPUT_FILE, TEST_OUTPUT_FILE,
                            memory_limit, merge_order, &stats);
    if (success) {
        if (!read_and_check_output(TEST_OUTPUT_FILE, expected, size)) {
            success = 0;
        }
    }

    printf("Result: %s\n", success ? "PASS" : "FAIL");
    print_stats(&stats);
    printf("\n");

    free(expected);
    remove(TEST_INPUT_FILE);
    remove(TEST_OUTPUT_FILE);
    return success;
}

static int run_demo(void)
{
    int passed = 0;
    int total = 0;
    const int t2[] = {9, 1};
    const int t3[] = {1, 2, 3, 4, 5, 6, 7};
    const int t4[] = {9, 8, 7, 6, 5, 4, 3, 2, 1};
    const int t5[] = {5, 5, 5, 5, 5, 5, 5, 5};
    const int t6[] = {4, -1, 0, -99, 15, -2, 0};
    const int t7[] = {14, 2, 9, 3, 7, 10, 1};
    const int t8[] = {22, -8, 4, 4, 0, 19, -15, 3, 99, 1,
                      12, -8, 6, 2, 7, 0, 31, -1, 5, 18,
                      11, 10, -4, 27, 8, 6, 14, -22, 13, 2, 17};

    printf("External Merge Sort Simulator: M = %d, K = %d\n\n",
           DEFAULT_M, DEFAULT_K);

#define RUN_TEST(name, data, count) do { \
    total++; \
    if (run_test_case((name), (data), (count), DEFAULT_M, DEFAULT_K)) passed++; \
} while (0)

    RUN_TEST("1. Empty file", NULL, 0);
    RUN_TEST("2. Input smaller than RAM buffer", t2, sizeof(t2) / sizeof(t2[0]));
    RUN_TEST("3. Already sorted data", t3, sizeof(t3) / sizeof(t3[0]));
    RUN_TEST("4. Reverse-sorted data", t4, sizeof(t4) / sizeof(t4[0]));
    RUN_TEST("5. All duplicate values", t5, sizeof(t5) / sizeof(t5[0]));
    RUN_TEST("6. Negative values and zeros", t6, sizeof(t6) / sizeof(t6[0]));
    RUN_TEST("7. Final chunk is incomplete", t7, sizeof(t7) / sizeof(t7[0]));
    RUN_TEST("8. Larger mixed input across multiple merge passes",
             t8, sizeof(t8) / sizeof(t8[0]));

#undef RUN_TEST

    printf("Demo summary: %d/%d tests passed.\n", passed, total);
    return passed == total ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char *argv[])
{
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "--demo") == 0)) {
        return run_demo();
    }

    if (argc >= 3 && argc <= 5) {
        size_t memory_limit = DEFAULT_M;
        size_t merge_order = DEFAULT_K;
        SortStats stats;

        if (argc >= 4 && !parse_positive_size(argv[3], &memory_limit)) {
            fprintf(stderr, "Invalid M: expected a positive integer.\n");
            return EXIT_FAILURE;
        }
        if (argc >= 5 && !parse_positive_size(argv[4], &merge_order)) {
            fprintf(stderr, "Invalid K: expected an integer of at least 2.\n");
            return EXIT_FAILURE;
        }
        if (merge_order < 2) {
            fprintf(stderr, "K must be at least 2.\n");
            return EXIT_FAILURE;
        }

        if (!external_sort(argv[1], argv[2], memory_limit, merge_order, &stats)) {
            fprintf(stderr, "External sort failed.\n");
            return EXIT_FAILURE;
        }

        printf("Sorted '%s' into '%s'.\n", argv[1], argv[2]);
        print_stats(&stats);
        return EXIT_SUCCESS;
    }

    fprintf(stderr,
            "Usage: %s <input_file> <output_file> [M] [K]\n"
            "       %s --demo\n"
            "M is the in-memory chunk capacity; K is the merge order (K >= 2).\n",
            argv[0], argv[0]);
    return EXIT_FAILURE;
}
