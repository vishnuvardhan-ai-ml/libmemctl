#include "libmemctl.h"

#include <errno.h>
#include <pthread.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * libmemctl - Stage 4 Implementation (Corrected):
 * High-performance three-tier memory allocator with optimized in-place realloc
 * and concurrency/integrity hardening:
 *
 * 1. Small Tier (<= 1024 B): Size-class slabs with LIFO freelists.
 *    - In-place realloc when request fits within the existing slot capacity.
 * 2. Medium Tier (1025 B .. 65536 B): 1 MB reusable regions with 6 segregated
 *    free bins, Knuth boundary tags, chunk splitting, bidirectional coalescing,
 *    and spare region reclamation.
 *    - In-place realloc when capacity suffices or adjacent right free chunk
 *      can be consumed without invalidating boundary tags.
 * 3. Large Tier (> 65536 B): Dedicated anonymous page-aligned mmap/munmap.
 */

#define ALLOC_MAGIC_SMALL   0x534C4142U /* 'SLAB' */
#define ALLOC_MAGIC_MEDIUM  0x4D454449U /* 'MEDI' */
#define ALLOC_MAGIC_DIRECT  0x4D4D4150U /* 'MMAP' */
#define ALLOC_MAGIC_FREE    0x46524545U /* 'FREE' */

#define SLAB_MAGIC          0x534C424DU /* 'SLBM' */
#define REGION_MAGIC        0x52474E4DU /* 'RGNM' */

#define NUM_SLAB_CLASSES    12U
#define SLAB_MAX_PAYLOAD    1024U
#define SLAB_BASE_BYTES     65536U

#define MEDIUM_MIN_PAYLOAD  1025U
#define MEDIUM_MAX_PAYLOAD  65536U
#define REGION_BASE_BYTES   1048576U /* 1 MB */
#define NUM_MEDIUM_BINS     6U
#define MEDIUM_MIN_CHUNK    64U

#define CHUNK_FLAG_IN_USE    0x0001U
#define CHUNK_FLAG_PREV_FREE 0x0002U

typedef enum alloc_kind {
    ALLOC_KIND_SMALL  = 1,
    ALLOC_KIND_MEDIUM = 2,
    ALLOC_KIND_DIRECT = 3
} alloc_kind_t;

typedef struct alloc_header {
    uint32_t magic;          /* Validation tag: ALLOC_MAGIC_* */
    uint16_t kind;           /* Allocation tier: ALLOC_KIND_* */
    uint16_t flags;          /* Small: class_idx; Medium: CHUNK_FLAG_* */
    size_t requested_size;   /* User payload size in bytes */
    size_t total_size;       /* Gross slot, chunk, or mapping size in bytes */
    void *container;         /* Owning slab_t* (small), region_t* (medium), or NULL */
} alloc_header_t;

_Static_assert(sizeof(alloc_header_t) == 32U,
               "alloc_header_t must be exactly 32 bytes");
_Static_assert((sizeof(alloc_header_t) % _Alignof(max_align_t)) == 0U,
               "alloc_header_t size must be a multiple of _Alignof(max_align_t)");

/* --- Small Slab Allocator Structures --- */

typedef struct slab_slot_node {
    struct slab_slot_node *next;
} slab_slot_node_t;

typedef struct slab {
    struct slab *prev;
    struct slab *next;
    uint32_t magic;              /* SLAB_MAGIC */
    uint16_t class_idx;          /* Index 0..11 */
    uint16_t is_spare;           /* 1 if cached spare, 0 otherwise */
    size_t slot_size;            /* Gross slot size */
    size_t total_slots;          /* Max capacity of slots */
    size_t free_slots;           /* Available slots count */
    slab_slot_node_t *freelist;  /* Recycled free slots LIFO list */
    char *slot_start;            /* Address of first aligned slot */
    char *bump_ptr;              /* Next uncarved slot start */
    char *slot_limit;            /* Upper boundary of usable slots */
} slab_t;

typedef struct slab_class {
    pthread_mutex_t lock;
    size_t payload_size;
    size_t gross_slot_size;
    slab_t *partial_slabs;       /* Doubly linked list of slabs with free slots */
    slab_t *full_slabs;          /* Doubly linked list of completely full slabs */
    slab_t *spare_slab;          /* At most one cached empty slab */
} slab_class_t;

/* --- Medium Region Allocator Structures --- */

typedef struct medium_free_node {
    struct medium_free_node *next;
    struct medium_free_node *prev;
} medium_free_node_t;

typedef struct region {
    struct region *prev;
    struct region *next;
    uint32_t magic;              /* REGION_MAGIC */
    uint32_t is_spare;           /* 1 if cached spare, 0 otherwise */
    size_t total_mapping_bytes;  /* Mapping length */
    size_t active_allocations;   /* Count of allocated chunks in this region */
    char *usable_start;          /* First chunk start address (aligned to 16) */
    char *usable_end;            /* End boundary of chunks (aligned to 16) */
} region_t;

