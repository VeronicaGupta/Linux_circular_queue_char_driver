Below is the complete AWS EC2 → Docker → QEMU workflow you used, arranged in execution order with comments explaining each command.

```bash
# ============================================================
# 1. CHECK AMAZON LINUX / KERNEL
# ============================================================

# Show the exact running Linux kernel version.
uname -r

# Show full kernel/system information.
uname -a


# ============================================================
# 2. UPDATE AMAZON LINUX
# ============================================================

# Refresh packages and install available updates.
sudo dnf update -y


# ============================================================
# 3. INSTALL DEVELOPMENT TOOLS
# ============================================================

# Initial generic package attempt.
# This did NOT work for the Amazon Linux 6.18 kernel because
# kernel-devel-$(uname -r) was not the package naming scheme.
sudo dnf install -y \
    git \
    gcc \
    gcc-c++ \
    make \
    kernel-devel-$(uname -r) \
    kernel-headers-$(uname -r) \
    perf


# ============================================================
# 4. INSTALL CORRECT AMAZON LINUX 6.18 DEVELOPMENT PACKAGES
# ============================================================

# Install Git, compiler, Make, matching 6.18 kernel development
# packages, and Linux performance tools.
sudo dnf install -y \
    git \
    gcc \
    gcc-c++ \
    make \
    kernel6.18-devel \
    kernel6.18-headers \
    perf6.18


# ============================================================
# 5. VERIFY KERNEL BUILD ENVIRONMENT
# ============================================================

# Confirm currently running kernel.
uname -r

# Check installed Amazon Linux 6.18 kernel packages.
rpm -qa | grep kernel6.18

# Verify that the kernel build directory exists.
ls -l /lib/modules/$(uname -r)/build

# Show installed kernel source/build directories.
ls /usr/src/kernels/


# ============================================================
# 6. GIT SETUP
# ============================================================

# Check Git installation.
git --version

# Optional: configure Git identity.
git config --global user.name "Your Name"
git config --global user.email "your-email@example.com"

# Verify Git configuration.
git config --global --list


# ============================================================
# 7. CLONE THE CHARACTER DRIVER PROJECT
# ============================================================

# Move to the home directory.
cd ~

# Clone the project from GitHub.
git clone https://github.com/VeronicaGupta/Linux_circular_queue_char_driver.git

# Enter the project.
cd Linux_circular_queue_char_driver

# Inspect repository contents.
ls

# Check repository state.
git status

# Check active branch.
git branch --show-current


# ============================================================
# 8. INSTALL / VERIFY DOCKER
# ============================================================

# Install Docker on Amazon Linux.
sudo dnf install -y docker

# Start Docker immediately.
sudo systemctl start docker

# Enable Docker after reboot.
sudo systemctl enable docker

# Check Docker CLI.
docker --version

# Verify Docker daemon.
sudo docker info


# ============================================================
# 9. BUILD THE DOCKER DRIVER-LAB IMAGE
# ============================================================

# Build the Docker image containing:
# - Linux kernel
# - matching kernel headers
# - GCC/Kbuild
# - BusyBox
# - QEMU
# - character driver source/tests
sudo docker build \
  -t char-driver-lab \
  -f docker/Dockerfile .


# ============================================================
# 10. VERIFY DOCKER IMAGE
# ============================================================

# Confirm the image exists locally.
sudo docker images | grep char-driver-lab


# ============================================================
# 11. CREATE OUTPUT DIRECTORY
# ============================================================

# Store QEMU serial/test logs outside the temporary container.
mkdir -p docker/output


# ============================================================
# 12. RUN AUTOMATED DOCKER + QEMU TEST
# ============================================================

# Start the Docker container.
#
# Docker builds the kernel modules and initramfs.
# QEMU boots a separate Linux kernel.
# Inside QEMU:
#   simple_char.ko is loaded/tested
#   thread_safe_char.ko is loaded/tested
#   lock_free_char.ko is loaded/tested
#   boundary/stress tests execute
sudo docker run --rm \
  -v "$(pwd)/docker/output:/workspace/docker/output" \
  char-driver-lab test


# ============================================================
# 13. CHECK GENERATED TEST LOG
# ============================================================

# Show files produced by the QEMU test.
ls -l docker/output

# Read the complete latest QEMU execution log.
cat docker/output/latest.log

# Search for lines containing "fail".
# This showed both harmless kernel warnings and the test totals.
cat docker/output/latest.log | grep "fail"
```

Your important result was:

```text
Result: 7 passed, 0 failed
Result: 13 passed, 0 failed
Result: 15 passed, 0 failed
```

The following messages were **not test failures**:

```text
ACPI: _OSC evaluation for CPUs failed
acpi ... fail to add MMCONFIG information
```

and:

```text
simple_char: module verification failed:
signature and/or required key missing - tainting kernel
```

The latter means the locally compiled module is unsigned/out-of-tree. It still loaded successfully.

---

### Interactive QEMU practice

After the automated test, the interactive guest was started.

