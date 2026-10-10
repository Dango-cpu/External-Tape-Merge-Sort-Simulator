#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// Lớp thống kê chi tiết truy xuất đĩa (I/O) và số lượt Pass[cite: 1, 2]
typedef struct {
    long long bytes_read;
    long long bytes_written;
    int passes;
} IOStats;

void log_read(IOStats *stats, size_t size_bytes) { stats->bytes_read += size_bytes; }
void log_write(IOStats *stats, size_t size_bytes) { stats->bytes_written += size_bytes; }
void reset_stats(IOStats *stats) {
    stats->bytes_read = 0;
    stats->bytes_written = 0;
    stats->passes = 0;
}

// Cấu trúc nút cho Min-Heap phục vụ quá trình trộn k-way[cite: 1, 2]
typedef struct {
    int value;
    int file_index;
} HeapNode;

typedef struct {
    HeapNode *arr;
    int size;
    int capacity;
} MinHeap;

void swap(HeapNode *a, HeapNode *b) {
    HeapNode temp = *a;
    *a = *b;
    *b = temp;
}

void heapify(MinHeap *heap, int idx) {
    int smallest = idx;
    int left = 2 * idx + 1;
    int right = 2 * idx + 2;

    if (left < heap->size && heap->arr[left].value < heap->arr[smallest].value)
        smallest = left;
    if (right < heap->size && heap->arr[right].value < heap->arr[smallest].value)
        smallest = right;
    if (smallest != idx) {
        swap(&heap->arr[idx], &heap->arr[smallest]);
        heapify(heap, smallest);
    }
}

// Pass 0: Đọc từng chunk M bản ghi, sắp xếp nội, ghi thành tệp run ban đầu[cite: 2]
int compare_ints(const void *a, const void *b) {
    return (*(int *)a - *(int *)b);
}

int create_initial_runs(const char *input_file, int buffer_size, IOStats *stats, char runs[][256]) {
    FILE *in = fopen(input_file, "r");
    if (!in) return 0;

    int *buffer = (int *)malloc(buffer_size * sizeof(int));
    int run_counter = 0;
    char line[256];

    while (1) {
        int count = 0;
        while (count < buffer_size && fgets(line, sizeof(line), in)) {
            log_read(stats, strlen(line)); // Ghi nhận số bytes đọc[cite: 2]
            buffer[count++] = atoi(line);
        }

        if (count == 0) break;

        // Sắp xếp nội chunk[cite: 1, 2]
        qsort(buffer, count, sizeof(int), compare_ints);

        sprintf(runs[run_counter], "temp_run/run_0_%d.txt", run_counter);
        FILE *out = fopen(runs[run_counter], "w");
        for (int i = 0; i < count; i++) {
            char out_line[256];
            int len = sprintf(out_line, "%d\n", buffer[i]);
            fputs(out_line, out);
            log_write(stats, len); // Ghi nhận số bytes ghi[cite: 2]
        }
        fclose(out);
        run_counter++;
    }

    free(buffer);
    fclose(in);
    stats->passes += 1; 
    return run_counter;
}

// Trộn tối đa k tệp run thành 1 tệp run hợp nhất sử dụng Min-Heap[cite: 2]
void merge_k_runs(char input_runs[][256], int k, const char *output_path, IOStats *stats) {
    FILE **open_files = malloc(k * sizeof(FILE *));
    MinHeap heap;
    heap.arr = malloc(k * sizeof(HeapNode));
    heap.size = 0;
    heap.capacity = k;

    char line[256];
    for (int i = 0; i < k; i++) {
        open_files[i] = fopen(input_runs[i], "r");
        if (open_files[i] && fgets(line, sizeof(line), open_files[i])) {
            log_read(stats, strlen(line));
            heap.arr[heap.size].value = atoi(line);
            heap.arr[heap.size].file_index = i;
            heap.size++;
        }
    }

    for (int i = (heap.size - 1) / 2; i >= 0; i--) {
        heapify(&heap, i);
    }

    FILE *out_f = fopen(output_path, "w");
    while (heap.size > 0) {
        HeapNode root = heap.arr[0];
        
        char out_line[256];
        int len = sprintf(out_line, "%d\n", root.value);
        fputs(out_line, out_f);
        log_write(stats, len); // Ghi nhận I/O khi ghi dữ liệu trộn[cite: 2]

        int f_idx = root.file_index;
        if (fgets(line, sizeof(line), open_files[f_idx])) {
            log_read(stats, strlen(line));
            heap.arr[0].value = atoi(line);
            heap.arr[0].file_index = f_idx;
        } else {
            heap.arr[0] = heap.arr[heap.size - 1];
            heap.size--;
        }
        heapify(&heap, 0);
    }

    fclose(out_f);
    for (int i = 0; i < k; i++) {
        if (open_files[i]) fclose(open_files[i]);
    }
    free(open_files);
    free(heap.arr);
}