/* Global state */
static pthread_once_t s_page_size_once = PTHREAD_ONCE_INIT;
static size_t s_cached_page_size = 0U;

static const size_t s_slab_payload_sizes[NUM_SLAB_CLASSES] = {
    16U, 32U, 48U, 64U, 96U, 128U, 192U, 256U, 384U, 512U, 768U, 1024U
};

/* 12 size classes with statically initialized pthread mutexes */
static slab_class_t s_slab_classes[NUM_SLAB_CLASSES] = {
    { .lock = PTHREAD_MUTEX_INITIALIZER, .payload_size = 16U,   .gross_slot_size = 48U,   .partial_slabs = NULL, .full_slabs = NULL, .spare_slab = NULL },
    { .lock = PTHREAD_MUTEX_INITIALIZER, .payload_size = 32U,   .gross_slot_size = 64U,   .partial_slabs = NULL, .full_slabs = NULL, .spare_slab = NULL },
    { .lock = PTHREAD_MUTEX_INITIALIZER, .payload_size = 48U,   .gross_slot_size = 80U,   .partial_slabs = NULL, .full_slabs = NULL, .spare_slab = NULL },
    { .lock = PTHREAD_MUTEX_INITIALIZER, .payload_size = 64U,   .gross_slot_size = 96U,   .partial_slabs = NULL, .full_slabs = NULL, .spare_slab = NULL },
    { .lock = PTHREAD_MUTEX_INITIALIZER, .payload_size = 96U,   .gross_slot_size = 128U,  .partial_slabs = NULL, .full_slabs = NULL, .spare_slab = NULL },
    { .lock = PTHREAD_MUTEX_INITIALIZER, .payload_size = 128U,  .gross_slot_size = 160U,  .partial_slabs = NULL, .full_slabs = NULL, .spare_slab = NULL },
    { .lock = PTHREAD_MUTEX_INITIALIZER, .payload_size = 192U,  .gross_slot_size = 224U,  .partial_slabs = NULL, .full_slabs = NULL, .spare_slab = NULL },
    { .lock = PTHREAD_MUTEX_INITIALIZER, .payload_size = 256U,  .gross_slot_size = 288U,  .partial_slabs = NULL, .full_slabs = NULL, .spare_slab = NULL },
    { .lock = PTHREAD_MUTEX_INITIALIZER, .payload_size = 384U,  .gross_slot_size = 416U,  .partial_slabs = NULL, .full_slabs = NULL, .spare_slab = NULL },
    { .lock = PTHREAD_MUTEX_INITIALIZER, .payload_size = 512U,  .gross_slot_size = 544U,  .partial_slabs = NULL, .full_slabs = NULL, .spare_slab = NULL },
    { .lock = PTHREAD_MUTEX_INITIALIZER, .payload_size = 768U,  .gross_slot_size = 800U,  .partial_slabs = NULL, .full_slabs = NULL, .spare_slab = NULL },
    { .lock = PTHREAD_MUTEX_INITIALIZER, .payload_size = 1024U, .gross_slot_size = 1056U, .partial_slabs = NULL, .full_slabs = NULL, .spare_slab = NULL }
};

/* Medium allocator state protected by a statically initialized mutex */
static pthread_mutex_t s_medium_mutex = PTHREAD_MUTEX_INITIALIZER;
static medium_free_node_t *s_medium_bins[NUM_MEDIUM_BINS] = { NULL };
static region_t *s_active_regions = NULL;
static region_t *s_spare_region = NULL;

/* --- Forward Declarations --- */
static void init_page_size(void);
static size_t get_page_size(void);
static size_t get_slab_mapping_bytes(void);
static size_t get_region_mapping_bytes(void);
static int calculate_mapping_size(size_t payload_bytes, size_t *mapping_bytes);

static size_t find_slab_class(size_t size);
static void slab_list_insert(slab_t **head, slab_t *slab);
static void slab_list_remove(slab_t **head, slab_t *slab);
static slab_t *create_slab(size_t class_idx);
static void *slab_malloc(size_t size);
static void slab_free(alloc_header_t *header);
static void *small_realloc_in_place(alloc_header_t *header, size_t new_size);

static size_t get_medium_bin_index(size_t gross_size);
static medium_free_node_t *chunk_to_free_node(alloc_header_t *chunk);
static alloc_header_t *free_node_to_chunk(medium_free_node_t *node);
static void medium_bin_insert(alloc_header_t *chunk);
static void medium_bin_remove(alloc_header_t *chunk);
static void region_list_insert(region_t **head, region_t *region);
static void region_list_remove(region_t **head, region_t *region);
static region_t *create_region(void);
static void *medium_malloc(size_t size);
static void medium_free(alloc_header_t *header);
static void *medium_realloc_in_place(alloc_header_t *chunk, size_t new_size);

static void *direct_malloc(size_t size);
static void direct_free(alloc_header_t *header);

/* --- Page Size and Utility Functions --- */

