#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define SIMPLE_PATH "/dev/simple_char"
#define SAFE_PATH "/dev/thread_safe_char"
#define LOCK_FREE_PATH "/dev/lock_free_char"
#define SIMPLE_CAPACITY 1024U

struct stream_context {
    const char *path;
    size_t total_bytes;
    size_t chunk_size;
    atomic_ullong bytes_written;
    atomic_ullong bytes_read;
    atomic_ullong write_calls;
    atomic_ullong read_calls;
    atomic_ullong short_writes;
    atomic_ullong short_reads;
    atomic_ullong mismatches;
    atomic_int error_code;
};

struct producer_context {
    const char *path;
    uint8_t token;
    size_t iterations;
    atomic_ullong *eagain_count;
    atomic_int *producers_done;
    atomic_int *unexpected_errors;
    atomic_bool *stop;
};

struct consumer_context {
    const char *path;
    unsigned int producer_count;
    size_t expected_total;
    atomic_ullong *consumed_total;
    atomic_ullong *eagain_count;
    atomic_int *producers_done;
    atomic_int *unexpected_errors;
    atomic_bool *stop;
    unsigned long long histogram[256];
};

struct simple_writer_context {
    uint8_t marker;
    atomic_int *unexpected_errors;
};

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

static uint8_t stream_byte(size_t offset)
{
    return (uint8_t)(((offset * 131U) + 29U) & 0xFFU);
}

static void prepare_stream_data(uint8_t *buffer, size_t length, size_t offset)
{
    size_t index;

    for (index = 0; index < length; ++index)
        buffer[index] = stream_byte(offset + index);
}

static void *stream_writer(void *argument)
{
    struct stream_context *context = argument;
    uint8_t *buffer;
    size_t offset = 0;
    ssize_t result;
    int fd;

    buffer = malloc(context->chunk_size);
    if (!buffer) {
        atomic_store(&context->error_code, ENOMEM);
        return NULL;
    }

    fd = open(context->path, O_WRONLY);
    if (fd < 0) {
        atomic_store(&context->error_code, errno);
        free(buffer);
        return NULL;
    }

    while (offset < context->total_bytes && atomic_load(&context->error_code) == 0) {
        size_t requested = context->total_bytes - offset;

        if (requested > context->chunk_size)
            requested = context->chunk_size;

        prepare_stream_data(buffer, requested, offset);
        result = write(fd, buffer, requested);
        atomic_fetch_add(&context->write_calls, 1);

        if (result < 0) {
            if (errno == EINTR)
                continue;
            atomic_store(&context->error_code, errno);
            break;
        }

        if (result == 0) {
            atomic_store(&context->error_code, EIO);
            break;
        }

        if ((size_t)result < requested)
            atomic_fetch_add(&context->short_writes, 1);

        offset += (size_t)result;
        atomic_fetch_add(&context->bytes_written, (unsigned long long)result);
    }

    close(fd);
    free(buffer);
    return NULL;
}

static void *stream_reader(void *argument)
{
    struct stream_context *context = argument;
    uint8_t *buffer;
    size_t offset = 0;
    ssize_t result;
    int fd;

    buffer = malloc(context->chunk_size);
    if (!buffer) {
        atomic_store(&context->error_code, ENOMEM);
        return NULL;
    }

    fd = open(context->path, O_RDONLY);
    if (fd < 0) {
        atomic_store(&context->error_code, errno);
        free(buffer);
        return NULL;
    }

    while (offset < context->total_bytes && atomic_load(&context->error_code) == 0) {
        size_t requested = context->total_bytes - offset;
        size_t index;

        if (requested > context->chunk_size)
            requested = context->chunk_size;

        result = read(fd, buffer, requested);
        atomic_fetch_add(&context->read_calls, 1);

        if (result < 0) {
            if (errno == EINTR)
                continue;
            atomic_store(&context->error_code, errno);
            break;
        }

        if (result == 0) {
            atomic_store(&context->error_code, EIO);
            break;
        }

        if ((size_t)result < requested)
            atomic_fetch_add(&context->short_reads, 1);

        for (index = 0; index < (size_t)result; ++index) {
            if (buffer[index] != stream_byte(offset + index))
                atomic_fetch_add(&context->mismatches, 1);
        }

        offset += (size_t)result;
        atomic_fetch_add(&context->bytes_read, (unsigned long long)result);
    }

    close(fd);
    free(buffer);
    return NULL;
}

