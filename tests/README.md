# Character Device Test Suite

## Purpose

The test suite validates boundary behavior, concurrency correctness, reliability, and measurable Linux runtime characteristics for the three character-device implementations.

The same test framework exposes the architectural distinction between the drivers:

| Driver | Intended concurrency | Reliability criterion | Performance comparison |
|---|---|---|---|
| `simple_char` | Controlled single-access demonstration | Boundary behavior passes; concurrent shared writes have no correctness guarantee | No FIFO throughput comparison |
| `thread_safe_char` | Multiple producers and multiple consumers | Exact byte accounting under MPMC contention | SPSC stream throughput recorded as the mutex baseline |
| `lock_free_char` | Single producer and single consumer | Exact SPSC stream plus rejection of additional readers/writers | SPSC stream throughput compared with mutex baseline |

The basic driver is not treated as a queue. A concurrent-write observation is included specifically to expose the absence of synchronization rather than to impose queue semantics on that implementation.

---

## Test files

```text
tests/
├── Makefile
├── README.md
├── boundary_tests.c
├── stress_tests.c
├── run_tests.sh
└── results/                 # created when run_tests.sh executes
```

`boundary_tests.c` checks deterministic API and capacity behavior.

`stress_tests.c` measures sustained transfer correctness, throughput, syscall activity, scheduler context switches, retry behavior, and concurrent-access reliability.

`run_tests.sh` loads one kernel module at a time, runs the appropriate tests, unloads the module, and writes a timestamped report under `tests/results/`.

---

# Boundary tests

## Basic character device

The following conditions are required:

- device opens successfully;
- zero-length write returns `0`;
- exactly `1024` bytes can be written;
- another byte at the end of the same file position returns `ENOSPC`;
- a new reader retrieves exactly the stored bytes;
- reading past stored data returns `0` as EOF;
- an oversized write returns a partial count equal to the fixed capacity.

### Pass criterion

Every deterministic boundary assertion must pass.

The basic driver contains shared state without a lock, so concurrent access is intentionally excluded from the correctness contract.

---

## Thread-safe circular driver

The following conditions are required:

- zero-length read and write return `0`;
- empty non-blocking read returns `EAGAIN`;
- exactly `4096` bytes fill the queue;
- another non-blocking write to a full queue returns `EAGAIN`;
- all queued bytes are returned unchanged and in FIFO order;
- an oversized write returns only the currently available capacity;
- a read/write sequence crossing the physical ring boundary preserves logical ordering.

### Pass criterion

Every boundary assertion must pass with zero byte mismatches.

---

## Lock-free circular driver

The FIFO boundary requirements match the thread-safe driver. Additional SPSC ownership requirements are checked:

- one active reader opens successfully;
- a second concurrent reader receives `EBUSY`;
- one active writer opens successfully;
- a second concurrent writer receives `EBUSY`.

### Pass criterion

Every FIFO boundary assertion must pass and both extra-endpoint admission checks must return `EBUSY`.

The rejection behavior is part of correctness. SPSC ownership replaces the general-purpose mutex used by the thread-safe implementation.

---

# Reliability stress tests

## SPSC stream integrity

The same test is applied to `thread_safe_char` and `lock_free_char`.

A producer generates a deterministic byte stream while a consumer validates every received byte. The default workload transfers `64 MiB` in `1024-byte` application chunks.

Recorded metrics:

- requested bytes;
- bytes accepted by `write()`;
- bytes returned by `read()`;
- data mismatches;
- reliability percentage;
- `write()` syscall count;
- `read()` syscall count;
- short writes;
- short reads;
- elapsed wall time;
- throughput in MiB/s;
- voluntary context switches;
- involuntary context switches;
- unexpected error code.

### Pass criterion

```text
bytes_written == requested_bytes
bytes_read    == requested_bytes
data_mismatches == 0
unexpected_error == 0
```

Relative throughput is deliberately not a correctness criterion. Scheduler policy, CPU frequency, virtualization, mitigations, kernel configuration, and memory behavior can change the measured winner. The measured numbers provide evidence rather than a predetermined benchmark result.

---

## MPMC reliability

The MPMC test applies to `thread_safe_char` only.

Four producers write one-byte producer identifiers concurrently. Four consumers drain the queue concurrently. One-byte writes avoid imposing message-boundary semantics on the byte FIFO while still creating heavy lock contention.

