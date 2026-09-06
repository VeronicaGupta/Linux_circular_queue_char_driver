# Linux Character Device Driver Lab — Windows + Docker + QEMU

A compact learning project for Linux character-device drivers while developing on Windows. Three virtual drivers use the same Unix character-device interface but progressively change the synchronization model:

1. `simple_char` — explicit major/minor registration and a fixed kernel buffer; intentionally unsynchronized.
2. `thread_safe_char` — bounded circular byte FIFO protected by a mutex and wait queues; supports multiple producers and consumers.
3. `lock_free_char` — bounded SPSC circular byte FIFO using acquire/release ordering; one active reader and one active writer.

The existing driver implementations are kept unchanged. Docker and QEMU provide the Linux build/test environment around them.

## Why Docker and QEMU are both used

A Docker Linux container provides GCC, Kbuild, Linux headers, BusyBox, and QEMU, but a container alone is not a separate kernel on Windows. QEMU therefore boots a stock Linux kernel that matches the installed kernel headers. The `.ko` files are built against that exact kernel and loaded inside the QEMU guest.

```text
Windows
  |
  v
Docker Desktop (Linux container)
  |-- GCC / make / Kbuild
  |-- Ubuntu kernel image + matching headers
  |-- BusyBox + cpio
  `-- QEMU
        |
        v
     Linux guest kernel
        |
        |-- insmod simple_char.ko
        |-- insmod thread_safe_char.ko
        |-- insmod lock_free_char.ko
        `-- /dev/simple_char
            /dev/thread_safe_char
            /dev/lock_free_char
```

This follows the same development pattern as the reference project at:

- https://github.com/czhao-dev/linux-device-drivers/tree/main/linux-character-device-driver

The reference builds an out-of-tree module inside Docker, packages the module and static tests into a BusyBox initramfs, and boots a matching Linux kernel under QEMU for real `insmod`/`rmmod` testing.

## Repository layout

```text
.
├── 01_simple_char/
│   ├── simple_char.c
│   ├── simple_char.h
│   └── Makefile
├── 02_thread_safe_char/
│   ├── thread_safe_char.c
│   ├── thread_safe_char.h
│   └── Makefile
├── 03_lock_free_char/
│   ├── lock_free_char.c
│   ├── lock_free_char.h
│   └── Makefile
├── user/
│   ├── main.c
│   └── Makefile
├── tests/
│   ├── boundary_tests.c
│   ├── stress_tests.c
│   └── Makefile
├── docker/
│   ├── Dockerfile
│   ├── lab.sh
│   ├── init-test.sh
│   ├── init-shell.sh
│   ├── run.ps1
│   ├── run.sh
│   └── output/
├── driver-lab.cmd
├── Makefile
└── README.md
```

## Windows prerequisites

Docker Desktop must be installed and running in **Linux container mode**. The WSL 2 backend is the normal Windows setup.

Verify from PowerShell:

```powershell
docker version
docker run --rm hello-world
```

No Windows installation of GCC, Linux headers, QEMU, or `make` is required.

## One-command automated test

From PowerShell at the repository root:

```powershell
.\driver-lab.cmd test
```

The launcher:

1. builds the Docker image;
2. installs/uses an Ubuntu 24.04 generic kernel and matching headers inside the image;
3. builds all three kernel modules through Kbuild;
4. compiles the user program and tests as static Linux executables;
5. creates a BusyBox initramfs;
6. boots the matching kernel with `qemu-system-x86_64`;
7. loads the three `.ko` modules;
8. runs boundary and stress tests;
9. scans kernel diagnostics;
10. unloads the modules and powers off.

A successful run ends with:

```text
DRIVER LAB RESULT: PASS
Docker/QEMU test result: PASS
```

The serial log is saved as:

```text
docker/output/latest.log
```

## Build without booting QEMU

```powershell
.\driver-lab.cmd build
```

Generated artifacts are copied to:

```text
docker/output/build/
├── kernel-version.txt
├── modules/
│   ├── simple_char.ko
│   ├── thread_safe_char.ko
│   └── lock_free_char.ko
└── bin/
    ├── char_device_demo
    ├── boundary_tests
    └── stress_tests
```

The `.ko` files are for the QEMU guest kernel, not the Windows/WSL host kernel.

## Interactive learning mode

The most useful mode while learning is:

```powershell
.\driver-lab.cmd shell
```

QEMU boots to a BusyBox shell. No module is loaded automatically, allowing the complete driver lifecycle to be practiced manually.

### Simple character driver

```sh
uname -a
ls /modules

insmod /modules/simple_char.ko
lsmod
ls -l /dev/simple_char
cat /proc/devices

/bin/char_device_demo simple roundtrip HELLO
/bin/boundary_tests simple

dmesg | tail -30
rmmod simple_char
```

Concepts visible in this stage:

