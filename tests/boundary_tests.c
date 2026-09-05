#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SIMPLE_PATH "/dev/simple_char"
#define SAFE_PATH "/dev/thread_safe_char"
#define LOCK_FREE_PATH "/dev/lock_free_char"

#define SIMPLE_CAPACITY 1024U
#define QUEUE_CAPACITY 4096U

struct test_state {
    unsigned int passed;
    unsigned int failed;
};

static void record_result(struct test_state *state, bool condition,
                          const char *name)
{
    printf("  %-48s %s\n", name, condition ? "PASS" : "FAIL");
    if (condition)
        state->passed++;
    else
        state->failed++;
}

static void fill_pattern(uint8_t *buffer, size_t length, uint32_t seed)
{
    size_t index;

    for (index = 0; index < length; ++index)
        buffer[index] = (uint8_t)((index * 37U + seed * 17U) & 0xFFU);
}

static bool drain_queue(const char *path)
{
    uint8_t buffer[512];
    ssize_t bytes_read;
    int fd;

    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return false;

    for (;;) {
        bytes_read = read(fd, buffer, sizeof(buffer));
        if (bytes_read > 0)
            continue;
        if (bytes_read < 0 && errno == EINTR)
            continue;
        if (bytes_read < 0 && errno == EAGAIN)
            break;
        if (bytes_read == 0)
            break;
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

static int test_simple_driver(void)
{
    struct test_state state = {0};
    uint8_t write_buffer[SIMPLE_CAPACITY + 64U];
    uint8_t read_buffer[SIMPLE_CAPACITY + 64U];
    uint8_t byte = 0xA5U;
    ssize_t result;
    int write_fd;
    int read_fd;

    printf("\n[simple_char boundary tests]\n");

    write_fd = open(SIMPLE_PATH, O_WRONLY);
    record_result(&state, write_fd >= 0, "device opens for writing");
    if (write_fd < 0)
        goto summary;

    result = write(write_fd, write_buffer, 0);
    record_result(&state, result == 0, "zero-length write returns 0");

    fill_pattern(write_buffer, SIMPLE_CAPACITY, 1U);
    result = write(write_fd, write_buffer, SIMPLE_CAPACITY);
    record_result(&state, result == (ssize_t)SIMPLE_CAPACITY,
                  "exact-capacity write succeeds");

    errno = 0;
    result = write(write_fd, &byte, 1);
    record_result(&state, result == -1 && errno == ENOSPC,
                  "write beyond current file capacity returns ENOSPC");
    close(write_fd);

    read_fd = open(SIMPLE_PATH, O_RDONLY);
    record_result(&state, read_fd >= 0, "device opens for reading");
    if (read_fd < 0)
        goto summary;

    memset(read_buffer, 0, sizeof(read_buffer));
    result = read(read_fd, read_buffer, SIMPLE_CAPACITY);
    record_result(&state,
                  result == (ssize_t)SIMPLE_CAPACITY &&
                      memcmp(write_buffer, read_buffer, SIMPLE_CAPACITY) == 0,
                  "exact-capacity read preserves data");

    result = read(read_fd, &byte, 1);
    record_result(&state, result == 0, "read at end of stored data returns EOF");
    close(read_fd);

    write_fd = open(SIMPLE_PATH, O_WRONLY);
    if (write_fd >= 0) {
        fill_pattern(write_buffer, sizeof(write_buffer), 2U);
        result = write(write_fd, write_buffer, sizeof(write_buffer));
        record_result(&state, result == (ssize_t)SIMPLE_CAPACITY,
                      "oversized write is truncated to device capacity");
        close(write_fd);
    } else {
        record_result(&state, false, "oversized write is truncated to device capacity");
    }

summary:
    printf("  Result: %u passed, %u failed\n", state.passed, state.failed);
    return state.failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int test_queue_boundaries(const char *path, const char *label,
                                 bool test_exclusive_open)
{
    struct test_state state = {0};
    uint8_t *write_buffer = NULL;
    uint8_t *read_buffer = NULL;
    uint8_t *expected = NULL;
    uint8_t extra_byte = 0x5AU;
    ssize_t result;
    size_t first_write = 3000U;
    size_t first_read = 2000U;
    size_t second_write = 3000U;
    size_t expected_length = (first_write - first_read) + second_write;
    int fd = -1;
    int reader_fd = -1;
    int second_reader_fd = -1;
    int writer_fd = -1;
    int second_writer_fd = -1;

    printf("\n[%s boundary tests]\n", label);

    record_result(&state, drain_queue(path), "queue starts in a drained state");

    fd = open(path, O_RDWR | O_NONBLOCK);
    record_result(&state, fd >= 0, "device opens read/write in non-blocking mode");
    if (fd < 0)
        goto summary;

    result = write(fd, &extra_byte, 0);
    record_result(&state, result == 0, "zero-length write returns 0");

    result = read(fd, &extra_byte, 0);
    record_result(&state, result == 0, "zero-length read returns 0");

    errno = 0;
    result = read(fd, &extra_byte, 1);
    record_result(&state, result == -1 && errno == EAGAIN,
                  "empty non-blocking read returns EAGAIN");

    write_buffer = malloc(8192U);
    read_buffer = malloc(8192U);
    expected = malloc(8192U);
    if (!write_buffer || !read_buffer || !expected) {
        record_result(&state, false, "test buffers allocate successfully");
        goto close_main_fd;
    }
    record_result(&state, true, "test buffers allocate successfully");

    fill_pattern(write_buffer, 8192U, 3U);
    result = write(fd, write_buffer, QUEUE_CAPACITY);
    record_result(&state, result == (ssize_t)QUEUE_CAPACITY,
                  "exact-capacity write fills the queue");

    errno = 0;
    result = write(fd, &extra_byte, 1);
    record_result(&state, result == -1 && errno == EAGAIN,
                  "full non-blocking write returns EAGAIN");

    memset(read_buffer, 0, 8192U);
    result = read(fd, read_buffer, QUEUE_CAPACITY);
    record_result(&state,
                  result == (ssize_t)QUEUE_CAPACITY &&
                      memcmp(write_buffer, read_buffer, QUEUE_CAPACITY) == 0,
                  "full-buffer read preserves FIFO bytes");

    errno = 0;
    result = read(fd, &extra_byte, 1);
    record_result(&state, result == -1 && errno == EAGAIN,
                  "queue becomes empty after complete drain");

    fill_pattern(write_buffer, 8192U, 4U);
    result = write(fd, write_buffer, QUEUE_CAPACITY + 64U);
    record_result(&state, result == (ssize_t)QUEUE_CAPACITY,
                  "oversized write returns available queue capacity");

    result = read(fd, read_buffer, QUEUE_CAPACITY);
    record_result(&state, result == (ssize_t)QUEUE_CAPACITY,
                  "oversized-write payload can be drained completely");

    fill_pattern(write_buffer, first_write, 5U);
    result = write(fd, write_buffer, first_write);
    record_result(&state, result == (ssize_t)first_write,
                  "wrap test initial write succeeds");

    result = read(fd, read_buffer, first_read);
    record_result(&state,
                  result == (ssize_t)first_read &&
                      memcmp(write_buffer, read_buffer, first_read) == 0,
                  "wrap test initial read succeeds");

    fill_pattern(write_buffer + first_write, second_write, 6U);
    result = write(fd, write_buffer + first_write, second_write);
    record_result(&state, result == (ssize_t)second_write,
                  "write crossing physical ring end succeeds");

    memcpy(expected, write_buffer + first_read, first_write - first_read);
    memcpy(expected + (first_write - first_read),
           write_buffer + first_write, second_write);

    result = read(fd, read_buffer, expected_length);
    record_result(&state,
                  result == (ssize_t)expected_length &&
                      memcmp(expected, read_buffer, expected_length) == 0,
                  "wrap-around preserves FIFO ordering");

close_main_fd:
    close(fd);
    fd = -1;

    if (test_exclusive_open) {
        reader_fd = open(path, O_RDONLY | O_NONBLOCK);
        second_reader_fd = open(path, O_RDONLY | O_NONBLOCK);
        record_result(&state,
                      reader_fd >= 0 && second_reader_fd < 0 && errno == EBUSY,
                      "second concurrent reader is rejected with EBUSY");
        if (second_reader_fd >= 0)
            close(second_reader_fd);
        if (reader_fd >= 0)
            close(reader_fd);

        writer_fd = open(path, O_WRONLY | O_NONBLOCK);
        second_writer_fd = open(path, O_WRONLY | O_NONBLOCK);
        record_result(&state,
                      writer_fd >= 0 && second_writer_fd < 0 && errno == EBUSY,
                      "second concurrent writer is rejected with EBUSY");
        if (second_writer_fd >= 0)
            close(second_writer_fd);
        if (writer_fd >= 0)
            close(writer_fd);
    }

summary:
    if (fd >= 0)
        close(fd);
    free(write_buffer);
    free(read_buffer);
    free(expected);

    printf("  Result: %u passed, %u failed\n", state.passed, state.failed);
    return state.failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void print_usage(const char *program)
{
    fprintf(stderr, "Usage: %s simple|safe|lockfree|all\n", program);
}

int main(int argc, char **argv)
{
    int result = EXIT_SUCCESS;

    if (argc != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "simple") == 0)
        return test_simple_driver();

    if (strcmp(argv[1], "safe") == 0)
        return test_queue_boundaries(SAFE_PATH, "thread_safe_char", false);

    if (strcmp(argv[1], "lockfree") == 0)
        return test_queue_boundaries(LOCK_FREE_PATH, "lock_free_char", true);

    if (strcmp(argv[1], "all") == 0) {
        if (test_simple_driver() != EXIT_SUCCESS)
            result = EXIT_FAILURE;
        if (test_queue_boundaries(SAFE_PATH, "thread_safe_char", false) != EXIT_SUCCESS)
            result = EXIT_FAILURE;
        if (test_queue_boundaries(LOCK_FREE_PATH, "lock_free_char", true) != EXIT_SUCCESS)
            result = EXIT_FAILURE;
        return result;
    }

    print_usage(argv[0]);
    return EXIT_FAILURE;
}
