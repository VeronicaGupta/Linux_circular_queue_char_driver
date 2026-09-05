CC ?= gcc
CFLAGS ?= -Wall -Wextra -Wpedantic -O2 -std=c11
LDLIBS := -pthread

TARGETS := boundary_tests stress_tests

.PHONY: all clean

all: $(TARGETS)

boundary_tests: boundary_tests.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

stress_tests: stress_tests.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGETS)
