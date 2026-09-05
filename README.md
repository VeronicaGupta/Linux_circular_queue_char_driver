# Linux Character Device Driver Progression

## Purpose

The repository presents three character-device implementations with the same user-space access model and progressively stronger concurrency behavior.

- `01_simple_char` — basic character device with explicit major/minor allocation and a shared byte buffer.
- `02_thread_safe_char` — bounded circular byte FIFO protected by a mutex and wait queues.
- `03_lock_free_char` — bounded SPSC circular byte FIFO using ownership rules and acquire/release memory ordering.
- `user/main.c` — common user-space program for `open()`, `read()`, and `write()` demonstrations.
- `tests/` — deterministic boundary tests plus concurrency, reliability, and throughput stress tests.

The progression isolates character-device mechanics from synchronization mechanics. A common system-call interface allows direct behavioral comparison across all three kernel modules.

---

## Repository layout

```text
linux_char_devices_rewritten/
├── Makefile
├── README.md
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
└── tests/
    ├── boundary_tests.c
    ├── stress_tests.c
    ├── run_tests.sh
    ├── Makefile
    └── README.md
```

---

# Character device driver

A character device driver exposes kernel-controlled functionality through a file-like interface. Device nodes under `/dev` become entry points for standard file operations such as `open()`, `read()`, `write()`, and `close()`.

A user-space call such as:

```c
write(fd, buffer, length);
```

passes through the system-call and VFS layers before dispatch through a driver `struct file_operations` table.

```text
User process
    |
    | write()
    v
System call layer
    |
    v
Virtual File System
    |
    v
struct file_operations
    |
    v
Driver write callback
```

Character devices are appropriate for byte-oriented or command-oriented kernel interfaces where block-storage semantics are unnecessary. Typical examples include serial ports, sensors, control channels, virtual devices, debug interfaces, and custom IPC mechanisms.

---

# Why a character device is useful in a Linux system

Kernel code cannot be invoked as an ordinary user-space function. A controlled boundary is required between applications and privileged kernel resources.

A character device provides that boundary through established Linux abstractions:

- pathname-based access through `/dev`
- file descriptors
- permission checks
- VFS dispatch
- standard `read()` and `write()` semantics
- blocking and non-blocking behavior
- compatibility with tools such as `cat`, `echo`, `poll`, and application runtimes

The same user-space API can therefore remain stable while internal driver architecture changes.

---

# 01 — Basic character device

## Design

The basic implementation demonstrates explicit character-device registration.

```text
alloc_chrdev_region()
        |
        v
major + minor number
        |
        v
cdev_init()
        |
        v
cdev_add()
        |
        v
class_create()
        |
        v
device_create()
        |
        v
/dev/simple_char
```

A fixed 1024-byte kernel buffer stores data. File position controls read and write offsets. `copy_from_user()` transfers bytes into kernel memory. `copy_to_user()` transfers bytes back into user memory.

### Major number

The major number identifies the driver associated with a device number.

### Minor number

The minor number identifies a device instance managed by the same driver.

Only minor `0` is required for the basic implementation.

## Concurrency properties

No synchronization primitive protects the shared buffer or `simple_data_size`.

Concurrent access can therefore produce races such as:

```text
Writer A reads shared state
Writer B reads same shared state
Writer A changes buffer contents
Writer B changes overlapping contents
Final state depends on execution timing
```

The absence of synchronization is intentional. Character-device registration and user/kernel data transfer remain visible without concurrency code obscuring the basic path.

## Trade-offs

Advantages:

- minimal driver structure
- explicit major/minor registration
- direct visibility into `cdev` registration
- small code surface

Disadvantages:

- unsafe shared state under concurrent access
- no bounded producer-consumer semantics
- no blocking behavior
- no queue ordering between independent writes and reads

Suitable use:

- character-driver fundamentals
- VFS callback tracing
- major/minor registration study
- controlled single-process experiments

---

# Why a circular queue becomes useful

A single shared buffer has poor behavior when producers and consumers operate at different rates.

A producer can generate data before a consumer becomes ready. Direct replacement of the buffer can destroy unread data. A circular queue allows already allocated storage to be reused continuously while preserving FIFO ordering.

```text
capacity = 8

        read_index
            v
+---+---+---+---+---+---+---+---+
| D | E |   |   |   | A | B | C |
+---+---+---+---+---+---+---+---+
                ^
            write_index
```

