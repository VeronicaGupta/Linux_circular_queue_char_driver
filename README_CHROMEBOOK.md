# Build and Test on a Chromebook (ChromeOS Linux / Crostini)

## Scope

ChromeOS Linux development environments commonly run inside **Crostini**, which is a Debian container hosted by a ChromeOS-managed virtual machine. User-space C programs can be compiled and executed normally, but custom Linux kernel modules generally cannot be built and loaded against the host ChromeOS kernel in the same way as on a native Debian or Ubuntu installation.

The repository therefore has two usable paths on a Chromebook:

- **User-space build and inspection:** supported directly in Crostini.
- **Kernel-driver build, `insmod`, `/dev` testing, and stress testing:** requires a Linux environment with matching kernel headers and permission to load custom modules, such as a native Linux machine or suitable Linux VM.

---

## 1. Confirm the environment

Open the ChromeOS Linux terminal and run:

```bash
uname -a
cat /etc/os-release
```

A Crostini environment may show a ChromeOS-specific kernel version such as:

```text
6.6.x-xxxxx-g<commit>
```

The Debian container may still report Debian Bookworm or another Debian release. The container user space and the ChromeOS-managed kernel are separate concerns.

---

## 2. Install user-space development tools

Install the compiler, linker, standard development headers, and `make`:

```bash
sudo apt update
sudo apt install -y build-essential
```

Verify the installation:

```bash
make --version
gcc --version
```

Both commands should print version information.

If a previous command such as the following failed:

```bash
sudo apt install build-essential linux-headers-$(uname -r)
```

run the `build-essential` installation separately. A missing ChromeOS-specific kernel-header package can cause the combined command to fail before the required user-space tools are installed.

---

## 3. Clone or enter the repository

Example:

```bash
cd ~
git clone <repository-url> Linux_circular_queue_char_driver
cd Linux_circular_queue_char_driver
```

For an existing checkout:

```bash
cd ~/Linux_circular_queue_char_driver
```

Confirm the layout:

```bash
ls
```

Expected directories:

```text
01_simple_char
02_thread_safe_char
03_lock_free_char
tests
user
```

---

## 4. Build the shared user-space application

The user-space application does not require kernel headers.

```bash
make -C user
```

Expected result:

```text
user/char_device_demo
```

Run the program without arguments to inspect its usage:

```bash
./user/char_device_demo
```

The executable can be compiled successfully on Crostini even when the corresponding `/dev/...` devices do not exist.

---

## 5. Build the user-space test programs

The boundary and stress-test executables are also ordinary user-space programs.

```bash
make -C tests
```

Expected binaries include:

```text
tests/boundary_tests
tests/stress_tests
```

The test programs can be compiled on Crostini. Tests that open the custom character devices cannot complete until the kernel modules are loaded in a compatible Linux kernel environment.

---

## 6. Why the kernel modules usually cannot be built directly in Crostini

An out-of-tree Linux kernel module is normally compiled against the exact running kernel build tree:

```text
/lib/modules/$(uname -r)/build
```

Check whether that tree exists:

```bash
ls -ld /lib/modules/$(uname -r)/build
```

On a normal Debian or Ubuntu machine, matching headers are commonly installed with:

```bash
sudo apt install linux-headers-$(uname -r)
```

On a Chromebook, `uname -r` may identify a ChromeOS-specific kernel rather than a Debian-distributed kernel. Debian repositories therefore may return:

```text
E: Unable to locate package linux-headers-<ChromeOS-kernel-version>
```

Even if source or headers are obtained separately, Crostini is not intended to behave as a normal host environment for arbitrary custom kernel-module loading. The kernel belongs to the ChromeOS virtualization stack rather than to the Debian container.

---

## 7. What can be tested directly on the Chromebook

### Build validation

```bash
make -C user clean
make -C user

make -C tests clean
make -C tests
```

Pass criteria:

```text
PASS: all user-space sources compile with no errors
PASS: expected executables are produced
```

### Static source inspection

Useful commands:

```bash
grep -R "file_operations" 01_simple_char 02_thread_safe_char 03_lock_free_char
grep -R "mutex_" 02_thread_safe_char
grep -R "wait_event\|wake_up" 02_thread_safe_char 03_lock_free_char
grep -R "smp_load_acquire\|smp_store_release" 03_lock_free_char
```

These checks confirm that the intended synchronization strategies are represented in each implementation.

### User application error-path validation

Because the device is not loaded, the application should fail cleanly instead of crashing:

```bash
./user/char_device_demo simple read 32
```

An expected failure is similar to:

```text
open: No such file or directory
```

This confirms that user-space error handling works when the kernel device node is absent.

---

## 8. What requires another Linux environment

The following operations require a kernel that supports locally built and loadable modules:

```text
kernel module compilation
insmod / modprobe
rmmod
module log inspection with dmesg
creation of the custom character devices
real read/write interaction with the drivers
blocking and O_NONBLOCK validation
concurrent producer-consumer correctness testing
throughput comparison
perf measurements
reliability and stress testing
```

