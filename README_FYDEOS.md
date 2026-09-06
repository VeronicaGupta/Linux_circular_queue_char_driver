# FydeOS / Crostini Build Guide

## Environment model

FydeOS exposes a Debian Linux development environment through Crostini. The Debian container supplies user-space tools such as GCC, `make`, Git, Python, and development libraries. The kernel reported by `uname -r` belongs to the surrounding FydeOS/ChromiumOS virtualization environment rather than to the Debian package repository.

An observed system may therefore contain:

```text
Debian GNU/Linux 12 user space
        |
        v
FydeOS/Crostini kernel
6.6.76-08111-g8df27f55632a
```

A Debian package named `linux-headers-6.6.76-08111-g8df27f55632a` is not expected to exist when that kernel was not distributed by Debian.

---

## Install build tools

Only user-space development packages are required for the FydeOS-safe build:

```bash
sudo apt update
sudo apt install -y build-essential git
```

Verification:

```bash
make --version
gcc --version
```

---

## Build on FydeOS

From the repository root:

```bash
make
```

The default target is intentionally environment-aware.

When `/lib/modules/$(uname -r)/build` is missing, the following components are still built:

```text
user/char_device_demo
tests/boundary_tests
tests/stress_tests
```

Kernel modules are skipped with a diagnostic instead of terminating the complete build.

An explicit FydeOS target is also available:

```bash
make fydeos
```

The target builds user-space components and prints the detected kernel/build environment.

Environment diagnostics can be requested separately:

```bash
make env
```

---

## Expected output on the observed FydeOS system

For a kernel similar to:

```text
6.6.76-08111-g8df27f55632a
```

and no kernel build tree at:

```text
/lib/modules/6.6.76-08111-g8df27f55632a/build
```

`make` completes the user-space build and reports:

```text
[SKIP] Kernel module build
Running kernel: 6.6.76-08111-g8df27f55632a
Missing build tree: /lib/modules/6.6.76-08111-g8df27f55632a/build
```

This is a supported project state rather than a Makefile failure.

---

## Build only the user application

```bash
make -C user
```

Usage:

```bash
./user/char_device_demo
```

The executable can be built without a kernel module. Device operations fail with `ENOENT` until the corresponding `/dev` node is backed by a loaded driver.

---

## Build only the test executables

```bash
make -C tests
```

Generated binaries:

```text
tests/boundary_tests
tests/stress_tests
```

Compilation does not require the custom kernel drivers. Runtime driver tests do require the devices.

---

## Why `linux-headers-$(uname -r)` fails

The common Debian/Ubuntu workflow is:

```bash
sudo apt install linux-headers-$(uname -r)
```

That workflow assumes that the running kernel was distributed by the same package repository. The FydeOS/Crostini kernel does not satisfy that assumption. Installing generic Debian headers does not make them compatible with the currently running FydeOS kernel.

A kernel module must be built against a compatible kernel build tree containing the configuration and generated headers for the intended target kernel.

---

## Building against an externally prepared kernel tree

The project accepts an explicit `KDIR`:

```bash
make modules KDIR=/absolute/path/to/prepared/kernel-tree
```

A single module can also be built:

```bash
make -C 01_simple_char KDIR=/absolute/path/to/prepared/kernel-tree
make -C 02_thread_safe_char KDIR=/absolute/path/to/prepared/kernel-tree
make -C 03_lock_free_char KDIR=/absolute/path/to/prepared/kernel-tree
```

The supplied tree must be prepared for external-module builds and must correspond to the intended target kernel. Merely downloading a different Linux source tree or installing `linux-headers-amd64` is insufficient.

Compilation against an external tree only creates `.ko` files. It does not establish that the Crostini kernel permits loading them.

---

## Runtime testing

Full runtime testing requires:

```text
matching kernel build tree
        +
permission to load kernel modules
        +
/dev device creation
```

A standard native Debian/Ubuntu installation or a conventional Linux VM with its own kernel provides the simplest environment for that stage.

Typical native-Linux flow:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)

make native
sudo insmod 01_simple_char/simple_char.ko
ls -l /dev/simple_char
```

The complete automated test suite loads and unloads all three modules:

```bash
make test
```

The test runner requires `sudo`, `insmod`, `rmmod`, and loadable `.ko` files.

---

## FydeOS development workflow

```text
FydeOS / penguin
    |
    +-- edit source
    +-- git operations
    +-- make
    +-- compile user/main.c
    +-- compile boundary/stress tests
    +-- inspect driver source
    |
    `-- kernel module runtime
            |
            `-- execute in a compatible native Linux/VM kernel environment
```

This separation keeps normal development available on FydeOS while preventing accidental compilation against mismatched kernel headers.

---

## Useful commands

```bash
# FydeOS-safe complete build
make

# Explicit FydeOS build
make fydeos

# Environment diagnosis
make env

# Show available targets
make help

# User-space only
make -C user

# Tests only
make -C tests

# Kernel modules with a supplied kernel tree
make modules KDIR=/path/to/kernel-tree

# Remove all local artifacts even without kernel headers
make clean
```