Wrap-around avoids shifting data after every read. Queue operations remain constant-time aside from byte copying.

A bounded queue also introduces explicit backpressure. A full queue prevents unlimited kernel-memory growth, while an empty queue allows a consumer to sleep rather than spin.

---

# 02 — Thread-safe circular character device

## Queue state

The thread-safe driver maintains:

```c
buffer
capacity
read_index
write_index
data_count
```

`read_index` identifies the next byte available for consumption. `write_index` identifies the next free location for production. `data_count` distinguishes full and empty states directly.

## Synchronization problem

Several shared variables must change as one logical transaction.

A write operation requires:

```text
check free space
copy bytes into ring
advance write_index
increase data_count
```

A read operation requires:

```text
check available data
copy bytes from ring
advance read_index
decrease data_count
```

Interleaving without protection can break queue invariants.

Example:

```text
Initial data_count = 10

Reader A loads data_count = 10
Reader B loads data_count = 10
Reader A removes 8 bytes
Reader B removes 8 bytes using stale state
Queue metadata becomes inconsistent
```

## Mutex strategy

`struct mutex` protects the complete queue state.

```text
mutex_lock_interruptible()
        |
        v
inspect queue state
        |
        v
copy data
        |
        v
update indexes and count
        |
        v
mutex_unlock()
```

A mutex is appropriate because `read()` and `write()` callbacks execute in process context and sleeping operations can occur. A spinlock would waste CPU while contended and would impose stricter rules around sleeping code.

## Wait-queue strategy

Two wait queues provide blocking producer-consumer behavior.

```text
read_wait   -> reader sleeps while queue is empty
write_wait  -> writer sleeps while queue is full
```

Empty queue behavior:

```text
read()
  |
  v
queue empty
  |
  v
release mutex
  |
  v
wait_event_interruptible(read_wait, data_count > 0)
  |
  v
writer stores data
  |
  v
wake_up_interruptible(read_wait)
```

Full queue behavior follows the symmetric path through `write_wait`.

A `while` loop surrounds each blocking condition because a wakeup does not reserve queue state. Another runnable thread can consume newly available data before a previously sleeping thread reacquires the mutex.

## Non-blocking behavior

`O_NONBLOCK` changes queue-full and queue-empty behavior.

```text
empty read  -> -EAGAIN
full write  -> -EAGAIN
```

No sleeping occurs for those cases.

## User-memory transfer strategy

A temporary kernel buffer is used for each transfer.

Read path:

```text
ring -> temporary kernel buffer -> copy_to_user()
```

Write path:

```text
copy_from_user() -> temporary kernel buffer -> ring
```

Queue indexes are committed only after a successful user-memory transfer. Failed user copies therefore avoid partially committed queue metadata.

## Trade-offs

Advantages:

- multiple readers supported
- multiple writers supported
- simple correctness model
- blocking backpressure
- straightforward invariant validation

Disadvantages:

- mutex contention under heavy concurrency
- serialization of queue operations
- user-copy latency can extend mutex hold time
- temporary allocation adds allocation and copy overhead

Suitable use:

- general multi-threaded producer-consumer access
- correctness-oriented driver implementations
- moderate throughput requirements
- simple maintainable synchronization

---

# 03 — SPSC lock-free circular character device

## Concurrency contract

The lock-free queue supports exactly:

```text
one active reader
one active writer
```

Multiple readers or multiple writers are rejected with `-EBUSY` during `open()`.

The restriction converts shared queue state into single-writer ownership:

```text
reader owns read_index
writer owns write_index
```

No mutex or spinlock protects ring indexes during normal queue operations.

## Full and empty detection

An extra internal slot removes the need for a shared `data_count` variable.

For a usable capacity of 4096 bytes:

```text
allocated ring size = 4097 bytes
```

Empty state:

```text
read_index == write_index
```

Full state:

```text
used bytes == 4096
```

The extra slot eliminates ambiguity between full and empty conditions.

## Memory-ordering strategy

Lock removal does not remove ordering requirements.

Producer sequence:

```text
observe read_index with acquire ordering
        |
        v
copy new bytes into ring
        |
        v
publish write_index with release ordering
```

Consumer sequence:

```text
observe write_index with acquire ordering
        |
        v
copy available bytes from ring
        |
        v
publish read_index with release ordering
```

Relevant APIs:

```c
smp_load_acquire()
smp_store_release()
READ_ONCE()
```

