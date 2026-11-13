# libmemctl — Benchmark Results

## 1. Overview

This document records the performance and correctness results of the libmemctl memory allocator.

The benchmark measures allocation performance using single-threaded and multithreaded workloads.

## 2. Test Environment

| Component | Configuration |
|---|---|
| Operating System | Kali Linux |
| Architecture | x86_64 |
| Compiler | GCC 14.2.0 |
| Kernel | 6.12.25-amd64 |
| CPU | 11th Gen Intel Core i5-1135G7 @ 2.40 GHz |
| Virtualization | VirtualBox |
| CPUs assigned to VM | 2 |
| Optimization | -O2 |
| Language Standard | C11 |

Note: CPU information represents the configuration visible inside the virtual machine.

## 3. Benchmark Methodology

The benchmark uses a mixed allocation workload.

### Single-threaded workload

- 1 thread.
- 20,000 operations.
- 48 allocation slots.
- Mixed allocation, deallocation, and reallocation operations.

### Multithreaded workload

- 8 threads.
- 160,000 total operations.
- 48 allocation slots per thread.
- Concurrent allocation, deallocation, and reallocation operations.

The benchmark uses `CLOCK_MONOTONIC` to measure elapsed time.

Throughput is calculated as:

Operations per second = Total operations / Elapsed time.

The benchmark also checks data integrity and reports errors.

## 4. Latest Benchmark Results

Results from the latest clean build and benchmark run:

| Metric | 1 Thread | 8 Threads |
|---|---:|---:|
| Total operations | 20,000 | 160,000 |
| Elapsed time | 0.005367 seconds | 0.039408 seconds |
| Throughput | 3,726,466 ops/s | 4,060,069 ops/s |
| Errors | 0 | 0 |
| Result | PASS | PASS |

## 5. Correctness Test Results

Command executed:

`make clean && make check && make benchmark`

Results:

- Basic allocation, free, and alignment: PASS
- Calloc zeroing and multiplication overflow: PASS
- Realloc growth, shrink, and failure preservation: PASS
- Allocation boundary and neighboring-block integrity: PASS
- Mixed-size fragmentation and reuse stress: PASS
- Multithreaded allocation contention: PASS

**Total: 6 passed, 0 failed.**

The allocator symbol check also passed.

## 6. Performance Observations

- Both benchmark workloads completed successfully.
- No benchmark errors were reported.
- The eight-thread workload achieved approximately 4.06 million operations per second in the latest run.
- Performance can vary between runs due to scheduling and virtual-machine overhead.
- The VM has two assigned CPUs, so the results should not be interpreted as eight-core scaling.

The multithreaded timing includes thread creation and joining, so it also reflects some thread-management overhead.

## 7. Reproducibility

Run the following command from the project root:

`make clean && make check && make benchmark`

This rebuilds the allocator, executes correctness tests, checks allocator symbols, and runs both benchmark workloads.

## 8. Conclusion

The latest clean verification completed all six correctness tests successfully. Both benchmark workloads also completed with zero errors.

These results provide a reproducible baseline for evaluating future allocator improvements.

