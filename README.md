# Linux Character Device Driver Lab

A Linux kernel learning project that implements the same character-device interface using three synchronization designs: an unsynchronized baseline, a mutex/wait-queue circular FIFO, and a lock-free SPSC circular FIFO. The drivers are built and exercised in a reproducible Docker + QEMU Linux environment, with implementation under [`aws_linux_results/`](aws_linux_results/).

## Results at a glance

Correctness is treated as the primary benchmark. Throughput is compared only after both synchronized drivers complete the same workload with exact byte counts and zero corruption.

| Validation | Thread-safe FIFO | Lock-free SPSC FIFO |
|---|---:|---:|
| Boundary tests | **13 / 13 PASS** | **15 / 15 PASS** |
| SPSC transfer | 8 MiB | 8 MiB |
| Chunk size | 1024 B | 1024 B |
| Bytes written/read | 8,388,608 / 8,388,608 | 8,388,608 / 8,388,608 |
| Data mismatches | **0** | **0** |
| Short reads/writes | **0 / 0** | **0 / 0** |
| Elapsed time | 0.224 s | 0.196 s |
| Throughput | **35.73 MiB/s** | **40.81 MiB/s** |
| Result | **PASS** | **PASS** |

The recorded lock-free run achieved approximately **14.2% higher SPSC throughput** than the mutex implementation under the same QEMU workload. This is a controlled comparison, not a universal performance claim: QEMU scheduling, transfer size, syscall cost, queue capacity, and user/kernel copying can change the result.

The mutex implementation was also validated under **4 producers + 4 consumers**, consuming the expected **40,000 / 40,000 bytes** without loss. The lock-free implementation instead enforces its narrower SPSC contract by rejecting additional readers or writers with `-EBUSY`.

### Execution

**Docker/QEMU build environment**

<img src="aws_linux_results/screenshot/Screenshot%202026-09-06%20220130.png" alt="Docker build for the Linux character driver lab" width="100%">

**Simple driver lifecycle: load → `/dev` I/O → unload**

<img src="aws_linux_results/screenshot/Screenshot%202026-09-06%20225115.png" alt="Simple character driver module load, roundtrip and unload" width="760">

**Lock-free boundary and SPSC benchmark execution**

<img src="aws_linux_results/screenshot/Screenshot%202026-09-06%20231229.png" alt="Lock-free character driver benchmark and boundary test execution" width="100%">

Detailed logs:

- [`safe.log`](aws_linux_results/safe.log) — mutex boundary, SPSC, and MPMC results
- [`lockfree.log`](aws_linux_results/lockfree.log) — lock-free boundary and SPSC results
- [`driver_interaction.log`](aws_linux_results/driver_interaction.log) — module lifecycle and `/dev` interaction
- [`race_condition_injection.log`](aws_linux_results/race_condition_injection.log) — SPSC ownership-conflict experiment
- [`commands.txt`](aws_linux_results/commands.txt) — interactive commands
- [`setup_steps.md`](aws_linux_results/setup_steps.md) — AWS/Linux setup sequence

## Driver implementations

| Driver | Device node | Design | Concurrency contract |
|---|---|---|---|
| `simple_char` | `/dev/simple_char` | Fixed byte buffer, explicit major/minor + `cdev` | No synchronization; educational baseline |
| `thread_safe_char` | `/dev/thread_safe_char` | Circular byte FIFO, mutex + wait queues | Multiple producers / multiple consumers |
| `lock_free_char` | `/dev/lock_free_char` | Circular byte FIFO, acquire/release index publication | Single producer / single consumer |

The circular drivers implement **byte-stream FIFO semantics**. Separate `write()` calls are not preserved as message boundaries.

## Design and trade-offs

### Simple character driver

The baseline isolates Linux character-driver mechanics: `alloc_chrdev_region()`, `cdev_init()`, `cdev_add()`, `class_create()`, `device_create()`, `file_operations`, and `copy_to_user()` / `copy_from_user()`. Shared access is intentionally unsynchronized so the kernel/device plumbing remains easy to inspect.

### Thread-safe circular FIFO

A mutex protects the shared read index, write index, occupancy, and buffer mutation. Wait queues sleep readers when the FIFO is empty and writers when it is full instead of busy-waiting. `O_NONBLOCK` converts those cases into `-EAGAIN`.

**Trade-off:** the design is straightforward and supports MPMC access, but mutex contention and scheduler wakeups can add overhead.

### Lock-free SPSC circular FIFO

The queue mutex is removed by assigning exclusive ownership of `read_index` to the reader and `write_index` to the writer. `smp_store_release()` publishes completed buffer updates; `smp_load_acquire()` ensures the peer observes the associated data before using the published index.