Release publication prevents index visibility from preceding completion of associated buffer writes. Acquire observation prevents subsequent buffer reads from moving before observation of the published index.

`volatile` is not a replacement for this ordering model. Compiler-access visibility and inter-CPU memory ordering represent different concerns.

## Wait queues in a lock-free queue

Queue-state synchronization remains lock-free, while blocking system calls can still sleep.

```text
empty reader -> wait_event_interruptible()
full writer  -> wait_event_interruptible()
```

Wait queues can contain internal kernel synchronization. The description "lock-free" therefore applies to ring-state coordination between the single producer and single consumer, not to every instruction executed by the complete system-call path.

## Open admission

Atomic variables protect the SPSC access contract.

```text
reader_open: 0 -> 1
writer_open: 0 -> 1
```

`atomic_cmpxchg()` reserves each role. Queue data movement remains independent of those admission atomics.

## Trade-offs

Advantages:

- no queue mutex on the data path
- producer and consumer can progress concurrently
- reduced lock contention
- small synchronization state
- useful introduction to kernel memory ordering

Disadvantages:

- exactly one reader and one writer
- substantially stricter correctness assumptions
- harder review and debugging
- memory-ordering mistakes can create architecture-dependent failures
- no direct extension to MPMC operation without a different algorithm
- blocking wait queues mean the full driver is not wait-free

Suitable use:

- SPSC streaming paths
- deterministic producer-consumer ownership
- low-contention data transfer
- memory-ordering study

---

# Synchronization comparison

| Property | Basic | Thread-safe | Lock-free SPSC |
|---|---|---|---|
| Shared buffer | Yes | Circular FIFO | Circular FIFO |
| Multiple readers | Unsafe | Supported | Rejected |
| Multiple writers | Unsafe | Supported | Rejected |
| Mutex | No | Yes | No queue mutex |
| Wait queues | No | Yes | Yes |
| Blocking empty read | EOF-style result | Yes | Yes |
| Blocking full write | No | Yes | Yes |
| `O_NONBLOCK` queue handling | Not applicable | `-EAGAIN` | `-EAGAIN` |
| Memory ordering | No explicit concurrency contract | Provided by mutex | Acquire/release |
| Complexity | Low | Medium | Higher |
| Primary objective | Driver mechanics | General correctness | SPSC low-contention path |

---

# Design problems and API responses

| Problem | Consequence | Strategy | Kernel API / mechanism |
|---|---|---|---|
| Concurrent queue mutation | corrupted indexes or lost bytes | serialize complete state update | `mutex_lock_interruptible()` |
| Empty queue | CPU waste from polling | block consumer | `wait_event_interruptible()` |
| Full queue | unbounded growth or data overwrite | bounded backpressure | `wait_event_interruptible()` |
| State change after sleep | sleeping task remains blocked | wake relevant waiters | `wake_up_interruptible()` |
| Interrupted blocking call | unresponsive signal handling | abort syscall | `-ERESTARTSYS` |
| Non-blocking empty/full state | unwanted sleeping | immediate retry status | `-EAGAIN` |
| Invalid user pointer | kernel fault risk | guarded user access | `copy_to_user()`, `memdup_user()` |
| Ring wrap-around | out-of-range linear copy | split copy at ring boundary | two `memcpy()` segments |
| SPSC publication ordering | stale or premature data visibility | release/acquire synchronization | `smp_store_release()`, `smp_load_acquire()` |
| Multiple SPSC role owners | index write/write race | admission restriction | `atomic_cmpxchg()` |

---

# Why `volatile` does not solve synchronization

`volatile` constrains selected compiler optimizations around accesses. No mutual exclusion is created. No producer/consumer ownership is created. No complete cross-CPU publication protocol is created.

Thread-safe design requirements are satisfied with a mutex because several state variables form one protected invariant.

SPSC lock-free design requirements are satisfied with exclusive index ownership plus acquire/release ordering.

---

# Mutex versus lock-free design

A mutex-based implementation should generally be preferred when multiple producers or multiple consumers are required and measured contention remains acceptable. The correctness argument remains compact and maintainable.

A lock-free implementation becomes attractive when ownership can be constrained naturally and contention or latency measurements justify added complexity. Lock removal without a strict ownership model provides no correctness benefit.

Performance should be evaluated with measured workload behavior rather than inferred from the word "lock-free". Cache-line movement, system-call cost, copying, allocation, scheduling, and wakeup overhead can dominate lock cost.

