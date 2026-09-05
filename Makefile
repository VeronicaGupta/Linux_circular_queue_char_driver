.PHONY: all modules user tests test clean load-simple load-safe load-lockfree unload

all: modules user tests

modules:
	$(MAKE) -C 01_simple_char
	$(MAKE) -C 02_thread_safe_char
	$(MAKE) -C 03_lock_free_char

user:
	$(MAKE) -C user

tests:
	$(MAKE) -C tests

test: modules tests
	./tests/run_tests.sh

load-simple:
	sudo insmod 01_simple_char/simple_char.ko

load-safe:
	sudo insmod 02_thread_safe_char/thread_safe_char.ko

load-lockfree:
	sudo insmod 03_lock_free_char/lock_free_char.ko

unload:
	-sudo rmmod lock_free_char
	-sudo rmmod thread_safe_char
	-sudo rmmod simple_char

clean:
	$(MAKE) -C 01_simple_char clean
	$(MAKE) -C 02_thread_safe_char clean
	$(MAKE) -C 03_lock_free_char clean
	$(MAKE) -C user clean
	$(MAKE) -C tests clean
