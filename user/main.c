// SPDX-License-Identifier: GPL-2.0
/* Shared user-space demonstration program for all three drivers. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define READ_BUFFER_SIZE 4096U

static const char *select_device(const char *name)
{
	if (strcmp(name, "simple") == 0)
		return "/dev/simple_char";
	if (strcmp(name, "safe") == 0)
		return "/dev/thread_safe_char";
	if (strcmp(name, "lockfree") == 0)
		return "/dev/lock_free_char";
	return NULL;
}

static int write_device(const char *path, const char *message)
{
	int fd = open(path, O_WRONLY);
	ssize_t result;

	if (fd < 0) {
		perror("open for write");
		return EXIT_FAILURE;
	}

	result = write(fd, message, strlen(message));
	if (result < 0) {
		perror("write");
		close(fd);
		return EXIT_FAILURE;
	}

	printf("device=%s written=%zd data=\"%.*s\"\n",
	       path, result, (int)result, message);
	close(fd);
	return EXIT_SUCCESS;
}

static int read_device(const char *path, size_t requested)
{
	char buffer[READ_BUFFER_SIZE + 1];
	int fd;
	ssize_t result;

	if (requested == 0 || requested > READ_BUFFER_SIZE)
		requested = READ_BUFFER_SIZE;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open for read");
		return EXIT_FAILURE;
	}

	result = read(fd, buffer, requested);
	if (result < 0) {
		perror("read");
		close(fd);
		return EXIT_FAILURE;
	}

	buffer[result] = '\0';
	printf("device=%s read=%zd data=\"%s\"\n", path, result, buffer);
	close(fd);
	return EXIT_SUCCESS;
}

static void usage(const char *program)
{
	printf("Usage:\n");
	printf("  %s <simple|safe|lockfree> write <text>\n", program);
	printf("  %s <simple|safe|lockfree> read [bytes]\n", program);
	printf("  %s <simple|safe|lockfree> roundtrip <text>\n", program);
}

int main(int argc, char **argv)
{
	const char *path;

	if (argc < 3) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	path = select_device(argv[1]);
	if (!path) {
		fprintf(stderr, "unknown driver: %s\n", argv[1]);
		return EXIT_FAILURE;
	}

	if (strcmp(argv[2], "write") == 0 && argc == 4)
		return write_device(path, argv[3]);

	if (strcmp(argv[2], "read") == 0) {
		size_t requested = argc == 4 ? strtoul(argv[3], NULL, 10) : READ_BUFFER_SIZE;
		return read_device(path, requested);
	}

	if (strcmp(argv[2], "roundtrip") == 0 && argc == 4) {
		if (write_device(path, argv[3]) != EXIT_SUCCESS)
			return EXIT_FAILURE;
		return read_device(path, strlen(argv[3]));
	}

	usage(argv[0]);
	return EXIT_FAILURE;
}
