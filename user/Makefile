CC ?= gcc
CFLAGS ?= -Wall -Wextra -Wpedantic -O2
TARGET := char_device_demo

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