static void init_page_size(void)
{
    const long sc = sysconf(_SC_PAGESIZE);
    if (sc > 0) {
        s_cached_page_size = (size_t)sc;
    }
}

static size_t get_page_size(void)
{
    if (pthread_once(&s_page_size_once, init_page_size) != 0 || s_cached_page_size == 0U) {
        const long sc = sysconf(_SC_PAGESIZE);
        return (sc > 0) ? (size_t)sc : 0U;
    }
    return s_cached_page_size;
}

static size_t get_slab_mapping_bytes(void)
{
    const size_t page_size = get_page_size();
    if (page_size > SLAB_BASE_BYTES) {
        return page_size;
    }
    return ((SLAB_BASE_BYTES + page_size - 1U) / page_size) * page_size;
}

static size_t get_region_mapping_bytes(void)
{
    const size_t page_size = get_page_size();
    if (page_size > REGION_BASE_BYTES) {
        return page_size;
    }
    return ((REGION_BASE_BYTES + page_size - 1U) / page_size) * page_size;
}

static int calculate_mapping_size(size_t payload_bytes, size_t *mapping_bytes)
{
    const size_t page_size = get_page_size();
    size_t total_bytes;

    if (mapping_bytes == NULL || page_size == 0U) {
        errno = EINVAL;
        return -1;
    }

    /* Check addition overflow for header + payload */
    if (payload_bytes > SIZE_MAX - sizeof(alloc_header_t)) {
        errno = ENOMEM;
        return -1;
    }
    total_bytes = sizeof(alloc_header_t) + payload_bytes;

    /* Check addition overflow for page rounding without assuming power-of-two */
    if (total_bytes > SIZE_MAX - (page_size - 1U)) {
        errno = ENOMEM;
        return -1;
    }

    /* Arithmetic page rounding (works for any positive page size) */
    *mapping_bytes = ((total_bytes + page_size - 1U) / page_size) * page_size;
    return 0;
}

/* --- Small Slab Allocator Implementation --- */

static size_t find_slab_class(size_t size)
{
    for (size_t i = 0U; i < NUM_SLAB_CLASSES; ++i) {
        if (size <= s_slab_payload_sizes[i]) {
            return i;
        }
    }
    return NUM_SLAB_CLASSES - 1U;
}

static void slab_list_insert(slab_t **head, slab_t *slab)
{
    slab->prev = NULL;
    slab->next = *head;
    if (*head != NULL) {
        (*head)->prev = slab;
    }
    *head = slab;
}

static void slab_list_remove(slab_t **head, slab_t *slab)
{
    if (slab->prev != NULL) {
        slab->prev->next = slab->next;
    } else if (*head == slab) {
        *head = slab->next;
    }
    if (slab->next != NULL) {
        slab->next->prev = slab->prev;
    }
    slab->prev = NULL;
    slab->next = NULL;
}

static slab_t *create_slab(size_t class_idx)
{
    const size_t mapping_bytes = get_slab_mapping_bytes();
    const size_t slot_size = s_slab_classes[class_idx].gross_slot_size;
    void *mapping;
    slab_t *slab;
    uintptr_t raw_first;
    uintptr_t align_mask;
    uintptr_t first_slot;
    size_t header_offset;
    size_t available_bytes;
    size_t total_slots;
    char *slot_start;
    char *slot_limit;

    mapping = mmap(NULL,
                   mapping_bytes,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1,
                   0);
    if (mapping == MAP_FAILED) {
        return NULL;
    }

    slab = (slab_t *)mapping;

    /* Explicitly align the first slot to _Alignof(max_align_t) */
    raw_first = (uintptr_t)mapping + sizeof(slab_t);
    align_mask = (uintptr_t)_Alignof(max_align_t) - 1U;
    first_slot = (raw_first + align_mask) & ~align_mask;
    header_offset = (size_t)(first_slot - (uintptr_t)mapping);

    if (mapping_bytes > header_offset) {
        available_bytes = mapping_bytes - header_offset;
    } else {
        available_bytes = 0U;
    }
    total_slots = available_bytes / slot_size;

    if (total_slots == 0U) {
        (void)munmap(mapping, mapping_bytes);
        errno = ENOMEM;
        return NULL;
    }

    slot_start = (char *)first_slot;
    slot_limit = slot_start + (total_slots * slot_size);

    /* Initialize all metadata before publishing */
    slab->prev = NULL;
    slab->next = NULL;
    slab->magic = SLAB_MAGIC;
    slab->class_idx = (uint16_t)class_idx;
    slab->is_spare = 0U;
    slab->slot_size = slot_size;
    slab->total_slots = total_slots;
    slab->free_slots = total_slots;
    slab->freelist = NULL;
    slab->slot_start = slot_start;
    slab->bump_ptr = slot_start;
    slab->slot_limit = slot_limit;

    return slab;
}