static int run_stream_test(const char *path, const char *label,
                           size_t total_bytes, size_t chunk_size)
{
    struct stream_context context = {
        .path = path,
        .total_bytes = total_bytes,
        .chunk_size = chunk_size,
    };
    struct rusage usage_start;
    struct rusage usage_end;
    struct timespec start;
    struct timespec end;
    pthread_t writer_thread;
    pthread_t reader_thread;
    unsigned long long bytes_written;
    unsigned long long bytes_read;
    unsigned long long mismatches;
    unsigned long long write_calls;
    unsigned long long read_calls;
    double seconds;
    double throughput;
    double reliability;
    bool passed;

    printf("\n[%s SPSC stream stress]\n", label);
    printf("  Target bytes: %zu, chunk: %zu\n", total_bytes, chunk_size);

    getrusage(RUSAGE_SELF, &usage_start);
    clock_gettime(CLOCK_MONOTONIC, &start);

    if (pthread_create(&reader_thread, NULL, stream_reader, &context) != 0) {
        perror("pthread_create reader");
        return EXIT_FAILURE;
    }

    if (pthread_create(&writer_thread, NULL, stream_writer, &context) != 0) {
        perror("pthread_create writer");
        pthread_cancel(reader_thread);
        pthread_join(reader_thread, NULL);
        return EXIT_FAILURE;
    }

    pthread_join(writer_thread, NULL);
    pthread_join(reader_thread, NULL);

    clock_gettime(CLOCK_MONOTONIC, &end);
    getrusage(RUSAGE_SELF, &usage_end);

    seconds = elapsed_seconds(&start, &end);
    bytes_written = atomic_load(&context.bytes_written);
    bytes_read = atomic_load(&context.bytes_read);
    mismatches = atomic_load(&context.mismatches);
    write_calls = atomic_load(&context.write_calls);
    read_calls = atomic_load(&context.read_calls);
    throughput = seconds > 0.0 ? ((double)bytes_read / (1024.0 * 1024.0)) / seconds : 0.0;
    if (total_bytes > 0) {
        unsigned long long verified = bytes_read > total_bytes ? total_bytes : bytes_read;
        if (mismatches > verified)
            verified = 0;
        else
            verified -= mismatches;
        reliability = ((double)verified / (double)total_bytes) * 100.0;
    } else {
        reliability = 100.0;
    }

    passed = atomic_load(&context.error_code) == 0 &&
             bytes_written == total_bytes &&
             bytes_read == total_bytes &&
             mismatches == 0;

    printf("  Bytes written:       %llu\n", bytes_written);
    printf("  Bytes read:          %llu\n", bytes_read);
    printf("  Data mismatches:     %llu\n", mismatches);
    printf("  write() calls:       %llu\n", write_calls);
    printf("  read() calls:        %llu\n", read_calls);
    printf("  Short writes:        %llu\n", atomic_load(&context.short_writes));
    printf("  Short reads:         %llu\n", atomic_load(&context.short_reads));
    printf("  Elapsed:             %.3f s\n", seconds);
    printf("  Throughput:          %.2f MiB/s\n", throughput);
    printf("  Reliability:         %.6f%%\n", reliability);
    printf("  Voluntary switches:  %ld\n",
           usage_end.ru_nvcsw - usage_start.ru_nvcsw);
    printf("  Involuntary switches:%ld\n",
           usage_end.ru_nivcsw - usage_start.ru_nivcsw);
    printf("  Unexpected errno:    %d\n", atomic_load(&context.error_code));
    printf("  PASS criteria: exact byte count + zero corruption + zero unexpected errors\n");
    printf("  Result: %s\n", passed ? "PASS" : "FAIL");

    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void *mpmc_producer(void *argument)
{
    struct producer_context *context = argument;
    size_t index = 0;
    ssize_t result;
    int fd;

    fd = open(context->path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        atomic_fetch_add(context->unexpected_errors, 1);
        atomic_fetch_add(context->producers_done, 1);
        return NULL;
    }

    while (index < context->iterations && !atomic_load(context->stop)) {
        result = write(fd, &context->token, 1);
        if (result == 1) {
            ++index;
            continue;
        }

        if (result < 0 && errno == EAGAIN) {
            atomic_fetch_add(context->eagain_count, 1);
            sched_yield();
            continue;
        }

        if (result < 0 && errno == EINTR)
            continue;

        atomic_fetch_add(context->unexpected_errors, 1);
        break;
    }

    close(fd);
    atomic_fetch_add(context->producers_done, 1);
    return NULL;
}

static void *mpmc_consumer(void *argument)
{
    struct consumer_context *context = argument;
    uint8_t buffer[256];
    ssize_t result;
    int fd;

    fd = open(context->path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        atomic_fetch_add(context->unexpected_errors, 1);
        return NULL;
    }

    for (;;) {
        size_t index;

        if (atomic_load(context->stop))
            break;

        if (atomic_load(context->consumed_total) >= context->expected_total &&
            atomic_load(context->producers_done) == (int)context->producer_count)
            break;

        result = read(fd, buffer, sizeof(buffer));
        if (result > 0) {
            for (index = 0; index < (size_t)result; ++index)
                context->histogram[buffer[index]]++;
            atomic_fetch_add(context->consumed_total, (unsigned long long)result);
            continue;
        }

        if (result < 0 && errno == EAGAIN) {
            atomic_fetch_add(context->eagain_count, 1);
            sched_yield();
            continue;
        }

        if (result < 0 && errno == EINTR)
            continue;

        atomic_fetch_add(context->unexpected_errors, 1);
        break;
    }

    close(fd);
    return NULL;
}

static int run_mpmc_test(unsigned int producer_count,
                         unsigned int consumer_count,
                         size_t iterations)
{
    pthread_t *producer_threads = NULL;
    pthread_t *consumer_threads = NULL;
    struct producer_context *producer_contexts = NULL;
    struct consumer_context *consumer_contexts = NULL;
    atomic_ullong producer_eagain = 0;
    atomic_ullong consumer_eagain = 0;
    atomic_ullong consumed_total = 0;
    atomic_int producers_done = 0;
    atomic_int unexpected_errors = 0;
    atomic_bool stop = false;
    struct timespec start;
    struct timespec end;
    unsigned long long combined_histogram[256] = {0};
    size_t expected_total = (size_t)producer_count * iterations;
    unsigned int index;
    unsigned int consumer_index;
    unsigned int producers_created = 0;
    unsigned int consumers_created = 0;
    bool histogram_ok = true;
    bool passed;
    unsigned long long correct_tokens = 0;
    double seconds;
    double reliability;
    int result = EXIT_FAILURE;

    printf("\n[thread_safe_char MPMC reliability stress]\n");
    printf("  Producers: %u, consumers: %u, bytes/producer: %zu\n",
           producer_count, consumer_count, iterations);

    producer_threads = calloc(producer_count, sizeof(*producer_threads));
    consumer_threads = calloc(consumer_count, sizeof(*consumer_threads));
    producer_contexts = calloc(producer_count, sizeof(*producer_contexts));
    consumer_contexts = calloc(consumer_count, sizeof(*consumer_contexts));

    if (!producer_threads || !consumer_threads || !producer_contexts || !consumer_contexts) {
        fprintf(stderr, "allocation failure\n");
        goto cleanup;
    }

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (index = 0; index < consumer_count; ++index) {
        consumer_contexts[index].path = SAFE_PATH;
        consumer_contexts[index].producer_count = producer_count;
        consumer_contexts[index].expected_total = expected_total;
        consumer_contexts[index].consumed_total = &consumed_total;
        consumer_contexts[index].eagain_count = &consumer_eagain;
        consumer_contexts[index].producers_done = &producers_done;
        consumer_contexts[index].unexpected_errors = &unexpected_errors;
        consumer_contexts[index].stop = &stop;
        if (pthread_create(&consumer_threads[index], NULL, mpmc_consumer,
                           &consumer_contexts[index]) != 0) {
            atomic_fetch_add(&unexpected_errors, 1);
            atomic_store(&stop, true);
            break;
        }
        consumers_created++;
    }

    for (index = 0; index < producer_count; ++index) {
        producer_contexts[index].path = SAFE_PATH;
        producer_contexts[index].token = (uint8_t)(index + 1U);
        producer_contexts[index].iterations = iterations;
        producer_contexts[index].eagain_count = &producer_eagain;
        producer_contexts[index].producers_done = &producers_done;
        producer_contexts[index].unexpected_errors = &unexpected_errors;
        producer_contexts[index].stop = &stop;
        if (pthread_create(&producer_threads[index], NULL, mpmc_producer,
                           &producer_contexts[index]) != 0) {
            atomic_fetch_add(&unexpected_errors, 1);
            atomic_store(&stop, true);
            break;
        }
        producers_created++;
    }

    for (index = 0; index < producers_created; ++index)
        pthread_join(producer_threads[index], NULL);

    if (producers_created != producer_count)
        atomic_store(&stop, true);

    for (index = 0; index < consumers_created; ++index)
        pthread_join(consumer_threads[index], NULL);

    clock_gettime(CLOCK_MONOTONIC, &end);

    for (consumer_index = 0; consumer_index < consumers_created; ++consumer_index) {
        for (index = 0; index < 256U; ++index)
            combined_histogram[index] += consumer_contexts[consumer_index].histogram[index];
    }

    for (index = 0; index < producer_count; ++index) {
        unsigned long long observed = combined_histogram[index + 1U];
        if (observed != iterations)
            histogram_ok = false;
        correct_tokens += observed < iterations ? observed : iterations;
    }

    for (index = producer_count + 1U; index < 256U; ++index) {
        if (combined_histogram[index] != 0)
            histogram_ok = false;
    }

    seconds = elapsed_seconds(&start, &end);
    reliability = expected_total > 0 ? ((double)correct_tokens / (double)expected_total) * 100.0 : 100.0;
    passed = atomic_load(&unexpected_errors) == 0 &&
             atomic_load(&consumed_total) == expected_total &&
             histogram_ok;

    printf("  Expected bytes:      %zu\n", expected_total);
    printf("  Consumed bytes:      %llu\n", atomic_load(&consumed_total));
    printf("  Producer EAGAIN:     %llu\n", atomic_load(&producer_eagain));
    printf("  Consumer EAGAIN:     %llu\n", atomic_load(&consumer_eagain));
    printf("  Unexpected errors:   %d\n", atomic_load(&unexpected_errors));
    printf("  Token histogram:     %s\n", histogram_ok ? "exact" : "mismatch");
    printf("  Elapsed:             %.3f s\n", seconds);
    printf("  Aggregate rate:      %.0f bytes/s\n",
           seconds > 0.0 ? (double)atomic_load(&consumed_total) / seconds : 0.0);
    printf("  Reliability:         %.6f%%\n", reliability);
    printf("  PASS criteria: every producer byte consumed exactly once + zero unexpected errors\n");
    printf("  Result: %s\n", passed ? "PASS" : "FAIL");

    result = passed ? EXIT_SUCCESS : EXIT_FAILURE;

cleanup:
    free(producer_threads);
    free(consumer_threads);
    free(producer_contexts);
    free(consumer_contexts);
    return result;
}

static void *simple_writer(void *argument)
{
    struct simple_writer_context *context = argument;
    size_t offset;
    ssize_t result;
    int fd;

    fd = open(SIMPLE_PATH, O_WRONLY);
    if (fd < 0) {
        atomic_fetch_add(context->unexpected_errors, 1);
        return NULL;
    }

    for (offset = 0; offset < SIMPLE_CAPACITY; ++offset) {
        result = write(fd, &context->marker, 1);
        if (result != 1) {
            atomic_fetch_add(context->unexpected_errors, 1);
            break;
        }
        sched_yield();
    }

    close(fd);
    return NULL;
}

static int run_simple_race_observation(unsigned int writer_count,
                                       unsigned int rounds)
{
    unsigned int mixed_rounds = 0;
    unsigned int round;
    atomic_int unexpected_errors = 0;

    printf("\n[simple_char concurrent-write observation]\n");
    printf("  Writers: %u, rounds: %u, one-byte writes per writer: %u\n",
           writer_count, rounds, SIMPLE_CAPACITY);

    for (round = 0; round < rounds; ++round) {
        pthread_t *threads = calloc(writer_count, sizeof(*threads));
        struct simple_writer_context *contexts = calloc(writer_count, sizeof(*contexts));
        uint8_t buffer[SIMPLE_CAPACITY];
        bool seen[256] = {false};
        unsigned int unique_markers = 0;
        unsigned int index;
        ssize_t bytes_read;
        int fd;

        if (!threads || !contexts) {
            free(threads);
            free(contexts);
            atomic_fetch_add(&unexpected_errors, 1);
            break;
        }

        for (index = 0; index < writer_count; ++index) {
            contexts[index].marker = (uint8_t)(index + 1U);
            contexts[index].unexpected_errors = &unexpected_errors;
            if (pthread_create(&threads[index], NULL, simple_writer,
                               &contexts[index]) != 0) {
                atomic_fetch_add(&unexpected_errors, 1);
                writer_count = index;
                break;
            }
        }

        for (index = 0; index < writer_count; ++index)
            pthread_join(threads[index], NULL);

        fd = open(SIMPLE_PATH, O_RDONLY);
        if (fd < 0) {
            atomic_fetch_add(&unexpected_errors, 1);
        } else {
            bytes_read = read(fd, buffer, sizeof(buffer));
            close(fd);

            if (bytes_read == (ssize_t)sizeof(buffer)) {
                for (index = 0; index < sizeof(buffer); ++index)
                    seen[buffer[index]] = true;

                for (index = 1; index <= writer_count; ++index) {
                    if (seen[index])
                        ++unique_markers;
                }

                if (unique_markers > 1U)
                    ++mixed_rounds;
            }
        }

        free(threads);
        free(contexts);
    }

    printf("  Mixed final buffers: %u/%u\n", mixed_rounds, rounds);
    printf("  Unexpected errors:   %d\n", atomic_load(&unexpected_errors));
    printf("  Interpretation: mixed buffers demonstrate unsynchronized overlapping writers\n");
    printf("  Concurrency contract: UNSUPPORTED (not a correctness failure for the basic demo)\n");

    return atomic_load(&unexpected_errors) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s stream safe|lockfree [MiB] [chunk_bytes]\n"
            "  %s mpmc [producers] [consumers] [bytes_per_producer]\n"
            "  %s simple-race [writers] [rounds]\n",
            program, program, program);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "stream") == 0) {
        size_t mib = argc >= 4 ? strtoull(argv[3], NULL, 10) : 64U;
        size_t chunk = argc >= 5 ? strtoull(argv[4], NULL, 10) : 1024U;
        size_t total_bytes = mib * 1024U * 1024U;

        if (argc < 3 || mib == 0 || chunk == 0) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }

        if (strcmp(argv[2], "safe") == 0)
            return run_stream_test(SAFE_PATH, "thread_safe_char", total_bytes, chunk);

        if (strcmp(argv[2], "lockfree") == 0)
            return run_stream_test(LOCK_FREE_PATH, "lock_free_char", total_bytes, chunk);

        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "mpmc") == 0) {
        unsigned int producers = argc >= 3 ? (unsigned int)strtoul(argv[2], NULL, 10) : 4U;
        unsigned int consumers = argc >= 4 ? (unsigned int)strtoul(argv[3], NULL, 10) : 4U;
        size_t iterations = argc >= 5 ? strtoull(argv[4], NULL, 10) : 50000U;

        if (producers == 0 || consumers == 0 || producers > 200U || iterations == 0) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }

        return run_mpmc_test(producers, consumers, iterations);
    }

    if (strcmp(argv[1], "simple-race") == 0) {
        unsigned int writers = argc >= 3 ? (unsigned int)strtoul(argv[2], NULL, 10) : 4U;
        unsigned int rounds = argc >= 4 ? (unsigned int)strtoul(argv[3], NULL, 10) : 10U;

        if (writers < 2U || writers > 200U || rounds == 0) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }

        return run_simple_race_observation(writers, rounds);
    }

    print_usage(argv[0]);
    return EXIT_FAILURE;
}