```bash
# ============================================================
# 14. MAKE THE LAB WRAPPER EXECUTABLE
# ============================================================

# Allow the repository helper script to execute.
chmod +x driver-lab.sh


# ============================================================
# 15. START INTERACTIVE QEMU LINUX
# ============================================================

# Boot into the QEMU Linux guest instead of automatically
# executing the test suite.
sudo ./driver-lab.sh shell
```

Inside QEMU the prompt changes from:

```text
[ec2-user@ip-... Linux_circular_queue_char_driver]$
```

to:

```text
~ #
```

That distinction is important:

```text
EC2 shell                  QEMU guest
---------------------      ------------------
[ec2-user@...]$            ~ #
sudo docker ...      →     insmod ...
```

---

### Commands used inside QEMU

```sh
# ============================================================
# 16. SIMPLE CHARACTER DRIVER
# ============================================================

# Load the simple character-device kernel module.
insmod /modules/simple_char.ko
```

Successful output:

```text
simple_char: loading out-of-tree module taints kernel
simple_char: module verification failed...
simple_char: loaded major=241 minor=0
```

Useful inspection commands:

```sh
# Show loaded kernel modules.
lsmod

# Confirm simple_char is loaded.
lsmod | grep simple_char

# Verify the character-device node.
ls -l /dev/simple_char

# Show registered character-device major numbers.
cat /proc/devices

# Inspect recent kernel messages.
dmesg | tail -30
```

Exercise the device:

```sh
# Write and then read HELLO through the character driver.
/bin/char_device_demo simple roundtrip HELLO
```

Unload it:

```sh
# Remove the simple kernel module.
rmmod simple_char
```

---

### Lock-free driver commands used

```sh
# ============================================================
# 17. LOCK-FREE CHARACTER DRIVER
# ============================================================

# This command was initially executed BEFORE loading the module,
# so /dev/lock_free_char did not exist yet.
/bin/char_device_demo lockfree write HELLO
```

It therefore returned:

```text
open for write: No such file or directory
```

Then the module was loaded:

```sh
# Load the SPSC lock-free circular-buffer driver.
insmod /modules/lock_free_char.ko
```

Successful output:

```text
lock_free_char: loaded usable_capacity=4096 SPSC
```

A blocking reader was then started:

```sh
# Open the single permitted reader and wait for 5 bytes.
/bin/char_device_demo lockfree read 5 &
```

Then the boundary tests were run:

```sh
# Test lock-free boundary behavior.
/bin/boundary_tests lockfree
```

Because the previous background command already owned the one permitted reader, this initially produced:

```text
allocate test buffers            PASS
drain queue before test          FAIL
open O_RDWR | O_NONBLOCK         FAIL
Result: 1 passed, 2 failed
```

The stress test was also attempted while the reader was occupied:

```sh
# SPSC stress test:
# transfer 8 MiB using 1024-byte chunks.
/bin/stress_tests stream lockfree 8 1024
```

It therefore reported:

```text
Bytes written: 0
Bytes read:    0
Result: FAIL
```

That was an **SPSC ownership conflict**, not queue corruption.

---

### Correct manual SPSC sequence

The correct sequence is:

```sh
# Start one reader.
# It blocks because the queue is empty.
/bin/char_device_demo lockfree read 5 &

# Supply the five bytes from the single writer.
/bin/char_device_demo lockfree write HELLO

# Wait for the background reader to finish.
wait
```

Then:

```sh
# Verify no char_device_demo process still owns the reader.
ps | grep char_device_demo
```

Now the automated tests can obtain their own reader/writer:

```sh
# Boundary-condition tests.
/bin/boundary_tests lockfree

# 8 MiB SPSC reliability/performance test.
/bin/stress_tests stream lockfree 8 1024
```

---

### Thread-safe driver practice

The corresponding thread-safe commands are:

```sh
# Load mutex/wait-queue implementation.
insmod /modules/thread_safe_char.ko

# Basic read/write test.
/bin/char_device_demo safe roundtrip HELLO

# Boundary tests.
/bin/boundary_tests safe

# SPSC stress/performance test.
/bin/stress_tests stream safe 8 1024

# Multi-producer/multi-consumer concurrency test.
/bin/stress_tests mpmc 4 4 10000

# Remove module.
rmmod thread_safe_char
```

---

### Complete workflow in one picture

```text
Amazon Linux EC2
      │
      ├── dnf
      │    ├── git
      │    ├── gcc
      │    ├── make
      │    └── Docker
      │
      ├── git clone
      │
      └── docker build
              │
              ▼
       char-driver-lab
              │
              ▼
             QEMU
              │
              ├── simple_char.ko
              ├── thread_safe_char.ko
              └── lock_free_char.ko
              │
              ▼
       /dev character devices
              │
              ├── boundary tests
              ├── SPSC stress
              └── MPMC stress
              │
              ▼
       7/7 PASS
       13/13 PASS
       15/15 PASS
```

The single command representing the full automated validation after setup is:

```bash
sudo docker run --rm \
  -v "$(pwd)/docker/output:/workspace/docker/output" \
  char-driver-lab test
```

and the single command for hands-on driver practice is:

```bash
sudo ./driver-lab.sh shell
```