Additional readers/writers are rejected with `-EBUSY` because allowing them would violate the SPSC algorithm.

**Trade-off:** producer and consumer do not serialize on one queue mutex, but the concurrency contract is narrower and memory ordering is harder to reason about correctly.

## Character-device path

```text
User application
      |
      | open / read / write
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
Driver state / circular buffer
```

The simple driver exposes the registration path explicitly:

```text
alloc_chrdev_region()
        |
        v
major/minor
        |
        v
cdev_init() + cdev_add()
        |
        v
class_create() + device_create()
        |
        v
/dev/simple_char
```

## Synchronization behavior

### Mutex + wait queues

```text
reader                              writer
  |                                   |
read()                              write()
  |                                   |
queue empty                           |
  |                                   |
wait_event_interruptible()            |
  |                              mutex_lock()
  |                              enqueue data
  |                              mutex_unlock()
  |                                   |
  |<------ wake_up_interruptible() ---|
  |
consume data
```

### Lock-free SPSC ownership

```text
single reader                      single writer
     |                                  |
owns read_index                   owns write_index
     |                                  |
     '------ acquire / release ---------'
```

`volatile` is not used as a synchronization primitive. Correctness depends on ownership plus acquire/release ordering.

## Docker + QEMU test environment

Docker provides the reproducible Linux build toolchain and matching kernel headers. QEMU boots the Linux kernel in which the modules are actually loaded; a normal container alone would share the host kernel.

```text
Host: Windows / Linux / EC2
          |
          v
        Docker
          |
          +-- GCC / Kbuild
          +-- Linux kernel + headers
          +-- BusyBox / initramfs
          '-- QEMU
                |
                v
          Linux guest kernel
                |
                +-- simple_char.ko
                +-- thread_safe_char.ko
                '-- lock_free_char.ko
```

### Build and run automated tests

```bash
docker build -t char-driver-lab -f docker/Dockerfile .
docker run --rm char-driver-lab test
```

A successful automated run ends with:

```text
DRIVER LAB RESULT: PASS
Docker/QEMU test result: PASS
```

### Interactive driver practice

```bash
docker run --rm -it char-driver-lab shell
```

Inside the QEMU guest:

```sh
# Simple driver
insmod /modules/simple_char.ko
ls -l /dev/simple_char
cat /proc/devices
/bin/char_device_demo simple roundtrip HELLO
dmesg | tail -30
rmmod simple_char

# Thread-safe FIFO
insmod /modules/thread_safe_char.ko
/bin/boundary_tests safe
/bin/stress_tests stream safe 8 1024
/bin/stress_tests mpmc 4 4 10000
rmmod thread_safe_char

# Lock-free SPSC FIFO
insmod /modules/lock_free_char.ko
/bin/boundary_tests lockfree
/bin/stress_tests stream lockfree 8 1024
rmmod lock_free_char
```

Convenience launchers are also provided:

```bash
# Linux / EC2
chmod +x driver-lab.sh
./driver-lab.sh test
./driver-lab.sh shell
```

```powershell
# Windows + Docker Desktop
.\driver-lab.cmd test
.\driver-lab.cmd shell
```

## Test coverage

| Test area | Purpose |
|---|---|
| Empty/full queue | Validate blocking and non-blocking behavior |
| Exact-capacity transfer | Verify full FIFO utilization |
| Ring wrap-around | Verify FIFO order across buffer end |
| Zero-length I/O | Validate standard syscall edge behavior |
| `O_NONBLOCK` | Verify `-EAGAIN` on empty/full conditions |
| SPSC integrity | Verify exact byte counts and zero corruption |
| MPMC reliability | Verify mutex design with concurrent producers/consumers |
| SPSC admission | Verify extra readers/writers return `-EBUSY` |
| Unsynchronized race observation | Demonstrate why the baseline requires synchronization |

Benchmark pass criteria are correctness-based: exact byte counts, zero mismatches, zero unexpected errors, and valid concurrency-contract behavior. Throughput is reported as a measurement, not as a correctness requirement.

## Repository structure

```text
.
├── 01_simple_char/
├── 02_thread_safe_char/
├── 03_lock_free_char/
├── user/
├── tests/
├── docker/
├── aws_linux_results/
├── driver-lab.sh
├── driver-lab.cmd
├── Makefile
└── README.md
```

## Implementation note

Driver callbacks use module-specific names such as `simple_char_open()` to avoid collisions with existing kernel symbols such as Linux's `simple_open()`.

## Reference

The Docker/QEMU workflow follows the development pattern demonstrated by:

- https://github.com/czhao-dev/linux-device-drivers/tree/main/linux-character-device-driver
- https://docs.kernel.org/next/kbuild/modules.html
