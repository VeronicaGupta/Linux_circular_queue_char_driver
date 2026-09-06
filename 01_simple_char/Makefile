SHELL := /bin/bash

obj-m += simple_char.o

KDIR ?= /lib/modules/$(shell uname -r)/build
MODULE_DIR := $(CURDIR)

.PHONY: all check-kdir clean info

all: check-kdir
	$(MAKE) -C "$(KDIR)" M="$(MODULE_DIR)" modules

check-kdir:
	@if [[ ! -f "$(KDIR)/Makefile" ]]; then \
		echo "ERROR: kernel build tree not found: $(KDIR)"; \
		echo "Provide the exact prepared target-kernel tree:"; \
		echo "  make KDIR=/absolute/path/to/kernel-build-tree"; \
		exit 2; \
	fi

info:
	@echo "Module: simple_char.ko"
	@echo "Kernel: $(shell uname -r)"
	@echo "KDIR:   $(KDIR)"

clean:
	@if [[ -f "$(KDIR)/Makefile" ]]; then \
		$(MAKE) -C "$(KDIR)" M="$(MODULE_DIR)" clean; \
	else \
		rm -rf *.o *.ko *.mod *.mod.c .*.cmd Module.symvers modules.order \
		       .tmp_versions .cache.mk; \
	fi
