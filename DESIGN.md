# libmemctl — Allocator Design

## 1. Overview

libmemctl is a custom memory allocator implemented in C. It provides thread-safe implementations of:

- `my_malloc()`
- `my_free()`
- `my_calloc()`
- `my_realloc()`

The allocator uses a three-tier memory management strategy to handle small, medium, and large allocations efficiently.

## 2. Memory Management Architecture

### Small Allocations (1–1024 bytes)

- Uses 12 predefined size classes.
- Size classes: 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, and 1024 bytes.
- Memory is organized into approximately 64 KB slabs.
- Each slab maintains a free list and bump-allocation position.
- Partial, full, and spare slabs are tracked separately.
- One empty spare slab is retained per size class.
- Additional empty slabs are unmapped to release memory.

### Medium Allocations (1025–65536 bytes)

- Uses approximately 1 MB memory regions.
- Maintains six segregated free bins.
- Uses boundary tags and footer metadata for block management.
- Supports block splitting when the remaining space is sufficient.
- Uses a minimum chunk size of 64 bytes.
- Adjacent free blocks are coalesced to reduce fragmentation.
- One empty spare region is retained.
- Additional empty regions are unmapped.

### Large Allocations (Above 65536 bytes)

- Each allocation receives a dedicated anonymous memory mapping.
- `mmap()` is used to obtain memory.
- `munmap()` releases the allocation when freed.

## 3. Metadata Layout

Each allocation uses an internal 32-byte allocation header.

The header stores:

- Allocation magic value.
- Allocation kind.
- Allocation flags.
- Requested allocation size.
- Total allocation size.
- Container information.

Atomic fields are used for relevant metadata.

The allocator ensures `max_align_t` alignment for returned pointers.

## 4. Thread Safety

- Small allocation size classes use individual mutexes.
- Medium allocation management uses a global mutex.
- Large allocations use independent memory mappings.
- Synchronization protects allocator metadata and free-list operations.

## 5. Fragmentation Management

The allocator uses several strategies:

1. Size classes reduce internal fragmentation for small allocations.
2. Free lists allow reuse of previously released blocks.
3. Medium blocks are split when appropriate.
4. Boundary tags support adjacent-block coalescing.
5. Empty slabs and regions are retained selectively.
6. Dedicated mappings allow large allocations to be released independently.

## 6. Reallocation and Zero Initialization

### Reallocation

`my_realloc()` supports:

- In-place resizing when possible.
- Medium-block expansion into an adjacent free block when possible.
- Allocation and copying when in-place resizing is not possible.
- Preservation of the original allocation if a new allocation fails.

### Calloc

`my_calloc()`:

- Checks multiplication overflow.
- Allocates the requested memory.
- Initializes the allocated memory to zero.

## 7. Design Decisions and Trade-offs

### Decision 1: Three-tier allocation strategy

**Benefit:** Different allocation sizes use different memory-management strategies, avoiding a single general-purpose allocation path.

**Trade-off:** Maintaining separate small, medium, and large allocation systems increases implementation complexity.

### Decision 2: Per-size-class locking for small allocations

**Benefit:** Independent small allocation classes can operate with separate locks, reducing contention compared with one global lock.

**Trade-off:** Mutex management and slab bookkeeping add synchronization overhead.

### Decision 3: Retaining one spare slab or region

**Benefit:** A recently emptied allocation container can be reused without immediately requesting another mapping.

**Trade-off:** Retaining spare memory increases the allocator's memory footprint.

## 8. Safety and Correctness

The implementation includes:

- Allocation metadata validation.
- Integer-overflow checks.
- Maximum fundamental alignment.
- Reallocation failure preservation.
- Zero-initialized calloc allocations.
- Thread-safe allocator metadata operations.
- Tests for fragmentation, reuse, and concurrent allocation.

## 9. Conclusion

libmemctl combines size-class allocation, segregated free bins, boundary-tag coalescing, and dedicated large memory mappings in a thread-safe C memory allocator.
