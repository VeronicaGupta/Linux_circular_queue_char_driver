// SPDX-License-Identifier: GPL-2.0
/* User-space demonstration program for all three character devices. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define READ_BUFFER_SIZE 4096

static const char *device_path(const char *driver_name)
{
	if (strcmp(driver_name, "simple") == 0)
		return "/dev/simple_char";

	if (strcmp(driver_name, "safe") == 0)
		return "/dev/thread_safe_char";

	if (strcmp(driver_name, "lockfree") == 0)
		return "/dev/lock_free_char";

	return NULL;
}

static int write_device(const char *path, const char *message)
{
	int file_descriptor;
	ssize_t bytes_written;
	size_t message_length = strlen(message);

	file_descriptor = open(path, O_WRONLY);
	if (file_descriptor < 0) {
		perror("open for write");
		return EXIT_FAILURE;
	}

	bytes_written = write(file_descriptor, message, message_length);
	if (bytes_written < 0) {
		perror("write");
		close(file_descriptor);
		return EXIT_FAILURE;
	}

	printf("device=%s written=%zd data=\"%.*s\"\n",
	       path, bytes_written, (int)bytes_written, message);

	close(file_descriptor);
	return EXIT_SUCCESS;
}

static int read_device(const char *path, size_t requested_bytes)
{
	char buffer[READ_BUFFER_SIZE + 1];
	int file_descriptor;
	ssize_t bytes_read;
	size_t read_size = requested_bytes;

	if (read_size == 0 || read_size > READ_BUFFER_SIZE)
		read_size = READ_BUFFER_SIZE;

	file_descriptor = open(path, O_RDONLY);
	if (file_descriptor < 0) {
		perror("open for read");
		return EXIT_FAILURE;
	}

	bytes_read = read(file_descriptor, buffer, read_size);
	if (bytes_read < 0) {
		perror("read");
		close(file_descriptor);
		return EXIT_FAILURE;
	}

	buffer[bytes_read] = '\0';
	printf("device=%s read=%zd data=\"%s\"\n", path, bytes_read, buffer);

	close(file_descriptor);
	return EXIT_SUCCESS;
}

static int roundtrip_device(const char *path, const char *message)
{
	int result;

	result = write_device(path, message);
	if (result != EXIT_SUCCESS)
		return result;

	return read_device(path, strlen(message));
}

static void print_usage(const char *program_name)
{
	printf("Usage:\n");
	printf("  %s <simple|safe|lockfree> write <text>\n", program_name);
	printf("  %s <simple|safe|lockfree> read [bytes]\n", program_name);
	printf("  %s <simple|safe|lockfree> roundtrip <text>\n", program_name);
}

int main(int argc, char *argv[])
{
	const char *path;
	size_t requested_bytes;

	if (argc < 3) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	path = device_path(argv[1]);
	if (!path) {
		fprintf(stderr, "unknown driver: %s\n", argv[1]);
		return EXIT_FAILURE;
	}

	if (strcmp(argv[2], "write") == 0) {
		if (argc != 4) {
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
		return write_device(path, argv[3]);
	}

	if (strcmp(argv[2], "read") == 0) {
		requested_bytes = (argc == 4) ? strtoul(argv[3], NULL, 10)
					      : READ_BUFFER_SIZE;
		return read_device(path, requested_bytes);
	}

	if (strcmp(argv[2], "roundtrip") == 0) {
		if (argc != 4) {
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
		return roundtrip_device(path, argv[3]);
	}

	print_usage(argv[0]);
	return EXIT_FAILURE;
}
