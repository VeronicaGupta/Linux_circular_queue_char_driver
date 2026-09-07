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
#define LOCKFREE_PATH "/dev/lock_free_char"
#define SIMPLE_CAPACITY 1024U
#define FIFO_CAPACITY 4096U

struct result_count {
	unsigned int passed;
	unsigned int failed;
};

static void check(struct result_count *results, bool condition, const char *name)
{
	printf("  %-54s %s\n", name, condition ? "PASS" : "FAIL");
	if (condition)
		results->passed++;
	else
		results->failed++;
}

static void fill_pattern(uint8_t *buffer, size_t length, unsigned int seed)
{
	size_t index;
	for (index = 0; index < length; ++index)
		buffer[index] = (uint8_t)((index * 37U + seed * 19U) & 0xffU);
}

static bool drain_fifo(const char *path)
{
	uint8_t buffer[512];
	int fd = open(path, O_RDONLY | O_NONBLOCK);

	if (fd < 0)
		return false;

	for (;;) {
		ssize_t result = read(fd, buffer, sizeof(buffer));
		if (result > 0)
			continue;
		if (result < 0 && errno == EINTR)
			continue;
		if (result < 0 && errno == EAGAIN)
			break;
		if (result == 0)
			break;
		close(fd);
		return false;
	}

	close(fd);
	return true;
}

static int test_simple(void)
{
	struct result_count results = {0};
	uint8_t write_buffer[SIMPLE_CAPACITY + 64];
	uint8_t read_buffer[SIMPLE_CAPACITY + 64];
	int fd;
	ssize_t result;

	printf("\n[simple_char boundary tests]\n");
	fill_pattern(write_buffer, sizeof(write_buffer), 1);

	fd = open(SIMPLE_PATH, O_WRONLY);
	check(&results, fd >= 0, "open for write");
	if (fd < 0)
		goto done;

	result = write(fd, write_buffer, 0);
	check(&results, result == 0, "zero-length write returns 0");
	result = write(fd, write_buffer, SIMPLE_CAPACITY + 64);
	check(&results, result == SIMPLE_CAPACITY, "oversized write truncates to capacity");
	errno = 0;
	result = write(fd, write_buffer, 1);
	check(&results, result == -1 && errno == ENOSPC, "write after capacity returns ENOSPC");
	close(fd);

	fd = open(SIMPLE_PATH, O_RDONLY);
	check(&results, fd >= 0, "open for read");
	if (fd < 0)
		goto done;

	result = read(fd, read_buffer, SIMPLE_CAPACITY);
	check(&results,
	      result == SIMPLE_CAPACITY && memcmp(read_buffer, write_buffer, SIMPLE_CAPACITY) == 0,
	      "capacity read preserves bytes");
	result = read(fd, read_buffer, 1);
	check(&results, result == 0, "read past stored bytes returns EOF");
	close(fd);

done:
	printf("  Result: %u passed, %u failed\n", results.passed, results.failed);
	return results.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int test_fifo(const char *path, const char *label, bool exclusive)
{
	struct result_count results = {0};
	uint8_t *source = malloc(8192);
	uint8_t *received = malloc(8192);
	uint8_t *expected = malloc(8192);
	uint8_t one = 0x5a;
	int fd = -1;
	ssize_t result;
	const size_t first_write = 3000;
	const size_t first_read = 2000;
	const size_t second_write = 3000;
	const size_t expected_size = first_write - first_read + second_write;

	printf("\n[%s boundary tests]\n", label);
	check(&results, source && received && expected, "allocate test buffers");
	if (!source || !received || !expected)
		goto done;

	check(&results, drain_fifo(path), "drain queue before test");

	fd = open(path, O_RDWR | O_NONBLOCK);
	check(&results, fd >= 0, "open O_RDWR | O_NONBLOCK");
	if (fd < 0)
		goto done;

	result = read(fd, &one, 0);
	check(&results, result == 0, "zero-length read returns 0");
	result = write(fd, &one, 0);
	check(&results, result == 0, "zero-length write returns 0");

	errno = 0;
	result = read(fd, &one, 1);
	check(&results, result == -1 && errno == EAGAIN, "empty nonblocking read returns EAGAIN");

	fill_pattern(source, 8192, 2);
	result = write(fd, source, FIFO_CAPACITY);
	check(&results, result == FIFO_CAPACITY, "exact-capacity write fills FIFO");
	errno = 0;
	result = write(fd, &one, 1);
	check(&results, result == -1 && errno == EAGAIN, "full nonblocking write returns EAGAIN");

	result = read(fd, received, FIFO_CAPACITY);
	check(&results,
	      result == FIFO_CAPACITY && memcmp(source, received, FIFO_CAPACITY) == 0,
	      "full FIFO drains in order");

	fill_pattern(source, 8192, 3);
	result = write(fd, source, first_write);
	check(&results, result == (ssize_t)first_write, "wrap test first write");
	result = read(fd, received, first_read);
	check(&results, result == (ssize_t)first_read, "wrap test partial read");
	result = write(fd, source + first_write, second_write);
	check(&results, result == (ssize_t)second_write, "wrap test second write crosses ring end");

	memcpy(expected, source + first_read, first_write - first_read);
	memcpy(expected + first_write - first_read,
	       source + first_write, second_write);
	result = read(fd, received, expected_size);
	check(&results,
	      result == (ssize_t)expected_size && memcmp(received, expected, expected_size) == 0,
	      "wrap-around preserves FIFO ordering");

	if (exclusive) {
		int reader = open(path, O_RDONLY | O_NONBLOCK);
		int writer = open(path, O_WRONLY | O_NONBLOCK);
		check(&results, reader < 0 && errno == EBUSY,
		      "second reader rejected while O_RDWR owner is open");
		check(&results, writer < 0 && errno == EBUSY,
		      "second writer rejected while O_RDWR owner is open");
		if (reader >= 0)
			close(reader);
		if (writer >= 0)
			close(writer);
	}

	close(fd);
	fd = -1;

done:
	if (fd >= 0)
		close(fd);
	free(source);
	free(received);
	free(expected);
	printf("  Result: %u passed, %u failed\n", results.passed, results.failed);
	return results.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void usage(const char *program)
{
	fprintf(stderr, "Usage: %s <simple|safe|lockfree|all>\n", program);
}

int main(int argc, char **argv)
{
	int status = EXIT_SUCCESS;

	if (argc != 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	if (strcmp(argv[1], "simple") == 0 || strcmp(argv[1], "all") == 0)
		status |= test_simple();
	if (strcmp(argv[1], "safe") == 0 || strcmp(argv[1], "all") == 0)
		status |= test_fifo(SAFE_PATH, "thread_safe_char", false);
	if (strcmp(argv[1], "lockfree") == 0 || strcmp(argv[1], "all") == 0)
		status |= test_fifo(LOCKFREE_PATH, "lock_free_char", true);

	if (strcmp(argv[1], "simple") != 0 && strcmp(argv[1], "safe") != 0 &&
	    strcmp(argv[1], "lockfree") != 0 && strcmp(argv[1], "all") != 0) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	return status ? EXIT_FAILURE : EXIT_SUCCESS;
}