The complete functional test therefore needs a native Linux installation or a Linux VM whose running kernel has matching development headers.

---

## 9. Recommended test environment

A Debian or Ubuntu VM is sufficient. The VM must permit custom kernel-module loading.

Inside that Linux environment:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

Verify:

```bash
ls -ld /lib/modules/$(uname -r)/build
```

A valid directory should be displayed.

Clone or copy the repository into that environment, then run:

```bash
cd Linux_circular_queue_char_driver
make
```

The build should produce `.ko` files in the driver directories.

---

## 10. Build each kernel driver separately

### Simple character driver

```bash
make -C 01_simple_char
```

### Thread-safe circular driver

```bash
make -C 02_thread_safe_char
```

### Lock-free SPSC circular driver

```bash
make -C 03_lock_free_char
```

The top-level Makefile can also build all components when the required kernel headers are available:

```bash
make
```

---

## 11. Load and inspect a driver

Example for the simple driver:

```bash
sudo insmod 01_simple_char/simple_char.ko
lsmod | grep simple_char
sudo dmesg | tail -n 20
```

Inspect the device node:

```bash
ls -l /dev/simple_char
```

Unload after testing:

```bash
sudo rmmod simple_char
```

Equivalent steps apply to the thread-safe and lock-free modules using their module and device names.

---

## 12. Functional testing

After the selected driver is loaded, use the shared user application.

Examples:

```bash
./user/char_device_demo simple roundtrip "hello"
./user/char_device_demo safe roundtrip "hello"
./user/char_device_demo lockfree roundtrip "hello"
```

A successful test should show that written data can be retrieved according to the semantics of the selected driver.

---

## 13. Boundary tests

Build the test suite if not already built:

```bash
make -C tests
```

Run the boundary tests according to the commands documented in `tests/README.md`.

The suite covers conditions such as:

```text
zero-length operations
empty-buffer reads
full-buffer writes
exact-capacity transfers
oversized transfers
wrap-around behavior
O_NONBLOCK / EAGAIN behavior
lock-free SPSC ownership enforcement
```

Pass criteria are defined in `tests/README.md` and should remain deterministic across repeated runs.

---

## 14. Stress, reliability, and performance tests

Run the supplied test runner in the full Linux environment:

```bash
./tests/run_tests.sh
```

Reports are written under:

```text
tests/results/
```

Important reliability criteria include:

```text
requested bytes == written bytes
written bytes == consumed bytes
data mismatches == 0
unexpected errors == 0
reliability == 100%
```

The thread-safe driver should preserve correctness with multiple producers and consumers. The lock-free implementation should preserve correctness under its documented SPSC contract and reject unsupported simultaneous ownership where applicable.

Performance metrics include:

```text
elapsed time
throughput in MiB/s
read/write syscall counts
short reads/writes
EAGAIN retries
voluntary context switches
involuntary context switches
data mismatches
```

The performance comparison should not use a pass criterion requiring the lock-free implementation to always outperform the mutex implementation. Scheduler behavior, payload size, CPU topology, virtualization, and syscall overhead can dominate small benchmarks. Correctness is mandatory; throughput is an observed engineering metric.

---

## 15. Optional `perf` measurements

On a Linux environment with `perf` installed:

```bash
perf stat -e cycles,instructions,cache-misses,context-switches \
    ./tests/stress_tests <arguments>
```

Useful comparisons include:

```text
thread-safe SPSC vs lock-free SPSC
small payload vs large payload
blocking vs non-blocking operation
low contention vs high contention
```

Performance claims should be based on multiple runs and reported with the exact kernel, VM/native environment, CPU count, buffer size, transfer size, and workload configuration.

---

## 16. Recommended Chromebook workflow

```text
ChromeOS / Crostini
        |
        +-- edit C sources
        +-- use Git
        +-- build user/main.c
        +-- build user-space tests
        +-- perform static inspection
        |
        `-- push repository
                 |
                 v
       Native Linux / Linux VM
                 |
                 +-- install matching kernel headers
                 +-- build .ko modules
                 +-- insmod / rmmod
                 +-- test /dev devices
                 +-- run boundary tests
                 +-- run stress tests
                 `-- collect performance results
```

This workflow preserves the Chromebook as the development workstation while placing kernel-module execution in an environment designed to support out-of-tree kernel drivers.

---

## Quick command reference

### Chromebook / Crostini

```bash
sudo apt update
sudo apt install -y build-essential

cd ~/Linux_circular_queue_char_driver
make -C user
make -C tests

./user/char_device_demo
```

### Native Linux or compatible VM

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)

cd ~/Linux_circular_queue_char_driver
make

sudo insmod 01_simple_char/simple_char.ko
# run tests
sudo rmmod simple_char
```

See `tests/README.md` for the complete boundary, reliability, and performance test procedure.
