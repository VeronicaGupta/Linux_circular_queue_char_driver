# Linux Character Device Driver Lab

A compact learning project containing three Linux character drivers that expose the same user-space file API while using different synchronization designs.

## Drivers

| Driver | Device node | Design | Intended concurrency |
|---|---|---|---|
| `simple_char` | `/dev/simple_char` | Fixed byte buffer, explicit major/minor + `cdev` | Educational only; no synchronization |
| `thread_safe_char` | `/dev/thread_safe_char` | Circular byte FIFO, mutex + wait queues | Multiple producers / multiple consumers |
| `lock_free_char` | `/dev/lock_free_char` | Circular byte FIFO, acquire/release index publication | Single producer / single consumer |

The queue drivers are byte-stream FIFOs. Individual `write()` calls are not preserved as message boundaries.

## Why three versions

The simple driver isolates the Linux character-device plumbing: device-number allocation, `cdev`, `class_create()`, `device_create()`, `file_operations`, and user/kernel copies. It deliberately omits synchronization so that registration and syscall dispatch remain visible.

The thread-safe driver adds a bounded circular FIFO. A mutex protects the shared read index, write index, occupancy, and buffer mutation. Wait queues block readers when the FIFO is empty and writers when it is full. `O_NONBLOCK` converts those waits into `-EAGAIN`.

The lock-free version removes the FIFO mutex by narrowing the contract to SPSC. The reader exclusively publishes `read_index`; the writer exclusively publishes `write_index`. `smp_store_release()` publishes completed buffer updates and `smp_load_acquire()` observes them in the required order. Extra readers or writers are rejected with `-EBUSY`.

## Trade-offs

The unsynchronized implementation has the smallest code path but cannot guarantee correct concurrent access. The mutex design is straightforward and supports MPMC use, but lock contention and scheduler wakeups can increase latency under load. The SPSC lock-free design allows one producer and one consumer to progress without a queue lock, but requires strict ownership, explicit memory ordering, and rejects broader concurrency.

Lock-free should not be assumed to be faster in every workload. Small transfers emphasize synchronization and syscall overhead; large transfers increasingly emphasize `copy_to_user()`/`copy_from_user()` and memory movement. Benchmark results should be interpreted under identical workloads.

# Docker + QEMU workflow

The Docker image contains Ubuntu kernel headers, the matching kernel image, GCC/Kbuild, BusyBox, and QEMU. The three modules are built against that kernel, packaged with static test binaries into an initramfs, and loaded inside a QEMU Linux guest.

Docker provides the reproducible build environment. QEMU provides the actual Linux kernel under test. A normal container alone is insufficient for portable kernel-module testing because containers share the host kernel.

## Build the Docker image

```bash
docker build -t char-driver-lab -f docker/Dockerfile .
```

## Automated build and test

```bash
docker run --rm char-driver-lab test
```

The automated guest performs:

- module insertion and removal;
- `/dev` node verification;
- basic `open/read/write` exercises;
- boundary tests;
- SPSC integrity/throughput tests for mutex and lock-free drivers;
- MPMC reliability testing for the mutex driver;
- kernel-log inspection.

A successful run ends with:

```text
DRIVER LAB RESULT: PASS
Docker/QEMU test result: PASS
```

## Interactive learning shell

```bash
docker run --rm -it char-driver-lab shell
```

Inside the QEMU guest:

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

Thread-safe FIFO:

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

Lock-free SPSC FIFO:

```sh
insmod /modules/lock_free_char.ko
/bin/char_device_demo lockfree read 5 &
/bin/char_device_demo lockfree write HELLO
wait
/bin/boundary_tests lockfree
/bin/stress_tests stream lockfree 8 1024
rmmod lock_free_char
```

Exit the guest with:

```sh
poweroff -f
```

## Convenience launchers

Linux / EC2:

```bash
chmod +x driver-lab.sh
./driver-lab.sh image
./driver-lab.sh test
./driver-lab.sh shell
```

Windows PowerShell / Command Prompt with Docker Desktop:

```powershell
.\driver-lab.cmd image
.\driver-lab.cmd test
.\driver-lab.cmd shell
```

The launchers mount `docker/output/` so QEMU logs and generated artifacts persist on the host.

# Native Linux workflow

When matching headers for the running kernel are available, Docker/QEMU is optional.

```bash
make
sudo insmod 01_simple_char/simple_char.ko
./user/char_device_demo simple roundtrip HELLO
sudo rmmod simple_char
```

Then repeat with the safe and lock-free modules.

# Benchmarking

SPSC comparison:

```sh
/bin/stress_tests stream safe 64 64
/bin/stress_tests stream lockfree 64 64

/bin/stress_tests stream safe 64 1024
/bin/stress_tests stream lockfree 64 1024

/bin/stress_tests stream safe 64 4096
/bin/stress_tests stream lockfree 64 4096
```

Metrics reported include elapsed time, throughput, syscall counts, short I/O, context switches, byte counts, and corruption count.

MPMC reliability for the mutex implementation:

```sh
/bin/stress_tests mpmc 1 1 100000
/bin/stress_tests mpmc 2 2 100000
/bin/stress_tests mpmc 4 4 100000
```

Pass criteria are correctness-based: exact byte counts, zero corruption, and zero unexpected errors. No test assumes the lock-free implementation must outperform the mutex implementation.

# Repository structure

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
│   └── output/
├── driver-lab.sh
├── driver-lab.cmd
├── Makefile
└── README.md
```

# Important implementation note

All driver callbacks use module-specific names such as `simple_char_open()`. This avoids collisions with existing kernel symbols such as Linux's own `simple_open()` declared in `<linux/fs.h>`.
