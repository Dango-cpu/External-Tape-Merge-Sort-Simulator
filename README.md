# External Tape Merge Sort Simulator

A C11 project that demonstrates **external merge sorting** for integer files that are too large to sort in one in-memory operation. The program reads a limited number of values at a time, sorts each chunk, stores the sorted chunks as temporary runs, and merges those runs using a min-heap.

The project also records the number of processing passes and the amount of logical data read and written. Different merge orders (`K`) can be tested to see how they affect the number of passes, I/O volume, and runtime.

## Project features

- External sorting with a configurable in-memory buffer size (`M`).
- Initial run generation using `qsort()`.
- Configurable K-way merging using a min-heap (`K` must be at least 2).
- Command-line sorting: input file, output file, `M`, and `K` can be supplied by the user.
- Input validation for malformed integers, values outside the C `int` range, overlong tokens, and embedded NUL bytes.
- Error handling for memory allocation and file operations, with temporary-file cleanup on tested failure paths.
- In-place sorting, where the input and output paths are the same.
- Built-in correctness tests, stress tests, and benchmark mode.
- Statistics for initial runs, merge passes, total processing passes, bytes read, bytes written, and total logical data I/O.

## How the algorithm works

1. **Read a chunk:** Read up to `M` integer values from the input file.
2. **Sort the chunk:** Sort the values in memory.
3. **Write an initial run:** Write the sorted values to a temporary run file.
4. **Merge runs:** Read up to `K` runs at a time. A min-heap keeps the smallest current value from each run. The smallest value is written to the output run, and the next value from that same input run is added to the heap.
5. **Repeat merge passes:** Continue merging groups of runs until only one sorted run remains. That run becomes the requested output file.

The program uses bounded chunks for run generation, so it does not need to load the entire input file into memory at once. `M` controls the chunk size, while `K` controls how many runs are merged in one group.

## Requirements

- GCC or Clang with C11 support.
- A command-line terminal.
- The standard C library.

The final Week 4 source was verified with GCC and Clang using strict warning options. Windows/MSVC is **not verified** in the recorded tests.

## Build

