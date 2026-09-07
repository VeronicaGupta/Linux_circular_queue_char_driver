CC ?= cc
CFLAGS ?= -Wall -Wextra -Wpedantic -O2 -std=c11
LDFLAGS ?= -pthread
STATIC ?= 0

ifeq ($(STATIC),1)
LDFLAGS += -static
endif

.PHONY: all clean
all: boundary_tests stress_tests

boundary_tests: boundary_tests.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

stress_tests: stress_tests.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f boundary_tests stress_tests