static void *slab_malloc(size_t size)
{
    const size_t payload_bytes = (size == 0U) ? 1U : size;
    const size_t class_idx = find_slab_class(payload_bytes);
    slab_class_t *sclass = &s_slab_classes[class_idx];
    slab_t *slab;
    void *slot;
    alloc_header_t *header;

    if (pthread_mutex_lock(&sclass->lock) != 0) {
        return NULL;
    }

    slab = sclass->partial_slabs;
    if (slab == NULL) {
        if (sclass->spare_slab != NULL) {
            slab = sclass->spare_slab;
            sclass->spare_slab = NULL;
            slab->is_spare = 0U;
            slab_list_insert(&sclass->partial_slabs, slab);
        } else {
            slab = create_slab(class_idx);
            if (slab == NULL) {
                (void)pthread_mutex_unlock(&sclass->lock);
                return NULL;
            }
            slab_list_insert(&sclass->partial_slabs, slab);
        }
    }

    /* Allocate slot: prefer recycled freelist, then bump pointer */
    if (slab->freelist != NULL) {
        slab_slot_node_t *node = slab->freelist;
        slab->freelist = node->next;
        slab->free_slots--;
        slot = (void *)node;
    } else if (slab->bump_ptr + slab->slot_size <= slab->slot_limit) {
        slot = (void *)slab->bump_ptr;
        slab->bump_ptr += slab->slot_size;
        slab->free_slots--;
    } else {
        /* Inconsistent state fallback */
        (void)pthread_mutex_unlock(&sclass->lock);
        return NULL;
    }

    /* If slab is now full, move from partial_slabs to full_slabs */
    if (slab->free_slots == 0U) {
        slab_list_remove(&sclass->partial_slabs, slab);
        slab_list_insert(&sclass->full_slabs, slab);
    }

    (void)pthread_mutex_unlock(&sclass->lock);

    /* Initialize allocation header */
    header = (alloc_header_t *)slot;
    header->magic = ALLOC_MAGIC_SMALL;
    header->kind = (uint16_t)ALLOC_KIND_SMALL;
    header->flags = (uint16_t)class_idx;
    header->requested_size = size;
    header->total_size = slab->slot_size;
    header->container = (void *)slab;

    return (void *)(header + 1);
}

static void slab_free(alloc_header_t *header)
{
    const uint16_t class_idx = header->flags;
    slab_t *slab;
    slab_class_t *sclass;
    slab_slot_node_t *node;
    int unmap_slab = 0;

    if (class_idx >= NUM_SLAB_CLASSES) {
        return;
    }

    slab = (slab_t *)header->container;
    if (slab == NULL || slab->magic != SLAB_MAGIC) {
        return;
    }

    sclass = &s_slab_classes[class_idx];
    if (pthread_mutex_lock(&sclass->lock) != 0) {
        return;
    }

    /* Verify slot is within slab bounds and aligned to slot boundary */
    if ((char *)header < slab->slot_start ||
        (char *)header + slab->slot_size > slab->slot_limit) {
        (void)pthread_mutex_unlock(&sclass->lock);
        return;
    }
    if (((size_t)((char *)header - slab->slot_start)) % slab->slot_size != 0U) {
        (void)pthread_mutex_unlock(&sclass->lock);
        return;
    }

    /* Poison magic to guard against double-free */
    header->magic = ALLOC_MAGIC_FREE;

    /* Push slot to LIFO freelist */
    node = (slab_slot_node_t *)header;
    node->next = slab->freelist;
    slab->freelist = node;
    slab->free_slots++;

    /* If slab was full, move to partial list */
    if (slab->free_slots == 1U) {
        slab_list_remove(&sclass->full_slabs, slab);
        slab_list_insert(&sclass->partial_slabs, slab);
    } else if (slab->free_slots == slab->total_slots) {
        /* Slab is completely empty */
        slab->freelist = NULL;
        slab->bump_ptr = slab->slot_start;
        slab_list_remove(&sclass->partial_slabs, slab);
        if (sclass->spare_slab == NULL) {
            sclass->spare_slab = slab;
            slab->is_spare = 1U;
        } else {
            /* Spare already cached; unmap this slab */
            unmap_slab = 1;
        }
    }

    (void)pthread_mutex_unlock(&sclass->lock);

    if (unmap_slab != 0) {
        (void)munmap((void *)slab, get_slab_mapping_bytes());
    }
}

/*
 * In-place realloc optimization for small slab allocations:
 * If the new requested size fits within the payload capacity of the current slot,
 * update requested_size safely and retain the same pointer.
 */
static void *small_realloc_in_place(alloc_header_t *header, size_t new_size)
{
    const uint16_t class_idx = header->flags;
    slab_t *slab;
    slab_class_t *sclass;
    size_t slot_capacity;

    if (class_idx >= NUM_SLAB_CLASSES) {
        return NULL;
    }

    slab = (slab_t *)header->container;
    if (slab == NULL || slab->magic != SLAB_MAGIC) {
        return NULL;
    }

    sclass = &s_slab_classes[class_idx];
    slot_capacity = sclass->payload_size;

    /* Check if the new size fits within the current slot capacity */
    if (new_size <= slot_capacity) {
        if (pthread_mutex_lock(&sclass->lock) != 0) {
            return NULL;
        }
        /* Verify slot integrity and bounds */
        if ((char *)header >= slab->slot_start &&
            (char *)header + slab->slot_size <= slab->slot_limit) {
            header->requested_size = new_size;
            (void)pthread_mutex_unlock(&sclass->lock);
            return (void *)(header + 1);
        }
        (void)pthread_mutex_unlock(&sclass->lock);
    }

    return NULL;
}

