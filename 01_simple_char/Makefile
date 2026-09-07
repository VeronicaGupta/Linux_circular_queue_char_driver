obj-m += simple_char.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)

.PHONY: all clean
all:
	$(MAKE) -C "$(KDIR)" M="$(PWD)" modules

clean:
	@if [ -d "$(KDIR)" ]; then \
		$(MAKE) -C "$(KDIR)" M="$(PWD)" clean; \
	else \
		rm -f *.o *.ko *.mod *.mod.c *.symvers modules.order Module.symvers .*.cmd; \
		rm -rf .tmp_versions; \
	fi
