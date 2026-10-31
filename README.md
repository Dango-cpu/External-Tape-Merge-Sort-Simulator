# External Tape Merge Sort Simulator

This is a C11 project for sorting integer files that do not need to fit entirely in main memory. It reads the input in chunks, sorts each chunk, saves the sorted chunks as temporary run files, and combines those runs with a min-heap-based K-way merge.

The program reports processing passes and logical data I/O. The user can change the in-memory chunk size (`M`) and merge order (`K`) to compare their effect on the sort.

> **Algorithm scope:** The final Week 4 implementation uses **grouped K-way multi-pass external merge sort**. Each temporary run is read as a stream during merging. This version does **not** implement the classical polyphase run-distribution schedule or a separate tape-device rotation model. The GitHub repository description should use the same wording. If a course requirement specifically asks for true polyphase merging, that requirement needs a separate implementation and has not been verified by this source.

> **Final source file:** The final Week 4 source is `stress_test_week4_fixed.c`. The public `main` branch did not include this file when this README was updated. Add the source file to the repository together with this README before trying to build it from a fresh clone.

## Features

- External sorting with a configurable in-memory buffer size (`M`).
- Initial run generation using `qsort()`.
- Configurable K-way merging using a min-heap (`K` must be at least 2).
- Command-line sorting with input/output paths and optional `M` and `K` arguments.
- Strict integer parsing, including rejection of malformed values, out-of-range values, overlong tokens, and embedded NUL bytes.
- Temporary files are created with exclusive-create mode and unique names. A name collision is retried instead of truncating an existing file.
- The final result is written to a temporary file beside the destination. The destination is replaced only after all writes and the close operation succeed.
- Existing output files are preserved when a sort fails before the final replacement.
- Built-in correctness tests, stress tests, and benchmark mode.
- Statistics for initial runs, merge passes, total processing passes, bytes read, bytes written, total logical data I/O, and CPU time.

## How the algorithm works

1. Read up to `M` integer values from the input file.
2. Validate each value and sort the chunk in memory.
3. Write the sorted chunk to a uniquely created temporary run file.
4. Open up to `K` run files. A min-heap stores the current smallest value from each run.
5. Write the smallest value to a new temporary run, then read the next value from the same source run and insert it into the heap.
6. Repeat the merge passes until only one run remains. Replace the requested output file with that completed run.

The input is processed in bounded chunks, so the whole data set is not loaded into memory at once. `M` controls the chunk size and `K` controls the number of runs merged in one group.

## Requirements

- GCC or Clang with C11 support.
- A command-line terminal.
- The standard C library.

The final source was compiled and tested with GCC and Clang. Windows/MSVC compatibility is not verified in the recorded tests.

## Build

After adding `stress_test_week4_fixed.c` to the repository, compile with GCC:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -O2 stress_test_week4_fixed.c -o external_sort
```

Or compile with Clang:

```bash
clang -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -O2 stress_test_week4_fixed.c -o external_sort
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
| `OUTPUT` | Destination for sorted values | Required |
| `M` | Maximum values held in one in-memory chunk | `1000`; must be positive |
| `K` | Maximum runs merged in one group | `2`; must be at least `2` |

Input values can be separated by spaces, tabs, or newlines. Every token must be a valid base-10 integer in the supported C `int` range. The output contains one integer per line.

If a Windows executable has been built successfully, run it as `external_sort.exe` with the same arguments. The Windows build path has not been tested for this release.

### Run the built-in correctness tests

```bash
./external_sort --test
```

The updated source passes **13/13 built-in tests**. The suite includes a check that a failed input does not overwrite an existing destination file.

### Run the stress tests

```bash
./external_sort --stress
```

On POSIX systems that support `RLIMIT_FSIZE`, the updated source passes **6/6 stress tests**. The suite covers malformed and overlong input, embedded NUL bytes, destination replacement failure, preservation of an existing output after an input error, and preservation of an existing output when the final merge write is forced to fail. On platforms without that file-size-limit facility, the portable suite contains five tests. The command returns a non-zero exit code when a test fails.

### Run the benchmarks

```bash
./external_sort --benchmark
```

Running the program without arguments also starts the benchmark:

```bash
./external_sort
```

The benchmark uses `M = 1,000`, input sizes of 5,000, 20,000, 100,000, and 1,000,000 records, and merge orders `K = 2, 3, 5, 10`. Benchmark/test fixture files are uniquely created and removed by the current program invocation; fixed filenames such as `benchmark_input.txt` and `stress_valid.txt` are not used.

## Final audit results

