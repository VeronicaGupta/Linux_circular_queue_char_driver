# Linux Character Device Driver Lab

A compact Linux kernel project that implements the same character-device interface with three synchronization designs: an intentionally unsynchronized baseline, a mutex/wait-queue circular FIFO, and a lock-free SPSC circular FIFO. The drivers are exercised through reproducible Docker + QEMU tests and retained AWS/Linux execution artifacts.

## Validated Results and Project Achievements

The project was built and exercised in an **AWS EC2 → Docker → QEMU Linux** environment. Docker provides a reproducible toolchain and matching kernel build environment; QEMU provides the isolated Linux kernel in which the out-of-tree modules are actually loaded and tested.

### Why the benchmark results are meaningful

Kernel queue performance is useful only after correctness is established. A faster FIFO that loses, duplicates, reorders, or corrupts bytes is not a successful synchronization design. The benchmark therefore uses the following order of evidence:

1. **Boundary correctness** — empty/full behavior, wrap-around, non-blocking I/O, exact-capacity transfers, and ownership restrictions must pass first.
2. **Data integrity under sustained transfer** — the mutex and lock-free implementations must transfer the same byte count with zero mismatches and zero short I/O before throughput is compared.
3. **Equivalent SPSC workload** — both synchronized implementations are compared using the same **8 MiB transfer with 1024-byte chunks**, producing 8,192 `write()` calls and 8,192 `read()` calls per run.
4. **Broader concurrency validation** — the mutex implementation is separately tested with **4 producers + 4 consumers** because its design supports MPMC access; the lock-free implementation deliberately does not.
5. **Negative-path validation** — the lock-free driver is also tested for rejection of additional readers/writers, demonstrating that the SPSC contract is actively enforced rather than merely documented.

Under the recorded QEMU workload, the lock-free SPSC implementation reached **40.81 MiB/s** versus **35.73 MiB/s** for the mutex-protected SPSC implementation, an improvement of approximately **14.2%**, while both transferred all 8 MiB with zero corruption. This is a useful controlled comparison because the workload, queue semantics, transfer size, and validation criteria are held constant.

The result should not be interpreted as a universal statement that lock-free synchronization is always faster. QEMU virtualization, syscall cost, user/kernel copies, queue capacity, scheduling, CPU topology, and transfer size can materially change the outcome. In fact, the recorded lock-free run had more voluntary context switches than the mutex run, which reinforces that throughput cannot be reduced to a single synchronization metric.

### Validation summary

| Area | Result | What it demonstrates |
|---|---:|---|
| Simple character-device lifecycle | **PASS** | Dynamic major/minor registration, `/dev` creation, `open/read/write`, user/kernel transfer, and clean module unload. |
| Thread-safe boundary behavior | **13 / 13 PASS** | Empty/full behavior, `O_NONBLOCK`, exact-capacity fill/drain, partial reads, and ring wrap-around preserve the FIFO contract. |
| Lock-free SPSC boundary behavior | **15 / 15 PASS** | FIFO boundaries pass and additional readers/writers are rejected according to the SPSC ownership contract. |
| Thread-safe SPSC integrity | **PASS** | 8 MiB written and read with **0 mismatches**, **0 short writes**, and **0 short reads**. |
| Lock-free SPSC integrity | **PASS** | 8 MiB written and read with **0 mismatches**, **0 short writes**, and **0 short reads**. |
| Thread-safe MPMC reliability | **PASS** | 4 producers + 4 consumers transferred the expected **40,000 / 40,000 bytes** without loss. |
| SPSC ownership/race experiment | **Observed and resolved** | A deliberately retained reader prevented a second reader from entering; after release, the complete lock-free suite returned to 15/15 PASS. |

### Recorded SPSC benchmark

Both synchronized implementations were measured using **8 MiB total transfer and 1024-byte chunks**.

| Metric | Thread-safe mutex FIFO | Lock-free SPSC FIFO |
|---|---:|---:|
| Bytes written | 8,388,608 | 8,388,608 |
| Bytes read | 8,388,608 | 8,388,608 |
| Data mismatches | **0** | **0** |
| `write()` calls | 8,192 | 8,192 |
| `read()` calls | 8,192 | 8,192 |
| Short writes | 0 | 0 |
| Short reads | 0 | 0 |
| Elapsed time | 0.224 s | 0.196 s |
| Throughput | **35.73 MiB/s** | **40.81 MiB/s** |
| Voluntary context switches | 140 | 185 |
| Involuntary context switches | 0 | 1 |
| Result | **PASS** | **PASS** |

**Recorded throughput delta:** approximately **+14.2%** for the lock-free SPSC implementation in this QEMU run.