/* --- Medium Region Allocator Implementation --- */

static size_t get_medium_bin_index(size_t gross_size)
{
    if (gross_size <= 2080U) return 0U;
    if (gross_size <= 4128U) return 1U;
    if (gross_size <= 8224U) return 2U;
    if (gross_size <= 16416U) return 3U;
    if (gross_size <= 32800U) return 4U;
    return 5U;
}

static medium_free_node_t *chunk_to_free_node(alloc_header_t *chunk)
{
    return (medium_free_node_t *)((char *)chunk + sizeof(alloc_header_t));
}

static alloc_header_t *free_node_to_chunk(medium_free_node_t *node)
{
    return (alloc_header_t *)((char *)node - sizeof(alloc_header_t));
}

static void medium_bin_insert(alloc_header_t *chunk)
{
    const size_t bin = get_medium_bin_index(chunk->total_size);
    medium_free_node_t *node = chunk_to_free_node(chunk);

    node->prev = NULL;
    node->next = s_medium_bins[bin];
    if (s_medium_bins[bin] != NULL) {
        s_medium_bins[bin]->prev = node;
    }
    s_medium_bins[bin] = node;
}

static void medium_bin_remove(alloc_header_t *chunk)
{
    const size_t bin = get_medium_bin_index(chunk->total_size);
    medium_free_node_t *node = chunk_to_free_node(chunk);

    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else if (s_medium_bins[bin] == node) {
        s_medium_bins[bin] = node->next;
    }
    if (node->next != NULL) {
        node->next->prev = node->prev;
    }
    node->prev = NULL;
    node->next = NULL;
}

static void region_list_insert(region_t **head, region_t *region)
{
    region->prev = NULL;
    region->next = *head;
    if (*head != NULL) {
        (*head)->prev = region;
    }
    *head = region;
}

static void region_list_remove(region_t **head, region_t *region)
{
    if (region->prev != NULL) {
        region->prev->next = region->next;
    } else if (*head == region) {
        *head = region->next;
    }
    if (region->next != NULL) {
        region->next->prev = region->prev;
    }
    region->prev = NULL;
    region->next = NULL;
}

static region_t *create_region(void)
{
    const size_t mapping_bytes = get_region_mapping_bytes();
    void *mapping;
    region_t *region;
    uintptr_t raw_first;
    uintptr_t align_mask;
    uintptr_t first_chunk;
    char *usable_start;
    size_t raw_usable;
    size_t usable_bytes;
    char *usable_end;
    alloc_header_t *initial_chunk;

    mapping = mmap(NULL,
                   mapping_bytes,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1,
                   0);
    if (mapping == MAP_FAILED) {
        return NULL;
    }

    region = (region_t *)mapping;

    raw_first = (uintptr_t)mapping + sizeof(region_t);
    align_mask = (uintptr_t)_Alignof(max_align_t) - 1U;
    first_chunk = (raw_first + align_mask) & ~align_mask;
    usable_start = (char *)first_chunk;

    raw_usable = mapping_bytes - (size_t)(usable_start - (char *)mapping);
    usable_bytes = raw_usable & ~align_mask;
    usable_end = usable_start + usable_bytes;

    if (usable_bytes < MEDIUM_MIN_CHUNK) {
        (void)munmap(mapping, mapping_bytes);
        errno = ENOMEM;
        return NULL;
    }

    region->prev = NULL;
    region->next = NULL;
    region->magic = REGION_MAGIC;
    region->is_spare = 0U;
    region->total_mapping_bytes = mapping_bytes;
    region->active_allocations = 0U;
    region->usable_start = usable_start;
    region->usable_end = usable_end;

    /* Initialize single free chunk spanning entire usable space */
    initial_chunk = (alloc_header_t *)usable_start;
    initial_chunk->magic = ALLOC_MAGIC_FREE;
    initial_chunk->kind = (uint16_t)ALLOC_KIND_MEDIUM;
    initial_chunk->flags = 0U;
    initial_chunk->requested_size = 0U;
    initial_chunk->total_size = usable_bytes;
    initial_chunk->container = (void *)region;

    /* Write Knuth boundary footer at end of chunk */
    *(size_t *)(usable_start + usable_bytes - sizeof(size_t)) = usable_bytes;

    return region;
}