After adding `stress_test_week4_fixed.c` to the repository, compile it with GCC:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -O2 stress_test_week4_fixed.c -lm -o external_sort
```

Or compile with Clang:

```bash
clang -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -O2 stress_test_week4_fixed.c -lm -o external_sort
```

These commands create an executable named `external_sort`.

## Usage

### Sort an input file

```bash
./external_sort INPUT OUTPUT [M [K]]
```

Example:

```bash
./external_sort input.txt output.txt 1000 10
```

| Argument | Meaning | Default / rule |
|---|---|---|
| `INPUT` | File containing integer values to sort | Required |
| `OUTPUT` | Destination for the sorted values | Required |
| `M` | Maximum number of values held in one in-memory chunk | `1000` by default; must be positive |
| `K` | Maximum number of runs merged in one group | `2` by default; must be at least `2` |

Input values may be separated by spaces, tabs, or newlines. Each token must be a valid base-10 integer in the supported `int` range. The sorted output contains one integer per line.

On Windows, run the executable as `external_sort.exe` and use the same arguments after the executable name.

### Run the built-in tests

```bash
./external_sort --test
```

Runs the built-in correctness test suite. The final Week 4 version passed **12/12 tests**.

### Run the stress tests

```bash
./external_sort --stress
```

Runs malformed-input and failure-path checks. The final Week 4 version passed **4/4 stress tests**. The command returns a non-zero exit code if a stress test fails or cannot run.

### Run the benchmarks

```bash
./external_sort --benchmark
```

Running the program without arguments also starts the default benchmark:

```bash
./external_sort
```

The benchmark uses `M = 1,000`, input sizes of 5,000, 20,000, 100,000, and 1,000,000 records, and merge orders `K = 2, 3, 5, 10`.

## Final verification results

The following results are from the Week 4 final verification of `stress_test_week4_fixed.c`:

| Check | Result |
|---|---:|
| Built-in correctness tests | **12/12 passed** |
| Stress tests | **4/4 passed** |
| Randomized differential tests against Python `sorted()` | **35/35 passed** |
| GCC strict-warning build | Passed without warnings |
| Clang strict-warning build | Passed without warnings |
| GCC AddressSanitizer and UndefinedBehaviorSanitizer | Passed; no sanitizer errors reported |
| In-place sorting | Passed |
| Temporary-file cleanup after tested input failure | Passed |
| Benchmark pass-count comparison | **48/48 executions matched** theoretical pass counts |

The benchmark configurations were repeated three times. All 16 configurations produced the same logical I/O totals across their repetitions, and actual total pass counts matched the theoretical counts in all 48 executions.

## Benchmark results

`M = 1,000` for all rows. Each cell shows **total passes / total logical I/O / median CPU time**. Times are in milliseconds and are the median of three local runs.

| Input records (`N`) | `K = 2` | `K = 3` | `K = 5` | `K = 10` |
|---:|---:|---:|---:|---:|
| 5,000 | 4 / 0.237 MiB / 4.24 ms | 3 / 0.197 MiB / 2.92 ms | 2 / 0.131 MiB / 2.33 ms | 2 / 0.131 MiB / 2.06 ms |
| 20,000 | 6 / 1.471 MiB / 19.74 ms | 4 / 1.025 MiB / 14.46 ms | 3 / 0.788 MiB / 12.17 ms | 3 / 0.788 MiB / 10.61 ms |
| 100,000 | 8 / 10.354 MiB / 115.62 ms | 6 / 7.608 MiB / 87.38 ms | 4 / 5.256 MiB / 66.01 ms | 3 / 3.942 MiB / 57.41 ms |
| 1,000,000 | 11 / 144.317 MiB / 1531.60 ms | 8 / 104.704 MiB / 1163.09 ms | 6 / 78.833 MiB / 902.08 ms | 4 / 52.556 MiB / 686.89 ms |

### What the results show

- Increasing `K` reduced or kept the same number of total passes for all tested input sizes.
- Total logical data I/O also decreased or stayed the same as `K` increased.
- For 1,000,000 records, `K = 10` used 4 total passes and 52.556 MiB of logical I/O. With `K = 2`, it used 11 passes and 144.317 MiB.
- The recorded CPU time was lower for larger `K` in these tests. The best setting can still depend on the computer, memory, storage, and file-system limits.

## Understanding the reported measurements

- **Initial runs:** Number of sorted temporary runs created from the input file.
- **Merge passes:** Number of passes used to combine the initial runs.
- **Total processing passes:** Initial run-generation pass plus merge passes.
- **Bytes read / bytes written:** Logical text bytes consumed and emitted by the program.
- **Total data I/O:** Bytes read plus bytes written, displayed in bytes or MiB.
- **Runtime:** Measured with C `clock()` and reported as CPU time, not wall-clock time.

The I/O counters do not measure physical disk traffic. They do not include device-level effects such as filesystem caching or storage-controller behavior. Text-mode newline translation can also make on-disk byte counts platform-dependent. Benchmark timings may differ on another computer.

## Team members

| Student ID | Name | Main contributions | Work share |
|---|---|---|---:|
| 23110040 | Phạm Gia Minh | Run generation and merge implementation, input parsing and comparator fixes, dynamic run tracking, memory/file error handling, and temporary-file cleanup. | 50% |
| 25110043 | Đức Minh | Command-line interface, correctness and stress tests, benchmark execution, randomized testing, compiler/sanitizer checks, and final verification. | 50% |

## Project status

**Final status: Complete for the implementation and verification scope tested in Week 4.** The final test, stress, randomized, compiler, sanitizer, cleanup, and benchmark checks listed above passed.

Known limitations and handoff notes:

- Windows/MSVC compatibility was not tested, so it should not be considered verified.
- Concurrent execution of multiple copies of the program was not tested. More collision-resistant temporary-file naming could be considered if concurrent runs are required.
- The Week 4 corrected source and final report were local deliverables when this README was prepared and had not yet been pushed to GitHub. Upload `stress_test_week4_fixed.c` before relying on the build commands in this README.
- Benchmark comparisons should include the compiler, optimization flags, computer, CPU-time measurement method, and logical-byte I/O convention.

## Repository

[External Tape Merge Sort Simulator on GitHub](https://github.com/Dango-cpu/External-Tape-Merge-Sort-Simulator)