### Thread-safe MPMC result

The mutex/wait-queue implementation was additionally exercised under broader concurrency:

```text
Producers:       4
Consumers:       4
Bytes/producer:  10,000
Expected bytes:  40,000
Consumed bytes:  40,000
Elapsed:         0.221 s
Aggregate rate:  181,296 bytes/s
Result:          PASS
```

This validates the central design trade-off: the mutex implementation accepts broader **MPMC concurrency**, while the lock-free implementation narrows the concurrency contract to **SPSC** in exchange for removing the queue mutex.

### SPSC ownership experiment

A negative test intentionally left one lock-free reader blocked on an empty FIFO:

```sh
/bin/char_device_demo lockfree read 5 &
```

While that reader owned the single-reader slot, later tests could not acquire another reader and correctly failed before transferring data. Supplying the expected data released the blocked reader:

```sh
/bin/char_device_demo lockfree write HELLO
```

After the reader exited, the lock-free boundary suite completed with:

```text
Result: 15 passed, 0 failed
```

This demonstrates that `-EBUSY` admission control is part of the lock-free driver's correctness model rather than an incidental restriction.

### Runtime driver lifecycle validated

The captured interactive execution validates the complete character-device path:

```text
insmod
  → module registration
  → dynamic major/minor allocation
  → /dev device creation
  → open/read/write through file_operations
  → copy_to_user()/copy_from_user()
  → rmmod
```

Representative output:

```text
simple_char: loaded major=241 minor=0
device=/dev/simple_char written=5 data="HELLO"
device=/dev/simple_char read=5 data="HELLO"
simple_char: unloaded

thread_safe_char: loaded capacity=4096
device=/dev/thread_safe_char written=5 data="HELLO"

lock_free_char: loaded usable_capacity=4096 SPSC
device=/dev/lock_free_char written=5 data="HELLO"
device=/dev/lock_free_char read=5 data="HELLO"
lock_free_char: unloaded
```

### Result artifacts

Complete logs, commands, screenshots, and the negative-test experiment are retained under [`aws_linux_results/`](aws_linux_results/):

- [`driver_interaction.log`](aws_linux_results/driver_interaction.log) — module load/unload and basic `/dev` interaction;
- [`safe.log`](aws_linux_results/safe.log) — thread-safe boundary, SPSC, and MPMC results;
- [`lockfree.log`](aws_linux_results/lockfree.log) — lock-free boundary and SPSC benchmark results;
- [`race_condition_injection.log`](aws_linux_results/race_condition_injection.log) — deliberate SPSC ownership conflict and recovery;
- [`commands.txt`](aws_linux_results/commands.txt) — interactive QEMU commands;
- [`setup_steps.md`](aws_linux_results/setup_steps.md) — AWS/Linux setup procedure;
- [`screenshot/`](aws_linux_results/screenshot/) — captured execution evidence.

### Demonstrated engineering outcomes

- Implemented and loaded three Linux character-device modules with distinct synchronization contracts.
- Verified standard `open()`, `read()`, and `write()` paths across the user/kernel boundary.
- Preserved FIFO ordering across full-buffer, partial-read, and wrap-around conditions.
- Validated `O_NONBLOCK` behavior with `-EAGAIN` on empty/full queues.
- Demonstrated correct MPMC behavior with mutex + wait-queue synchronization.
- Demonstrated lock-free SPSC ownership using acquire/release publication and explicit reader/writer admission control.
- Verified **zero corruption over 8 MiB SPSC transfers** for both synchronized implementations.
- Measured **40.81 MiB/s lock-free vs 35.73 MiB/s mutex throughput** under the same recorded QEMU workload.
- Captured reproducible Docker/QEMU execution artifacts for review and regression testing.

---

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

## Design trade-offs

| Implementation | Concurrency | Main advantage | Main cost / restriction |
|---|---|---|---|
| `simple_char` | Unsupported | Minimal character-driver plumbing | Shared state races under concurrent access |
| `thread_safe_char` | MPMC | Straightforward correctness and blocking semantics | Mutex serialization and scheduler/wakeup overhead |
| `lock_free_char` | SPSC | Producer and consumer do not serialize on one queue mutex | Exactly one reader and one writer; explicit memory ordering is harder to reason about |

The unsynchronized implementation has the smallest code path but cannot guarantee correct concurrent access. The mutex design is direct and supports MPMC use, but lock contention and scheduler wakeups can increase latency under load. The SPSC lock-free design allows one producer and one consumer to progress without a queue lock, but requires strict ownership, explicit memory ordering, and rejects broader concurrency.