static void *medium_malloc(size_t size)
{
    const uintptr_t align_mask = (uintptr_t)_Alignof(max_align_t) - 1U;
    const size_t gross_size = (sizeof(alloc_header_t) + size + align_mask) & ~align_mask;
    const size_t start_bin = get_medium_bin_index(gross_size);
    alloc_header_t *chunk = NULL;
    region_t *region = NULL;
    size_t excess;
    alloc_header_t *right_neighbor;

    if (pthread_mutex_lock(&s_medium_mutex) != 0) {
        return NULL;
    }

    /* 1. Search segregated bins for a chunk >= gross_size */
    for (size_t b = start_bin; b < NUM_MEDIUM_BINS; ++b) {
        medium_free_node_t *curr = s_medium_bins[b];
        while (curr != NULL) {
            alloc_header_t *candidate = free_node_to_chunk(curr);
            if (candidate->total_size >= gross_size) {
                chunk = candidate;
                medium_bin_remove(chunk);
                break;
            }
            curr = curr->next;
        }
        if (chunk != NULL) {
            break;
        }
    }

    /* 2. If no free chunk found, adopt spare region or create new region */
    if (chunk == NULL) {
        if (s_spare_region != NULL) {
            region = s_spare_region;
            s_spare_region = NULL;
            region->is_spare = 0U;
            region_list_insert(&s_active_regions, region);
            chunk = (alloc_header_t *)region->usable_start;
        } else {
            region = create_region();
            if (region == NULL) {
                (void)pthread_mutex_unlock(&s_medium_mutex);
                return NULL;
            }
            region_list_insert(&s_active_regions, region);
            chunk = (alloc_header_t *)region->usable_start;
        }
    } else {
        region = (region_t *)chunk->container;
    }

    /* 3. Chunk Splitting */
    excess = chunk->total_size - gross_size;
    if (excess >= MEDIUM_MIN_CHUNK) {
        alloc_header_t *rem_chunk = (alloc_header_t *)((char *)chunk + gross_size);
        rem_chunk->magic = ALLOC_MAGIC_FREE;
        rem_chunk->kind = (uint16_t)ALLOC_KIND_MEDIUM;
        rem_chunk->flags = 0U; /* Previous chunk will be IN_USE */
        rem_chunk->requested_size = 0U;
        rem_chunk->total_size = excess;
        rem_chunk->container = (void *)region;

        /* Write footer for remainder chunk */
        *(size_t *)((char *)rem_chunk + excess - sizeof(size_t)) = excess;

        /* Update chunk after remainder to indicate remainder is FREE */
        right_neighbor = (alloc_header_t *)((char *)rem_chunk + excess);
        if ((char *)right_neighbor < region->usable_end) {
            right_neighbor->flags |= CHUNK_FLAG_PREV_FREE;
        }

        /* Insert remainder chunk into appropriate segregated bin */
        medium_bin_insert(rem_chunk);

        chunk->total_size = gross_size;
    } else {
        /* No split: update right-hand neighbor to indicate chunk is IN_USE */
        right_neighbor = (alloc_header_t *)((char *)chunk + chunk->total_size);
        if ((char *)right_neighbor < region->usable_end) {
            right_neighbor->flags &= ~CHUNK_FLAG_PREV_FREE;
        }
    }

    /* 4. Finalize allocated chunk */
    chunk->magic = ALLOC_MAGIC_MEDIUM;
    chunk->kind = (uint16_t)ALLOC_KIND_MEDIUM;
    chunk->flags |= CHUNK_FLAG_IN_USE;
    chunk->requested_size = size;
    chunk->container = (void *)region;

    region->active_allocations++;

    (void)pthread_mutex_unlock(&s_medium_mutex);

    return (void *)(chunk + 1);
}