```text
alloc_chrdev_region()
       |
       v
major/minor device number
       |
       v
cdev_init() + cdev_add()
       |
       v
class_create() + device_create()
       |
       v
/dev/simple_char
       |
       v
file_operations -> open/read/write/release
```

### Thread-safe circular character driver

```sh
insmod /modules/thread_safe_char.ko

/bin/char_device_demo safe read 5 &
/bin/char_device_demo safe write HELLO
wait

/bin/boundary_tests safe
/bin/stress_tests stream safe 8 1024
/bin/stress_tests mpmc 4 4 10000

rmmod thread_safe_char
```

The blocking experiment shows the reader sleeping on a wait queue until the writer deposits data.

```text
reader                         writer
  |                              |
read()                         write()
  |                              |
queue empty                      |
  |                              |
wait_event_interruptible()       |
  |                              |
  |                         mutex_lock()
  |                         enqueue bytes
  |                         mutex_unlock()
  |                              |
  |<--- wake_up_interruptible()--|
  |
consume bytes
```

### Lock-free SPSC circular character driver

```sh
insmod /modules/lock_free_char.ko

/bin/char_device_demo lockfree read 5 &
/bin/char_device_demo lockfree write HELLO
wait

/bin/boundary_tests lockfree
/bin/stress_tests stream lockfree 8 1024

rmmod lock_free_char
```

The queue synchronization model changes to ownership rather than a mutex:

```text
single reader                    single writer
     |                                |
owns read_index                 owns write_index
     |                                |
     `------- acquire/release --------'
```

`volatile` is not used as a synchronization mechanism. The published ring indices use kernel memory-ordering primitives so buffer accesses happen before/after index publication as required.

## Automated tests

### Boundary tests

```sh
/bin/boundary_tests all
```

Coverage includes:

- zero-length read/write;
- exact-capacity transfer;
- oversized transfer;
- EOF behavior for the simple device;
- `EAGAIN` on empty/full non-blocking FIFO operations;
- circular-buffer wrap-around and FIFO ordering;
- `EBUSY` for a second reader or writer on the SPSC lock-free driver.

### SPSC stream stress

```sh
/bin/stress_tests stream safe 8 1024
/bin/stress_tests stream lockfree 8 1024
```

Pass criteria:

- requested bytes written exactly;
- requested bytes read exactly;
- zero data mismatches;
- zero unexpected errors;
- 100% verified reliability.

Metrics printed include elapsed time, MiB/s, syscall counts, short reads/writes, and context switches.

QEMU/TCG timing should be treated as a controlled comparative experiment, not native-hardware throughput.

### MPMC reliability

```sh
/bin/stress_tests mpmc 4 4 10000
```

This is intended for `thread_safe_char`. Multiple producers and consumers concurrently exercise the shared mutex-protected FIFO.

### Unsynchronized race observation

```sh
/bin/stress_tests simple-race 4 10
```

`simple_char` intentionally has no synchronization. The test observes concurrent overlapping writers rather than claiming thread safety.

## Driver trade-offs

| Implementation | Concurrency | Main advantage | Main cost / restriction |
|---|---|---|---|
| `simple_char` | Unsupported | Minimal character-driver plumbing | Shared state races under concurrent access |
| `thread_safe_char` | MPMC | Straightforward correctness and blocking semantics | Mutex serialization and contention |
| `lock_free_char` | SPSC | Producer and consumer do not serialize on one queue mutex | Exactly one reader and one writer; memory ordering is harder to reason about |

The lock-free design should not be described merely as “faster.” Its principal trade-off is reduced synchronization contention in exchange for a much narrower concurrency contract and more subtle correctness requirements.

## Useful launcher commands

```powershell
.\driver-lab.cmd image   # build only the Docker image
.\driver-lab.cmd build   # compile modules and static tools
.\driver-lab.cmd test    # full automated QEMU run
.\driver-lab.cmd shell   # interactive QEMU learning shell
.\driver-lab.cmd clean   # clear docker/output artifacts
```

PowerShell can also invoke the launcher directly:

```powershell
.\docker\run.ps1 test
```

Git Bash/WSL users can use:

```sh
./docker/run.sh test
```

## Kernel module build model

Each driver retains the standard external-module Kbuild pattern:

```make
obj-m += simple_char.o

all:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" modules
```

The Docker harness sets `KDIR` to the headers belonging to the same kernel image that QEMU later boots. This avoids the common module-version mismatch caused by building against one kernel and loading into another.

Official Kbuild documentation:

- https://docs.kernel.org/next/kbuild/modules.html

## Reference project

The Docker/QEMU structure is based on the testing pattern demonstrated by:

- https://github.com/czhao-dev/linux-device-drivers/tree/main/linux-character-device-driver

The source code in the three driver folders remains the existing project implementation; only the surrounding Windows Docker/QEMU workflow is added.
