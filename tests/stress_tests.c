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

#define SAFE_PATH "/dev/thread_safe_char"
#define LOCKFREE_PATH "/dev/lock_free_char"

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

static double elapsed_seconds(const struct timespec *start, const struct timespec *end)
{
	return (double)(end->tv_sec - start->tv_sec) +
	       (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

static uint8_t expected_byte(size_t offset)
{
	return (uint8_t)((offset * 131U + 17U) & 0xffU);
}

static void *stream_writer(void *argument)
{
	struct stream_context *context = argument;
	uint8_t *buffer = malloc(context->chunk_size);
	size_t offset = 0;
	int fd;

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
		size_t requested = context->chunk_size;
		size_t index;
		ssize_t result;

		if (requested > context->total_bytes - offset)
			requested = context->total_bytes - offset;
		for (index = 0; index < requested; ++index)
			buffer[index] = expected_byte(offset + index);

		result = write(fd, buffer, requested);
		atomic_fetch_add(&context->write_calls, 1);
		if (result > 0) {
			if ((size_t)result < requested)
				atomic_fetch_add(&context->short_writes, 1);
			offset += (size_t)result;
			atomic_fetch_add(&context->bytes_written, (unsigned long long)result);
			continue;
		}
		if (result < 0 && errno == EINTR)
			continue;
		atomic_store(&context->error_code, result < 0 ? errno : EIO);
	}

	close(fd);
	free(buffer);
	return NULL;
}

static void *stream_reader(void *argument)
{
	struct stream_context *context = argument;
	uint8_t *buffer = malloc(context->chunk_size);
	size_t offset = 0;
	int fd;

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
		size_t requested = context->chunk_size;
		size_t index;
		ssize_t result;

		if (requested > context->total_bytes - offset)
			requested = context->total_bytes - offset;

		result = read(fd, buffer, requested);
		atomic_fetch_add(&context->read_calls, 1);
		if (result > 0) {
			if ((size_t)result < requested)
				atomic_fetch_add(&context->short_reads, 1);
			for (index = 0; index < (size_t)result; ++index) {
				if (buffer[index] != expected_byte(offset + index))
					atomic_fetch_add(&context->mismatches, 1);
			}
			offset += (size_t)result;
			atomic_fetch_add(&context->bytes_read, (unsigned long long)result);
			continue;
		}
		if (result < 0 && errno == EINTR)
			continue;
		atomic_store(&context->error_code, result < 0 ? errno : EIO);
	}

	close(fd);
	free(buffer);
	return NULL;
}