Default workload:

```text
4 producers
4 consumers
50,000 bytes per producer
200,000 expected bytes total
```

A histogram tracks the byte count belonging to every producer.

Recorded metrics:

- expected bytes;
- consumed bytes;
- producer `EAGAIN` retries;
- consumer `EAGAIN` retries;
- unexpected errors;
- token histogram accuracy;
- reliability percentage;
- elapsed time;
- aggregate bytes/second.

### Pass criterion

```text
consumed_bytes == expected_bytes
count(token_N) == bytes_generated_by_producer_N
unexpected_errors == 0
```

This test distinguishes the mutex implementation from the SPSC implementation. The mutex driver supports simultaneous producers and consumers; the lock-free driver deliberately rejects additional endpoints instead of accepting an unsafe MPMC configuration.

---

# Basic-driver concurrent-write observation

Several writers independently open `/dev/simple_char` and write one-byte markers while yielding between writes. All file descriptors modify the same kernel buffer without synchronization.

Recorded metrics:

- number of stress rounds;
- number of final buffers containing markers from multiple writers;
- unexpected syscall failures.

A mixed final buffer demonstrates execution interleaving and the absence of a serialization guarantee.

### Interpretation criterion

```text
mixed_rounds > 0
    synchronization race behavior observed

mixed_rounds == 0
    race not observed during the selected run
```

No concurrency PASS requirement exists for `simple_char`; concurrent correctness is outside the driver's design contract. Increasing the number of rounds increases the probability of observing interleaving.

---

# Performance comparison

A direct throughput comparison is meaningful only between the two circular FIFO implementations under the same SPSC workload.

```text
thread_safe_char
    mutex acquisition/release
    shared data_count updates
    wait-queue wakeups
    user/kernel copies

lock_free_char
    acquire/release index publication
    exclusive read/write index ownership
    wait-queue wakeups
    user/kernel copies
```

Expected qualitative difference:

- the mutex driver provides broader MPMC semantics at the cost of serialization;
- the lock-free driver removes queue-lock contention but restricts concurrency to SPSC;
- the basic driver has less synchronization overhead but provides no FIFO or concurrent-correctness guarantee.

No fixed requirement such as "lock-free must be 20% faster" is valid across arbitrary Linux systems. A lock-free design can lose on a particular machine because allocation, copying, scheduling, cache movement, and wakeups can dominate lock cost.

---

# Optional `perf` measurements

Linux `perf` can provide additional scheduler and CPU counters after the corresponding module is loaded.

Thread-safe FIFO:

```bash
perf stat \
  -e task-clock,context-switches,cpu-migrations,page-faults,cycles,instructions,cache-misses \
  ./tests/stress_tests stream safe 64 1024
```

Lock-free FIFO:

```bash
perf stat \
  -e task-clock,context-switches,cpu-migrations,page-faults,cycles,instructions,cache-misses \
  ./tests/stress_tests stream lockfree 64 1024
```

Useful comparison fields include:

| Metric | Meaning |
|---|---|
| MiB/s | delivered data rate |
| context switches | scheduler activity caused by blocking/wakeup behavior |
| task-clock | CPU time consumed by the test |
| cycles | processor work required |
| instructions | executed instruction volume |
| cache misses | memory-hierarchy pressure |
| short I/O count | queue backpressure visible at syscall boundaries |
| mismatches | data-integrity failures |

---

# Build

Kernel modules must be built before the complete test run:

```bash
make
```

The test executables can also be built independently:

```bash
make -C tests
```

---

# Complete Linux run

```bash
./tests/run_tests.sh
```

The script requests `sudo` access for module loading and unloading. A timestamped report is written under:

```text
tests/results/test_report_YYYYMMDD_HHMMSS.txt
```

A successful run ends with:

```text
OVERALL RESULT: PASS
```

The expected behavioral contrast remains visible in the detailed report:

```text
simple_char
    deterministic boundary tests: PASS
    concurrent safety: unsupported; race interleaving may be observed

thread_safe_char
    deterministic FIFO boundaries: PASS
    SPSC stream integrity: PASS
    MPMC reliability: PASS

lock_free_char
    deterministic FIFO boundaries: PASS
    SPSC endpoint ownership: PASS
    SPSC stream integrity: PASS
    additional readers/writers: rejected with EBUSY
```