static void medium_free(alloc_header_t *header)
{
    region_t *region;
    alloc_header_t *chunk = header;
    alloc_header_t *right_neighbor;
    region_t *unmap_region = NULL;
    size_t total_usable;

    if (chunk == NULL || chunk->magic != ALLOC_MAGIC_MEDIUM || chunk->kind != (uint16_t)ALLOC_KIND_MEDIUM) {
        return;
    }

    region = (region_t *)chunk->container;
    if (region == NULL || region->magic != REGION_MAGIC) {
        return;
    }

    if (pthread_mutex_lock(&s_medium_mutex) != 0) {
        return;
    }

    /* Check double-free and bounds */
    if ((chunk->flags & CHUNK_FLAG_IN_USE) == 0U) {
        (void)pthread_mutex_unlock(&s_medium_mutex);
        return;
    }
    if ((char *)chunk < region->usable_start ||
        (char *)chunk + chunk->total_size > region->usable_end) {
        (void)pthread_mutex_unlock(&s_medium_mutex);
        return;
    }

    region->active_allocations--;

    /* Mark chunk free */
    chunk->magic = ALLOC_MAGIC_FREE;
    chunk->flags &= ~CHUNK_FLAG_IN_USE;

    /* 1. Bidirectional Coalescing: Left */
    if ((chunk->flags & CHUNK_FLAG_PREV_FREE) != 0U && (char *)chunk > region->usable_start) {
        const size_t prev_size = *(size_t *)((char *)chunk - sizeof(size_t));
        if (prev_size >= MEDIUM_MIN_CHUNK &&
            (prev_size % _Alignof(max_align_t)) == 0U &&
            (char *)chunk - prev_size >= region->usable_start) {
            alloc_header_t *left_chunk = (alloc_header_t *)((char *)chunk - prev_size);
            if (left_chunk->magic == ALLOC_MAGIC_FREE &&
                (left_chunk->flags & CHUNK_FLAG_IN_USE) == 0U &&
                left_chunk->total_size == prev_size) {
                medium_bin_remove(left_chunk);
                left_chunk->total_size += chunk->total_size;
                chunk = left_chunk;
            }
        }
    }

    /* 2. Bidirectional Coalescing: Right */
    right_neighbor = (alloc_header_t *)((char *)chunk + chunk->total_size);
    if ((char *)right_neighbor < region->usable_end) {
        if (right_neighbor->magic == ALLOC_MAGIC_FREE &&
            (right_neighbor->flags & CHUNK_FLAG_IN_USE) == 0U) {
            const size_t right_size = right_neighbor->total_size;
            if (right_size >= MEDIUM_MIN_CHUNK &&
                (right_size % _Alignof(max_align_t)) == 0U &&
                (char *)right_neighbor + right_size <= region->usable_end) {
                medium_bin_remove(right_neighbor);
                chunk->total_size += right_size;
            }
        }
    }

    /* 3. Write boundary tag footer for coalesced chunk */
    *(size_t *)((char *)chunk + chunk->total_size - sizeof(size_t)) = chunk->total_size;

    /* 4. Update right neighbor's PREV_FREE flag */
    right_neighbor = (alloc_header_t *)((char *)chunk + chunk->total_size);
    if ((char *)right_neighbor < region->usable_end) {
        right_neighbor->flags |= CHUNK_FLAG_PREV_FREE;
    }

    /* 5. Region Reclamation */
    total_usable = (size_t)(region->usable_end - region->usable_start);
    if (region->active_allocations == 0U && chunk->total_size == total_usable) {
        region_list_remove(&s_active_regions, region);
        if (s_spare_region == NULL) {
            s_spare_region = region;
            region->is_spare = 1U;
        } else {
            /* Spare region already retained; unmap this region */
            unmap_region = region;
        }
    } else {
        medium_bin_insert(chunk);
    }

    (void)pthread_mutex_unlock(&s_medium_mutex);

    if (unmap_region != NULL) {
        (void)munmap((void *)unmap_region, unmap_region->total_mapping_bytes);
    }
}

/*
 * In-place realloc optimization for medium allocations:
 * 1. If new size fits within the current chunk capacity, reuse the chunk
 *    without modifying chunk size or boundary tags.
 * 2. If new size exceeds capacity and stays within the medium tier, attempt to
 *    absorb the physically adjacent right free chunk without moving payload.
 */
static void *medium_realloc_in_place(alloc_header_t *chunk, size_t new_size)
{
    region_t *region;
    size_t capacity;

    if (chunk == NULL || chunk->magic != ALLOC_MAGIC_MEDIUM || chunk->kind != (uint16_t)ALLOC_KIND_MEDIUM) {
        return NULL;
    }

    region = (region_t *)chunk->container;
    if (region == NULL || region->magic != REGION_MAGIC) {
        return NULL;
    }

    capacity = chunk->total_size - sizeof(alloc_header_t);

    if (pthread_mutex_lock(&s_medium_mutex) != 0) {
        return NULL;
    }

    /* Verify chunk integrity and active state */
    if ((chunk->flags & CHUNK_FLAG_IN_USE) == 0U ||
        (char *)chunk < region->usable_start ||
        (char *)chunk + chunk->total_size > region->usable_end) {
        (void)pthread_mutex_unlock(&s_medium_mutex);
        return NULL;
    }

    /* Case A: Already fits within current chunk capacity */
    if (new_size <= capacity) {
        chunk->requested_size = new_size;
        (void)pthread_mutex_unlock(&s_medium_mutex);
        return (void *)(chunk + 1);
    }

    /* Case B: Growth by consuming adjacent right free chunk */
    if (new_size <= MEDIUM_MAX_PAYLOAD) {
        const uintptr_t align_mask = (uintptr_t)_Alignof(max_align_t) - 1U;
        const size_t gross_size = (sizeof(alloc_header_t) + new_size + align_mask) & ~align_mask;
        alloc_header_t *right = (alloc_header_t *)((char *)chunk + chunk->total_size);

        if ((char *)right < region->usable_end &&
            right->magic == ALLOC_MAGIC_FREE &&
            (right->flags & CHUNK_FLAG_IN_USE) == 0U) {
            const size_t right_size = right->total_size;
            if (right_size >= MEDIUM_MIN_CHUNK &&
                (right_size % _Alignof(max_align_t)) == 0U &&
                (char *)right + right_size <= region->usable_end) {
                const size_t combined_size = chunk->total_size + right_size;
                if (combined_size >= gross_size) {
                    /* Merge right chunk into this chunk */
                    medium_bin_remove(right);
                    chunk->total_size = combined_size;
                    chunk->requested_size = new_size;

                    /* Clear PREV_FREE on the chunk after the combined chunk */
                    alloc_header_t *next_neighbor = (alloc_header_t *)((char *)chunk + combined_size);
                    if ((char *)next_neighbor < region->usable_end) {
                        next_neighbor->flags &= ~CHUNK_FLAG_PREV_FREE;
                    }

                    (void)pthread_mutex_unlock(&s_medium_mutex);
                    return (void *)(chunk + 1);
                }
            }
        }
    }

    (void)pthread_mutex_unlock(&s_medium_mutex);
    return NULL;
}

