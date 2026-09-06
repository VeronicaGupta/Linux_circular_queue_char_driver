SHELL := /bin/bash

KDIR ?= /lib/modules/$(shell uname -r)/build
KERNEL_RELEASE := $(shell uname -r)
KERNEL_BUILD_READY := $(shell test -f "$(KDIR)/Makefile" && echo 1 || echo 0)

.PHONY: all fydeos userspace user tests modules optional-modules native \
        env check-kdir check-modules test clean load-simple load-safe \
        load-lockfree unload help

# Safe default for FydeOS/Crostini: always build user-space code and tests.
# Kernel modules are built automatically only when a valid KDIR is available.
all: userspace optional-modules
	@echo
	@echo "Build complete."
	@echo "  User application: user/char_device_demo"
	@echo "  Test binaries:    tests/boundary_tests, tests/stress_tests"
	@if [[ "$(KERNEL_BUILD_READY)" == "1" ]]; then \
		echo "  Kernel modules:   built against $(KDIR)"; \
	else \
		echo "  Kernel modules:   skipped (no prepared kernel tree at $(KDIR))"; \
	fi

# Explicit FydeOS/Crostini target. Kernel modules are not attempted.
fydeos: userspace env
	@echo
	@echo "FydeOS/Crostini build completed."
	@echo "Kernel-module runtime testing requires a compatible kernel environment."

userspace: user tests

user:
	$(MAKE) -C user

tests:
	$(MAKE) -C tests

optional-modules:
ifeq ($(KERNEL_BUILD_READY),1)
	$(MAKE) modules KDIR="$(KDIR)"
else
	@echo
	@echo "[SKIP] Kernel module build"
	@echo "       Running kernel: $(KERNEL_RELEASE)"
	@echo "       Missing build tree: $(KDIR)"
	@echo "       FydeOS/Crostini can still build user/ and tests/."
	@echo "       With an exact prepared kernel tree:"
	@echo "         make modules KDIR=/absolute/path/to/kernel-build-tree"
endif

# Build all three modules. KDIR must match the target kernel.
modules: check-kdir
	$(MAKE) -C 01_simple_char KDIR="$(KDIR)"
	$(MAKE) -C 02_thread_safe_char KDIR="$(KDIR)"
	$(MAKE) -C 03_lock_free_char KDIR="$(KDIR)"

# Full build intended for native Linux or a VM with matching headers.
native: modules userspace

check-kdir:
	@if [[ ! -f "$(KDIR)/Makefile" ]]; then \
		echo "ERROR: kernel build tree is unavailable."; \
		echo "Kernel: $(KERNEL_RELEASE)"; \
		echo "KDIR:   $(KDIR)"; \
		echo; \
		echo "On standard Debian/Ubuntu:"; \
		echo "  sudo apt install build-essential linux-headers-\$$(uname -r)"; \
		echo; \
		echo "On FydeOS/Crostini, the running kernel is not supplied by Debian."; \
		echo "Use 'make fydeos' for user-space builds, or provide an exact prepared"; \
		echo "kernel tree with KDIR=/path/to/kernel-tree."; \
		exit 2; \
	fi

check-modules:
	@for module in \
		01_simple_char/simple_char.ko \
		02_thread_safe_char/thread_safe_char.ko \
		03_lock_free_char/lock_free_char.ko; do \
		if [[ ! -f "$$module" ]]; then \
			echo "ERROR: missing $$module"; \
			echo "Build modules first in an environment with a matching kernel tree."; \
			exit 2; \
		fi; \
	done

# Runs the actual driver tests. This requires loadable .ko modules and sudo.
test: tests check-modules
	./tests/run_tests.sh

env:
	@./scripts/check_environment.sh

load-simple: check-modules
	sudo insmod 01_simple_char/simple_char.ko

load-safe: check-modules
	sudo insmod 02_thread_safe_char/thread_safe_char.ko

load-lockfree: check-modules
	sudo insmod 03_lock_free_char/lock_free_char.ko

unload:
	-sudo rmmod lock_free_char
	-sudo rmmod thread_safe_char
	-sudo rmmod simple_char

clean:
	$(MAKE) -C 01_simple_char clean KDIR="$(KDIR)"
	$(MAKE) -C 02_thread_safe_char clean KDIR="$(KDIR)"
	$(MAKE) -C 03_lock_free_char clean KDIR="$(KDIR)"
	$(MAKE) -C user clean
	$(MAKE) -C tests clean

help:
	@echo "Targets:"
	@echo "  make                 Build user-space code/tests; build modules only if KDIR exists"
	@echo "  make fydeos          FydeOS/Crostini-safe user-space build + environment report"
	@echo "  make modules         Build all .ko modules using the current/default KDIR"
	@echo "  make modules KDIR=…  Build against an explicitly prepared kernel tree"
	@echo "  make native          Build modules + user-space code + tests"
	@echo "  make test            Load modules and run boundary/stress tests"
	@echo "  make env             Print kernel/build-environment diagnostics"
	@echo "  make clean           Remove build artifacts even when KDIR is unavailable"