---

# Byte stream versus message queue

Both circular implementations provide byte-stream FIFO semantics.

Example:

```text
write("ABC")
write("DEF")
read(6) -> "ABCDEF"
```

Write boundaries are not retained. A true message queue requires explicit message metadata such as a length field or fixed-size message slots.

A message-preserving design could use:

```c
struct message {
    size_t length;
    char data[MAX_MESSAGE_SIZE];
};
```

The circular structure would then hold message objects rather than raw bytes.

---

# Build requirements

A Linux development environment with matching kernel headers is required.

Typical Debian/Ubuntu packages:

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)
```

Complete repository build:

```bash
make
```

Individual module build examples:

```bash
make -C 01_simple_char
make -C 02_thread_safe_char
make -C 03_lock_free_char
```

User-space program build:

```bash
make -C user
```

---

# Module loading

Only the driver under evaluation needs to be loaded.

Basic driver:

```bash
sudo insmod 01_simple_char/simple_char.ko
ls -l /dev/simple_char
```

Thread-safe driver:

```bash
sudo insmod 02_thread_safe_char/thread_safe_char.ko
ls -l /dev/thread_safe_char
```

Lock-free driver:

```bash
sudo insmod 03_lock_free_char/lock_free_char.ko
ls -l /dev/lock_free_char
```

Kernel messages:

```bash
dmesg | tail -n 30
```

Unload commands:

```bash
sudo rmmod simple_char
sudo rmmod thread_safe_char
sudo rmmod lock_free_char
```

---

# Shared user-space program

The same `main.c` executable accesses all three devices.

Build:

```bash
make -C user
```

Basic driver examples:

```bash
./user/char_device_demo simple write "hello"
./user/char_device_demo simple read 32
./user/char_device_demo simple roundtrip "hello"
```

Thread-safe driver examples:

```bash
./user/char_device_demo safe write "hello"
./user/char_device_demo safe read 32
./user/char_device_demo safe roundtrip "hello"
```

Lock-free driver examples:

```bash
./user/char_device_demo lockfree write "hello"
./user/char_device_demo lockfree read 32
./user/char_device_demo lockfree roundtrip "hello"
```

An empty read on either circular FIFO blocks until data becomes available. A full circular FIFO blocks a writer until space becomes available.

---

# Suggested validation

Basic character device validation:

```text
load module
confirm /dev/simple_char
write known bytes
read known bytes
compare result
inspect dmesg
unload module
```

Thread-safe validation:

```text
start multiple producers
start multiple consumers
verify FIFO data integrity
exercise empty-reader blocking
exercise full-writer blocking
exercise signal interruption
exercise non-blocking mode
```

Lock-free validation:

```text
start one producer
start one consumer
verify sustained FIFO integrity
attempt second reader and expect EBUSY
attempt second writer and expect EBUSY
exercise ring wrap-around repeatedly
exercise empty/full blocking
```

Kernel debugging facilities such as KASAN, KCSAN, lockdep, and dynamic debug can provide additional validation during development.

---

# Engineering summary

The basic character device establishes the Linux driver interface. The mutex-based circular device adds a general bounded producer-consumer model. The SPSC lock-free circular device replaces shared-state serialization with ownership and memory-ordering constraints.

The three designs demonstrate an important systems principle: synchronization strategy follows the concurrency contract. General multi-reader/multi-writer requirements favor simple protected invariants. Strict single-producer/single-consumer ownership permits a smaller lock-free state machine. Increased concurrency performance therefore carries increased assumptions, testing requirements, and maintenance cost.

---

# Testing and measurable comparison

The `tests/` directory separates deterministic API validation from sustained concurrency tests. Boundary checks cover capacity edges, empty/full behavior, wrap-around, `EAGAIN`, `ENOSPC`, EOF, and the SPSC `EBUSY` ownership rule.

Stress tests record byte counts, corruption mismatches, syscall counts, short I/O, `EAGAIN` retries, elapsed time, throughput, and scheduler context switches. The thread-safe implementation receives an additional MPMC reliability test. The lock-free implementation receives SPSC stream validation and endpoint-admission validation. The basic implementation receives a concurrent-write observation that demonstrates the absence of a synchronization guarantee.

Build test executables with:

```bash
make tests
```

Run the complete module-by-module Linux test sequence with:

```bash
make test
```

Detailed test rationale and pass criteria are documented in [`tests/README.md`](tests/README.md).
