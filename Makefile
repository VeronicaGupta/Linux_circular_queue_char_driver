KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all modules user tests clean docker-image docker-build docker-test docker-shell

all: modules user tests

modules:
	$(MAKE) -C 01_simple_char KDIR="$(KDIR)"
	$(MAKE) -C 02_thread_safe_char KDIR="$(KDIR)"
	$(MAKE) -C 03_lock_free_char KDIR="$(KDIR)"

user:
	$(MAKE) -C user

tests:
	$(MAKE) -C tests

clean:
	$(MAKE) -C 01_simple_char clean KDIR="$(KDIR)" || true
	$(MAKE) -C 02_thread_safe_char clean KDIR="$(KDIR)" || true
	$(MAKE) -C 03_lock_free_char clean KDIR="$(KDIR)" || true
	$(MAKE) -C user clean
	$(MAKE) -C tests clean

docker-image:
	docker build -t char-driver-lab -f docker/Dockerfile .

docker-build:
	./driver-lab.sh build

docker-test:
	./driver-lab.sh test

docker-shell:
	./driver-lab.sh shell
