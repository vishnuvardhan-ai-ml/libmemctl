# libmemctl — High-Throughput Thread-Safe Memory Allocator

A custom thread-safe memory allocator implemented in C11 for Linux, using `mmap()` and `munmap()` for memory management.

## Overview

`libmemctl` provides a custom allocation API with three allocation tiers, designed to support different allocation sizes while enabling concurrent access from multiple threads.

## Architecture

1. Small Allocations (up to 1 KB)

- Size-class slab allocator.
- Twelve size classes: 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, and 1024 bytes.
- Reusable slots within approximately 64 KB memory slabs.
- Per-size-class mutexes for synchronization.

2. Medium Allocations (1,025–65,536 bytes)

- Reusable memory regions (approximately 1 MB each).
- Six segregated free bins.
- Block splitting and bidirectional coalescing.
- Boundary tags for free-block management.
- Dedicated medium allocator mutex.

3. Large Allocations (above 65,536 bytes)

- Dedicated anonymous memory mappings.
- Memory returned to the operating system using `munmap()`.

## Technical Stack

- Language: C11
- Platform: Linux
- Concurrency: POSIX Threads (pthreads)
- Memory Management: `mmap()` / `munmap()`
- Build System: GNU Make
- Testing: Custom test runner, AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan), ThreadSanitizer (TSan)

## Public API

void *my_malloc(size_t size);
void my_free(void *ptr);
void *my_realloc(void *ptr, size_t size);
void *my_calloc(size_t nmemb, size_t size);

## Project Structure

libmemctl/
├── include/
│   └── libmemctl.h
├── src/
│   └── allocator.c
├── tests/
│   └── test_runner.c
├── benchmarks/
│   └── allocator_bench.c
├── results/
├── Makefile
├── .gitignore
├── README.md
├── DESIGN.md
├── BENCHMARKS.md
└── problem.md

## Build and Run

### Requirements

- GCC
- GNU Make
- Linux
- POSIX Threads support

### Build and Run the Test Suite

make clean
make check

### Run the Standalone Benchmark

make benchmark

## Testing Results

The latest clean verification completed all six tests successfully.

| Test | Result |
|---|---|
| Basic allocation, free and alignment | PASS |
| Calloc and multiplication overflow | PASS |
| Realloc and failure preservation | PASS |
| Allocation boundary integrity | PASS |
| Fragmentation and reuse stress | PASS |
| Multithreaded allocation contention | PASS |

Summary: 6 passed, 0 failed.

The allocator symbol check also passed.

## Sanitizer Verification

- AddressSanitizer (ASan): All 6 tests passed.
- UndefinedBehaviorSanitizer (UBSan): All 6 tests passed.
- ThreadSanitizer (TSan): All 6 tests passed; no race warnings appeared in the reported run.

## Latest Standalone Benchmark

Results from the latest clean benchmark run:

| Metric | 1 Thread | 8 Threads |
|---|---:|---:|
| Operations | 20,000 | 160,000 |
| Elapsed time | 0.005419 seconds | 0.035041 seconds |
| Throughput | 3,690,739 ops/s | 4,566,074 ops/s |
| Errors | 0 | 0 |
| Result | PASS | PASS |

The benchmark uses a mixed allocation, deallocation, and reallocation workload.

Environment: Kali Linux, x86_64, GCC 14.2.0, Intel Core i5-1135G7, VirtualBox VM with 2 assigned CPUs.

Benchmark results are measurements from the project's test environment, not a standardized comparison against other allocators. Multithreaded timing includes thread creation and joining overhead.

See [BENCHMARKS.md](BENCHMARKS.md) for methodology and detailed results.

## Design Highlights

- Three-tier allocation architecture: Separate strategies for small, medium, and large allocations.
- Fine-grained concurrency: Per-class mutexes for small allocations and a dedicated mutex for medium allocations.
- Atomic metadata: Relevant metadata fields use atomic operations for synchronized access.
- Memory reuse: LIFO freelists for small slabs and six segregated bins for medium chunks.
- Boundary tags: Bidirectional coalescing and chunk splitting.
- Defensive integrity: Overflow-safe size arithmetic, `max_align_t` alignment, and failure preservation semantics on `my_realloc()`.

See [DESIGN.md](DESIGN.md) for the complete allocator design.

## License

Add the license required by your hackathon or project distribution terms before publishing.