/* --- Large Direct Allocator Implementation --- */

static void *direct_malloc(size_t size)
{
    const size_t payload_bytes = (size == 0U) ? 1U : size;
    size_t mapping_bytes;
    alloc_header_t *header;
    void *mapping;

    if (calculate_mapping_size(payload_bytes, &mapping_bytes) != 0) {
        return NULL;
    }

    mapping = mmap(NULL,
                   mapping_bytes,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1,
                   0);
    if (mapping == MAP_FAILED) {
        return NULL;
    }

    header = (alloc_header_t *)mapping;
    header->magic = ALLOC_MAGIC_DIRECT;
    header->kind = (uint16_t)ALLOC_KIND_DIRECT;
    header->flags = 0U;
    header->requested_size = size;
    header->total_size = mapping_bytes;
    header->container = NULL;

    return (void *)(header + 1);
}

static void direct_free(alloc_header_t *header)
{
    /* Poison magic to guard against double-free */
    header->magic = ALLOC_MAGIC_FREE;

    (void)munmap((void *)header, header->total_size);
}

/* --- Public API Implementation --- */

void *my_malloc(size_t size)
{
    const size_t payload_bytes = (size == 0U) ? 1U : size;

    if (payload_bytes <= SLAB_MAX_PAYLOAD) {
        return slab_malloc(size);
    }
    if (payload_bytes <= MEDIUM_MAX_PAYLOAD) {
        return medium_malloc(size);
    }
    return direct_malloc(size);
}

void my_free(void *ptr)
{
    alloc_header_t *header;

    if (ptr == NULL) {
        return;
    }

    header = ((alloc_header_t *)ptr) - 1;
    if (header->magic == ALLOC_MAGIC_SMALL && header->kind == (uint16_t)ALLOC_KIND_SMALL) {
        slab_free(header);
    } else if (header->magic == ALLOC_MAGIC_MEDIUM && header->kind == (uint16_t)ALLOC_KIND_MEDIUM) {
        medium_free(header);
    } else if (header->magic == ALLOC_MAGIC_DIRECT && header->kind == (uint16_t)ALLOC_KIND_DIRECT) {
        direct_free(header);
    }
}

void *my_realloc(void *ptr, size_t size)
{
    alloc_header_t *old_header;
    size_t old_size;
    size_t copy_size;
    void *new_ptr;

    if (ptr == NULL) {
        return my_malloc(size);
    }

    if (size == 0U) {
        my_free(ptr);
        return NULL;
    }

    old_header = ((alloc_header_t *)ptr) - 1;

    /* 1. Try small in-place reuse */
    if (old_header->magic == ALLOC_MAGIC_SMALL && old_header->kind == (uint16_t)ALLOC_KIND_SMALL) {
        void *in_place = small_realloc_in_place(old_header, size);
        if (in_place != NULL) {
            return in_place;
        }
    }
    /* 2. Try medium in-place reuse or right-neighbor growth */
    else if (old_header->magic == ALLOC_MAGIC_MEDIUM && old_header->kind == (uint16_t)ALLOC_KIND_MEDIUM) {
        void *in_place = medium_realloc_in_place(old_header, size);
        if (in_place != NULL) {
            return in_place;
        }
    }
    else if (old_header->magic != ALLOC_MAGIC_DIRECT || old_header->kind != (uint16_t)ALLOC_KIND_DIRECT) {
        errno = EINVAL;
        return NULL;
    }

    old_size = old_header->requested_size;

    /* 3. Moving reallocation fallback */
    new_ptr = my_malloc(size);
    if (new_ptr == NULL) {
        /* The original allocation remains valid and intact on failure. */
        return NULL;
    }

    copy_size = (old_size < size) ? old_size : size;
    if (copy_size != 0U) {
        (void)memcpy(new_ptr, ptr, copy_size);
    }

    my_free(ptr);
    return new_ptr;
}

void *my_calloc(size_t nmemb, size_t size)
{
    size_t total_bytes;
    void *ptr;

    if (size != 0U && nmemb > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }

    total_bytes = nmemb * size;
    ptr = my_malloc(total_bytes);
    if (ptr == NULL) {
        return NULL;
    }

    if (total_bytes != 0U) {
        (void)memset(ptr, 0, total_bytes);
    }
    return ptr;
}
