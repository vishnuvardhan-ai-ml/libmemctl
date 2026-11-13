# libmemctl — High-Throughput Thread-Safe Memory Allocator

A custom thread-safe memory allocator implemented in C11 for Linux, using `mmap()` and `munmap()` for memory management.

## Overview

`libmemctl` provides a custom allocation API with three allocation tiers, designed to support different allocation sizes while enabling concurrent access from multiple threads.

## Architecture

### 1. Small Allocations (up to 1 KB)
- Size-class slab allocator.
- Multiple size classes: 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, and 1024 bytes.
- Reusable slots within memory slabs.
- Per-size-class mutexes for synchronization.

### 2. Medium Allocations (1,025–65,536 bytes)
- Reusable memory regions (1 MB each).
- Six segregated free bins.
- Block splitting and bidirectional coalescing.
- Boundary tags for free-block management.
- Dedicated medium allocator mutex.

### 3. Large Allocations (above 65,536 bytes)
- Dedicated anonymous memory mappings.
- Memory returned to the operating system using `munmap()`.

## Technical Stack

- **Language**: C11
- **Platform**: Linux
- **Concurrency**: POSIX Threads (pthreads)
- **Memory Management**: `mmap()` / `munmap()`
- **Build System**: GNU Make
- **Testing**: Custom test runner, AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan), ThreadSanitizer (TSan)

## Public API

```c
void *my_malloc(size_t size);
void my_free(void *ptr);
void *my_realloc(void *ptr, size_t size);
void *my_calloc(size_t nmemb, size_t size);
```

## Project Structure

```text
libmemctl/
├── include/
│   └── libmemctl.h
├── src/
│   └── allocator.c
├── tests/
│   └── test_runner.c
├── Makefile
├── .gitignore
├── README.md
└── problem.md
```

## Build and Run

### Requirements:
- GCC
- GNU Make
- Linux
- POSIX Threads support

### Build and Run the Test Suite:

```bash
make clean
make check
```

## Testing Results

The following results were obtained from the project's test runner:

| Test | Result |
| :--- | :--- |
| Basic allocation, free and alignment | **PASS** |
| Calloc and multiplication overflow | **PASS** |
| Realloc and failure preservation | **PASS** |
| Allocation boundary integrity | **PASS** |
| Fragmentation and reuse stress | **PASS** |
| Multithreaded allocation contention | **PASS** |

### Sanitizer Verification

- **AddressSanitizer (ASan)**: All 6 tests passed.
- **UndefinedBehaviorSanitizer (UBSan)**: All 6 tests passed.
- **ThreadSanitizer (TSan)**: All 6 tests passed; no race warnings appeared in the reported run.

### Recorded Throughput

A normal test run completed 160,000 threaded operations in approximately 0.039 seconds, equivalent to approximately **4.07 million operations per second**.

> *Note: This is a project test-run measurement, not a standardized comparison against other allocators.*

## Design Highlights

- **Three-tier allocation architecture** tailored for distinct size distributions.
- **Fine-grained concurrency**: Per-class mutexes for small allocations and a dedicated mutex for medium allocations minimize lock contention.
- **C11 atomic metadata synchronization**: Metadata fields (`magic`, `kind`, `flags`) use explicit atomic operations (`memory_order_acquire` / `memory_order_release`) for data-race-free dispatch and neighbor updates.
- **Memory reuse**: LIFO freelists for small slabs and 6 segregated bins for medium chunks.
- **Knuth boundary tags**: Bidirectional coalescing and chunk splitting without heap overhead.
- **Defensive integrity**: Overflow-safe size arithmetic, alignment guarantees to `_Alignof(max_align_t)`, and failure preservation semantics on `my_realloc()`.

## License

Add the license required by your hackathon or project distribution terms before publishing.
