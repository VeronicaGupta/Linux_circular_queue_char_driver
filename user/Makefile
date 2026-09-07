CC ?= cc
CFLAGS ?= -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=
STATIC ?= 0

ifeq ($(STATIC),1)
LDFLAGS += -static
endif

.PHONY: all clean
all: char_device_demo

char_device_demo: main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f char_device_demo