Lock-free should not be assumed to be faster in every workload. Small transfers emphasize synchronization and syscall overhead; large transfers increasingly emphasize `copy_to_user()`/`copy_from_user()` and memory movement. Performance comparisons must use identical workloads and must retain correctness as the primary pass criterion.

# Character-device architecture

A character driver exposes kernel functionality through the standard Unix file API:

```text
user application
      |
      | open/read/write/ioctl/poll
      v
Linux VFS
      |
      v
struct file_operations
      |
      +--> .open
      +--> .read
      +--> .write
      +--> .release
      |
      v
driver state / hardware / kernel service
```

The simple driver makes the registration path explicit:

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

The circular FIFO implementations then replace the fixed buffer with a bounded producer/consumer queue.

## Thread-safe FIFO synchronization

The thread-safe design protects queue mutation with a mutex and uses wait queues to avoid busy-waiting.

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

`O_NONBLOCK` changes empty/full waits into `-EAGAIN`, allowing applications to choose blocking or non-blocking semantics.

## Lock-free SPSC synchronization

The lock-free design replaces shared mutex ownership with strict index ownership:

```text
single reader                    single writer
     |                                |
owns read_index                 owns write_index
     |                                |
     `------- acquire/release --------'
```

`volatile` is not used as a synchronization mechanism. Buffer updates are completed before publishing a new index with `smp_store_release()`, while the peer observes that publication with `smp_load_acquire()` before consuming the corresponding data.

The contract is deliberately limited to one active reader and one active writer. Additional readers/writers are rejected with `-EBUSY` rather than being allowed to violate the SPSC algorithm.

# Docker + QEMU workflow

The Docker image contains Ubuntu kernel headers, the matching kernel image, GCC/Kbuild, BusyBox, and QEMU. The three modules are built against that kernel, packaged with static test binaries into an initramfs, and loaded inside a QEMU Linux guest.

Docker provides the reproducible build environment. QEMU provides the actual Linux kernel under test. A normal container alone is insufficient for portable kernel-module testing because containers share the host kernel.

```text
Host: Windows / Linux / EC2
         |
         v
       Docker
         |-- GCC / Kbuild
         |-- matching kernel + headers
         |-- BusyBox / cpio
         `-- QEMU
               |
               v
         Linux guest kernel
               |
               +-- simple_char.ko
               +-- thread_safe_char.ko
               +-- lock_free_char.ko
               |
               v
             /dev/*
```

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

# Benchmarking and reliability tests

## SPSC comparison

Identical workloads should be used when comparing the mutex and lock-free implementations:

```sh
/bin/stress_tests stream safe 64 64
/bin/stress_tests stream lockfree 64 64

/bin/stress_tests stream safe 64 1024
/bin/stress_tests stream lockfree 64 1024

/bin/stress_tests stream safe 64 4096
/bin/stress_tests stream lockfree 64 4096
```

Metrics include:

- elapsed time;
- throughput in MiB/s;
- `read()` / `write()` call counts;
- short I/O counts;
- voluntary and involuntary context switches;
- exact byte counts;
- data mismatch/corruption count.

Pass criteria are correctness-based: exact byte counts, zero corruption, and zero unexpected errors. No test assumes that the lock-free implementation must outperform the mutex implementation.

QEMU/TCG timing should be treated as a controlled comparative experiment, not native-hardware throughput.

## MPMC reliability

The mutex implementation supports multiple concurrent producers and consumers:

```sh
/bin/stress_tests mpmc 1 1 100000
/bin/stress_tests mpmc 2 2 100000
/bin/stress_tests mpmc 4 4 100000
```

The lock-free implementation is intentionally excluded from MPMC benchmarking because its correctness contract is SPSC.

## Unsynchronized race observation

The simple driver deliberately contains no synchronization and therefore acts as a baseline for observing unsafe concurrent access:

```sh
/bin/stress_tests simple-race 4 10
```

This test is observational rather than a thread-safety validation.

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
├── aws_linux_results/
├── driver-lab.sh
├── driver-lab.cmd
├── Makefile
└── README.md
```

# Important implementation note

All driver callbacks use module-specific names such as `simple_char_open()`. This avoids collisions with existing kernel symbols such as Linux's own `simple_open()` declared in `<linux/fs.h>`.

# Reference

The Docker/QEMU workflow follows the same development pattern as:

- https://github.com/czhao-dev/linux-device-drivers/tree/main/linux-character-device-driver

The reference project builds an out-of-tree module in Docker, packages the module and static tests into a BusyBox initramfs, and boots a matching Linux kernel under QEMU for real `insmod`/`rmmod` testing.

Official external-module Kbuild documentation:

- https://docs.kernel.org/next/kbuild/modules.html