static int run_stream(const char *driver, size_t mib, size_t chunk_size)
{
	struct stream_context context = {0};
	struct timespec start, end;
	struct rusage usage_start, usage_end;
	pthread_t writer, reader;
	bool passed;
	double seconds;
	double throughput;

	context.path = strcmp(driver, "safe") == 0 ? SAFE_PATH : LOCKFREE_PATH;
	context.total_bytes = mib * 1024U * 1024U;
	context.chunk_size = chunk_size;

	printf("\n[%s SPSC stream benchmark]\n", driver);
	printf("  Transfer: %zu MiB, chunk: %zu bytes\n", mib, chunk_size);

	getrusage(RUSAGE_SELF, &usage_start);
	clock_gettime(CLOCK_MONOTONIC, &start);
	if (pthread_create(&reader, NULL, stream_reader, &context) != 0)
		return EXIT_FAILURE;
	if (pthread_create(&writer, NULL, stream_writer, &context) != 0)
		return EXIT_FAILURE;
	pthread_join(writer, NULL);
	pthread_join(reader, NULL);
	clock_gettime(CLOCK_MONOTONIC, &end);
	getrusage(RUSAGE_SELF, &usage_end);

	seconds = elapsed_seconds(&start, &end);
	throughput = seconds > 0.0 ?
		((double)atomic_load(&context.bytes_read) / (1024.0 * 1024.0)) / seconds : 0.0;
	passed = atomic_load(&context.error_code) == 0 &&
		 atomic_load(&context.bytes_written) == context.total_bytes &&
		 atomic_load(&context.bytes_read) == context.total_bytes &&
		 atomic_load(&context.mismatches) == 0;

	printf("  Bytes written:        %llu\n", atomic_load(&context.bytes_written));
	printf("  Bytes read:           %llu\n", atomic_load(&context.bytes_read));
	printf("  Data mismatches:      %llu\n", atomic_load(&context.mismatches));
	printf("  write() calls:        %llu\n", atomic_load(&context.write_calls));
	printf("  read() calls:         %llu\n", atomic_load(&context.read_calls));
	printf("  Short writes:         %llu\n", atomic_load(&context.short_writes));
	printf("  Short reads:          %llu\n", atomic_load(&context.short_reads));
	printf("  Elapsed:              %.3f s\n", seconds);
	printf("  Throughput:           %.2f MiB/s\n", throughput);
	printf("  Voluntary switches:   %ld\n", usage_end.ru_nvcsw - usage_start.ru_nvcsw);
	printf("  Involuntary switches: %ld\n", usage_end.ru_nivcsw - usage_start.ru_nivcsw);
	printf("  Result: %s\n", passed ? "PASS" : "FAIL");
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

struct producer_context {
	uint8_t token;
	size_t iterations;
	atomic_int *done;
	atomic_int *error;
};

struct consumer_context {
	unsigned long long histogram[256];
	unsigned int producer_count;
	size_t expected_total;
	atomic_ullong *consumed;
	atomic_int *done;
	atomic_int *error;
};

static void *mpmc_producer(void *argument)
{
	struct producer_context *context = argument;
	int fd = open(SAFE_PATH, O_WRONLY | O_NONBLOCK);
	size_t sent = 0;

	if (fd < 0) {
		atomic_store(context->error, errno);
		atomic_fetch_add(context->done, 1);
		return NULL;
	}

	while (sent < context->iterations && atomic_load(context->error) == 0) {
		ssize_t result = write(fd, &context->token, 1);
		if (result == 1) {
			sent++;
			continue;
		}
		if (result < 0 && (errno == EAGAIN || errno == EINTR)) {
			sched_yield();
			continue;
		}
		atomic_store(context->error, result < 0 ? errno : EIO);
	}
	close(fd);
	atomic_fetch_add(context->done, 1);
	return NULL;
}

static void *mpmc_consumer(void *argument)
{
	struct consumer_context *context = argument;
	uint8_t buffer[256];
	int fd = open(SAFE_PATH, O_RDONLY | O_NONBLOCK);

	if (fd < 0) {
		atomic_store(context->error, errno);
		return NULL;
	}

	while (atomic_load(context->error) == 0) {
		ssize_t result;
		size_t index;

		if (atomic_load(context->consumed) >= context->expected_total &&
		    atomic_load(context->done) == (int)context->producer_count)
			break;

		result = read(fd, buffer, sizeof(buffer));
		if (result > 0) {
			for (index = 0; index < (size_t)result; ++index)
				context->histogram[buffer[index]]++;
			atomic_fetch_add(context->consumed, (unsigned long long)result);
			continue;
		}
		if (result < 0 && (errno == EAGAIN || errno == EINTR)) {
			sched_yield();
			continue;
		}
		atomic_store(context->error, result < 0 ? errno : EIO);
	}
	close(fd);
	return NULL;
}

static int run_mpmc(unsigned int producer_count, unsigned int consumer_count,
		    size_t iterations)
{
	pthread_t *producers = calloc(producer_count, sizeof(*producers));
	pthread_t *consumers = calloc(consumer_count, sizeof(*consumers));
	struct producer_context *producer_contexts = calloc(producer_count, sizeof(*producer_contexts));
	struct consumer_context *consumer_contexts = calloc(consumer_count, sizeof(*consumer_contexts));
	atomic_int done = 0;
	atomic_int error = 0;
	atomic_ullong consumed = 0;
	struct timespec start, end;
	unsigned long long histogram[256] = {0};
	size_t expected_total = (size_t)producer_count * iterations;
	unsigned int i, j;
	bool passed = true;
	double seconds;

	if (!producers || !consumers || !producer_contexts || !consumer_contexts)
		return EXIT_FAILURE;

	printf("\n[thread_safe_char MPMC reliability]\n");
	printf("  Producers: %u, consumers: %u, bytes/producer: %zu\n",
	       producer_count, consumer_count, iterations);
	clock_gettime(CLOCK_MONOTONIC, &start);

	for (i = 0; i < consumer_count; ++i) {
		consumer_contexts[i].producer_count = producer_count;
		consumer_contexts[i].expected_total = expected_total;
		consumer_contexts[i].consumed = &consumed;
		consumer_contexts[i].done = &done;
		consumer_contexts[i].error = &error;
		pthread_create(&consumers[i], NULL, mpmc_consumer, &consumer_contexts[i]);
	}
	for (i = 0; i < producer_count; ++i) {
		producer_contexts[i].token = (uint8_t)(i + 1U);
		producer_contexts[i].iterations = iterations;
		producer_contexts[i].done = &done;
		producer_contexts[i].error = &error;
		pthread_create(&producers[i], NULL, mpmc_producer, &producer_contexts[i]);
	}
	for (i = 0; i < producer_count; ++i)
		pthread_join(producers[i], NULL);
	for (i = 0; i < consumer_count; ++i)
		pthread_join(consumers[i], NULL);
	clock_gettime(CLOCK_MONOTONIC, &end);

	for (i = 0; i < consumer_count; ++i)
		for (j = 0; j < 256; ++j)
			histogram[j] += consumer_contexts[i].histogram[j];
	for (i = 0; i < producer_count; ++i)
		if (histogram[i + 1U] != iterations)
			passed = false;
	if (atomic_load(&consumed) != expected_total || atomic_load(&error) != 0)
		passed = false;

	seconds = elapsed_seconds(&start, &end);
	printf("  Expected bytes:       %zu\n", expected_total);
	printf("  Consumed bytes:       %llu\n", atomic_load(&consumed));
	printf("  Elapsed:              %.3f s\n", seconds);
	printf("  Aggregate rate:       %.0f bytes/s\n",
	       seconds > 0 ? (double)atomic_load(&consumed) / seconds : 0.0);
	printf("  Result: %s\n", passed ? "PASS" : "FAIL");

	free(producers);
	free(consumers);
	free(producer_contexts);
	free(consumer_contexts);
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void usage(const char *program)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "  %s stream <safe|lockfree> <MiB> <chunk_bytes>\n", program);
	fprintf(stderr, "  %s mpmc <producers> <consumers> <bytes_per_producer>\n", program);
}

int main(int argc, char **argv)
{
	if (argc == 5 && strcmp(argv[1], "stream") == 0 &&
	    (strcmp(argv[2], "safe") == 0 || strcmp(argv[2], "lockfree") == 0))
		return run_stream(argv[2], strtoul(argv[3], NULL, 10),
				  strtoul(argv[4], NULL, 10));

	if (argc == 5 && strcmp(argv[1], "mpmc") == 0)
		return run_mpmc(strtoul(argv[2], NULL, 10),
				strtoul(argv[3], NULL, 10),
				strtoul(argv[4], NULL, 10));

	usage(argv[0]);
	return EXIT_FAILURE;
}
