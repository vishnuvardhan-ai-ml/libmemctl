#define _POSIX_C_SOURCE 200809L

#include "libmemctl.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define THREAD_COUNT 8U
#define OPERATIONS_PER_THREAD 20000U
#define SLOT_COUNT 48U

typedef struct {
    unsigned char *ptr;
    size_t size;
    unsigned char tag;
} slot_t;

typedef struct {
    uint32_t seed;
    uint64_t operations;
    unsigned int errors;
} worker_t;

static uint32_t next_random(uint32_t *state)
{
    uint32_t value = *state;

    if (value == 0U) {
        value = UINT32_C(0x9e3779b9);
    }

    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;

    return value;
}

static size_t choose_size(uint32_t value)
{
    static const size_t sizes[] = {
        1U, 2U, 7U, 15U, 16U, 31U, 32U, 63U, 64U,
        65U, 127U, 128U, 129U, 255U, 256U, 257U,
        511U, 512U, 513U, 1023U, 1024U, 2048U,
        4095U, 4096U, 4097U, 8192U, 16384U
    };

    return sizes[value % (sizeof(sizes) / sizeof(sizes[0]))]
           % 4097U + 1U;
}

static void fill_data(unsigned char *ptr, size_t size,
                      unsigned char tag)
{
    size_t i;

    for (i = 0U; i < size; ++i) {
        ptr[i] = tag;
    }
}

static bool check_data(const unsigned char *ptr, size_t size,
                       unsigned char tag)
{
    size_t i;

    for (i = 0U; i < size; ++i) {
        if (ptr[i] != tag) {
            return false;
        }
    }

    return true;
}

static void *worker_run(void *argument)
{
    worker_t *worker = argument;
    slot_t slots[SLOT_COUNT] = {{0}};
    size_t operation;

    for (operation = 0U;
         operation < OPERATIONS_PER_THREAD;
         ++operation) {

        uint32_t random_value = next_random(&worker->seed);
        size_t index = random_value % SLOT_COUNT;
        slot_t *slot = &slots[index];
        uint32_t action = random_value % 100U;

        if (slot->ptr == NULL) {
            size_t size = choose_size(random_value >> 8U);
            unsigned char tag = (unsigned char)(random_value >> 24U);
            unsigned char *ptr = my_malloc(size);

            if (ptr == NULL) {
                ++worker->errors;
                continue;
            }

            fill_data(ptr, size, tag);
            slot->ptr = ptr;
            slot->size = size;
            slot->tag = tag;
        } else if (action < 25U) {
            if (!check_data(slot->ptr, slot->size, slot->tag)) {
                ++worker->errors;
            }

            my_free(slot->ptr);
            slot->ptr = NULL;
            slot->size = 0U;
        } else if (action < 55U) {
            size_t new_size = choose_size(random_value >> 8U);
            size_t preserve =
                slot->size < new_size ? slot->size : new_size;

            unsigned char *new_ptr;

            if (!check_data(slot->ptr, slot->size, slot->tag)) {
                ++worker->errors;
            }

            new_ptr = my_realloc(slot->ptr, new_size);

            if (new_ptr != NULL) {
                if (!check_data(new_ptr, preserve, slot->tag)) {
                    ++worker->errors;
                }

                slot->ptr = new_ptr;
                slot->size = new_size;
                fill_data(slot->ptr, slot->size, slot->tag);
            } else if (!check_data(slot->ptr, slot->size, slot->tag)) {
                ++worker->errors;
            }
        } else {
            if (!check_data(slot->ptr, slot->size, slot->tag)) {
                ++worker->errors;
            }
        }

        ++worker->operations;
    }

    for (operation = 0U; operation < SLOT_COUNT; ++operation) {
        if (slots[operation].ptr != NULL) {
            if (!check_data(slots[operation].ptr,
                            slots[operation].size,
                            slots[operation].tag)) {
                ++worker->errors;
            }

            my_free(slots[operation].ptr);
        }
    }

    return NULL;
}

static double elapsed_seconds(struct timespec start,
                              struct timespec finish)
{
    return (double)(finish.tv_sec - start.tv_sec) +
           (double)(finish.tv_nsec - start.tv_nsec) / 1e9;
}

static bool run_benchmark(unsigned int thread_count)
{
    pthread_t threads[THREAD_COUNT];
    worker_t workers[THREAD_COUNT];
    struct timespec start;
    struct timespec finish;

    uint64_t total_operations = 0U;
    unsigned int total_errors = 0U;
    unsigned int created = 0U;
    unsigned int i;
    double seconds;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return false;
    }

    for (i = 0U; i < thread_count; ++i) {
        workers[i].seed =
            UINT32_C(0x6a09e667) ^
            ((uint32_t)(i + 1U) * UINT32_C(0x9e3779b9));

        workers[i].operations = 0U;
        workers[i].errors = 0U;

        if (thread_count == 1U) {
            (void)worker_run(&workers[i]);
            created = 1U;
        } else {
            if (pthread_create(&threads[i], NULL,
                               worker_run, &workers[i]) != 0) {
                break;
            }
            ++created;
        }
    }

    if (thread_count > 1U) {
        for (i = 0U; i < created; ++i) {
            if (pthread_join(threads[i], NULL) != 0) {
                ++total_errors;
            }
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC, &finish) != 0) {
        return false;
    }

    for (i = 0U; i < created; ++i) {
        total_operations += workers[i].operations;
        total_errors += workers[i].errors;
    }

    seconds = elapsed_seconds(start, finish);

    printf("\nThreads: %u\n", thread_count);
    printf("Operations: %" PRIu64 "\n", total_operations);
    printf("Elapsed time: %.6f seconds\n", seconds);
    printf("Throughput: %.0f operations/second\n",
           seconds > 0.0 ? (double)total_operations / seconds : 0.0);
    printf("Errors: %u\n", total_errors);

    return created == thread_count &&
           total_errors == 0U &&
           total_operations ==
               (uint64_t)thread_count * OPERATIONS_PER_THREAD;
}

int main(void)
{
    bool single_ok;
    bool multi_ok;

    puts("libmemctl allocator benchmark");
    puts("==============================");

    single_ok = run_benchmark(1U);
    multi_ok = run_benchmark(THREAD_COUNT);

    puts("\nBenchmark result:");

    if (single_ok && multi_ok) {
        puts("PASS: Both benchmark workloads completed successfully.");
        return 0;
    }

    puts("FAIL: Benchmark encountered an error.");
    return 1;
}