Results from the updated source after the Week 4 fixes:

| Check | Result |
|---|---:|
| Built-in correctness tests | **13/13 passed** |
| Stress tests | **6/6 passed on the tested POSIX environment** |
| Additional randomized comparisons against Python `sorted()` | **60/60 passed in the latest verification run** |
| GCC strict-warning build | Passed without warnings |
| Clang strict-warning build | Passed without warnings |
| GCC AddressSanitizer and UndefinedBehaviorSanitizer | Passed; no sanitizer errors reported |
| GCC static analyzer | No diagnostics |
| Clang static analyzer | No diagnostics |
| In-place sorting | Passed |
| Forced final-merge write failure (`RLIMIT_FSIZE`) | Existing output file preserved; incomplete merge file removed |
| Pre-existing conventional test/benchmark filenames | Preserved |
| Four simultaneous sorts to separate destinations | Passed |
| Four simultaneous sorts to one shared destination | Final file was one complete sorted result |
| Benchmark pass-count comparison | All 16 configurations matched theoretical pass counts |

The shared-destination concurrency check confirms atomic replacement on the tested POSIX environment: if several successful jobs target the same destination, the last completed replacement determines which complete sorted result remains. Do not use the same destination when each job's distinct result must be retained.

## Benchmark results

`M = 1,000` for all rows. Each cell shows **total passes / total logical I/O / median CPU time**. Times are milliseconds and are the medians of three local runs recorded during Week 4. They are machine-dependent and should not be treated as universal performance figures.

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

## Measurement definitions

- **Initial runs:** Number of sorted temporary runs created from the input file.
- **Merge passes:** Number of passes used to combine the initial runs.
- **Total processing passes:** Initial run-generation pass plus merge passes.
- **Bytes read / bytes written:** Logical text bytes consumed and emitted by the program.
- **Total data I/O:** Bytes read plus bytes written, displayed in bytes or MiB.
- **Runtime:** Measured with C `clock()` and reported as CPU time, not wall-clock time.

The I/O counters do not measure physical disk traffic. They do not include device-level effects such as filesystem caching or storage-controller behavior. Text-mode newline translation can also make on-disk byte counts platform-dependent. Benchmark timings may differ on another computer.

## Temporary files and output safety

Temporary files are created in the destination directory using C11 exclusive-create mode (`fopen` with `"wx"`). This prevents the program from opening an existing path by truncating it. The completed final output is kept separate from the requested destination until the write and close operations succeed. It is then installed with a same-directory replacement operation (`rename` on POSIX; `MoveFileEx` with replacement flags on Windows).

The audit forced a final-merge write failure using a file-size limit and confirmed that a pre-existing destination file remained unchanged. Temporary-file cleanup failures are reported with a warning and the affected path.

## Team members

| Student ID | Name | Main contributions | Work share |
|---|---|---|---:|
| 23110040 | Phạm Gia Minh | Run generation and merge implementation, input parsing and comparator fixes, dynamic run tracking, memory/file error handling, temporary-file ownership, and output-safety improvements. | 50% |
| 25110043 | Đức Minh | Command-line interface, correctness and stress tests, benchmark execution, randomized testing, compiler/sanitizer checks, and final verification. | 50% |

## Project status

**Final status: Complete for the grouped K-way external merge implementation and the verification scope listed above.** The Week 4 code fixes the output-file data-loss bug, uses exclusive temporary-file creation, isolates test/benchmark fixtures, and reports cleanup failures.

Known limits and handoff notes:

- Windows/MSVC has not been verified, even though a Windows-specific replacement branch is included.
- The program implements grouped K-way multi-pass merging, not a classical polyphase run-distribution algorithm or a distinct tape-device simulation. Check the course specification before final submission if polyphase merging or explicit tape simulation is mandatory.
- If multiple processes write to the same output path, the last successful atomic replacement wins. Use different destination paths to retain each result.
- As of 31 October 2026, the public `main` branch did not yet contain `stress_test_week4_fixed.c`. Upload this source file together with this `README.md` before expecting a fresh clone to build the final version. After publishing, remove or revise this handoff note.

## Repository

[External Tape Merge Sort Simulator on GitHub](https://github.com/Dango-cpu/External-Tape-Merge-Sort-Simulator)

**Suggested GitHub About description** (replace the current polyphase/tape-simulation claim with this wording):

> C11 external sorting simulator using bounded-memory run generation, disk-based temporary runs, configurable grouped K-way multi-pass merging, and I/O/pass performance analysis. This project does not implement classical polyphase run distribution or a physical tape-device model.
