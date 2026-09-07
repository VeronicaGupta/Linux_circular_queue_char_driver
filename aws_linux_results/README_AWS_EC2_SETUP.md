# AWS EC2 Setup and Reset — Linux Character Driver Lab

This setup targets **Amazon Linux 2023 on an x86_64 EC2 instance** and prepares the complete Docker + QEMU character-driver environment for:

- `simple_char`;
- `thread_safe_char`;
- `lock_free_char`;
- boundary tests;
- SPSC stress/throughput tests;
- MPMC reliability testing for the mutex driver;
- persisted QEMU serial logs.

The repository under test is:

`https://github.com/VeronicaGupta/Linux_circular_queue_char_driver`

The project Docker image supplies the guest Linux kernel, matching kernel headers, compiler toolchain, BusyBox initramfs, and QEMU. Host `kernel-devel` packages are therefore **not required for the Docker/QEMU test path**.

## Files

```text
ec2-driver-lab.sh          # complete setup/test/reset script
README_AWS_EC2_SETUP.md    # this guide
```

## Initial EC2 requirements

The EC2 instance should use:

```text
Amazon Linux 2023
x86_64
2+ vCPU recommended
2+ GiB RAM recommended
internet access to GitHub and package repositories
```

The script uses `sudo` for package management, Docker service configuration, and Docker commands.

## Copy the script to EC2

After the script is present on the instance:

```bash
chmod +x ec2-driver-lab.sh
```

Display available commands:

```bash
./ec2-driver-lab.sh help
```

## Complete setup

The full automated setup is:

```bash
./ec2-driver-lab.sh setup
```

Optional Git identity can be supplied during setup:

```bash
GIT_NAME="Veronica Gupta" \
GIT_EMAIL="your-email@example.com" \
./ec2-driver-lab.sh setup
```

`setup` performs the following sequence:

```text
Amazon Linux 2023 validation
        |
        v
dnf update
        |
        v
install git/docker/gcc/g++/make/openssh-clients
        |
        v
systemctl enable --now docker
        |
        v
create dedicated GitHub SSH key
        |
        v
clone/update public GitHub repository over HTTPS
        |
        v
docker build -t char-driver-lab -f docker/Dockerfile .
        |
        v
docker run ... char-driver-lab test
        |
        v
QEMU guest boots
        |
        +--> simple driver tests
        +--> thread-safe driver tests
        +--> lock-free SPSC tests
        |
        v
DRIVER LAB RESULT: PASS
Docker/QEMU test result: PASS
```

The script returns a non-zero status if the Docker command fails, any structured test result reports a non-zero failed count, or either final PASS marker is missing.

### Important failure parsing

QEMU and unsigned development kernel modules can emit benign messages containing words such as `failed`, for example ACPI messages or module-signature warnings. The setup script therefore does **not** treat a generic `grep fail` match as a failed test.

Only structured failures such as:

```text
Result: 12 passed, 1 failed
```

or missing final PASS markers cause the setup to fail.

## Re-run only the tests

After setup is complete:

```bash
./ec2-driver-lab.sh test
```

The existing Docker image is reused. If the image is missing, it is rebuilt automatically.

QEMU logs are stored under:

```text
~/Linux_circular_queue_char_driver/docker/output/
```

The current guest serial log is normally:

```bash
cat ~/Linux_circular_queue_char_driver/docker/output/latest.log
```

## Check setup status

```bash
./ec2-driver-lab.sh status
```

Status includes:

- host kernel and architecture;
- Docker daemon availability;
- repository presence and Git remote;
- Docker image presence;
- dedicated GitHub SSH-key path;
- current QEMU log path.

## GitHub SSH key

`setup` creates a dedicated key:

```text
~/.ssh/id_ed25519_github_char_driver_lab
~/.ssh/id_ed25519_github_char_driver_lab.pub
```

The private key remains on the EC2 instance. Only the `.pub` key should be added to GitHub.

Print the public key at any time:

```bash
./ec2-driver-lab.sh show-key
```

Add that line in GitHub under:

```text
GitHub
  -> Settings
  -> SSH and GPG keys
  -> New SSH key
```

The public repository is cloned over HTTPS during setup, so QEMU testing does not wait for GitHub SSH registration.

After the public key has been added to GitHub, verify it and switch the repository remote to SSH:

```bash
./ec2-driver-lab.sh git-ssh
```

Expected GitHub authentication text contains:

```text
You've successfully authenticated
```

The repository remote then becomes:

```text
git@github.com:VeronicaGupta/Linux_circular_queue_char_driver.git
```

## Interactive driver practice

After setup passes:

```bash
cd ~/Linux_circular_queue_char_driver
sudo ./driver-lab.sh shell
```

The QEMU guest prompt appears as approximately:

```text
~ #
```

Example simple-driver practice:

```sh
insmod /modules/simple_char.ko
lsmod | grep simple_char
ls -l /dev/simple_char
cat /proc/devices
/bin/char_device_demo simple roundtrip HELLO
dmesg | tail -30
rmmod simple_char
```

Thread-safe driver:

```sh
insmod /modules/thread_safe_char.ko
/bin/char_device_demo safe roundtrip HELLO
/bin/boundary_tests safe
/bin/stress_tests stream safe 8 1024
/bin/stress_tests mpmc 4 4 10000
rmmod thread_safe_char
```

Lock-free SPSC driver:

```sh
insmod /modules/lock_free_char.ko
/bin/char_device_demo lockfree write HELLO
/bin/boundary_tests lockfree
/bin/stress_tests stream lockfree 8 1024
rmmod lock_free_char
```

A manually started blocking lock-free reader should be completed before automated lock-free tests are launched because the driver intentionally permits only one active reader and one active writer.

## Safe reset

The normal reset command is:

```bash
./ec2-driver-lab.sh reset
```

`reset` removes:

```text
Docker containers created from char-driver-lab
char-driver-lab Docker image
QEMU generated output/logs
```

`reset` preserves:

```text
cloned Git repository
source-code changes
Git configuration
GitHub SSH key
installed host packages
Docker installation/service
```

A clean lab can then be rebuilt with:

```bash
./ec2-driver-lab.sh setup
```

## Full reset

A destructive reset is available when a completely fresh repository/key state is required:

```bash
./ec2-driver-lab.sh reset-all --yes
```

`reset-all` performs the safe reset and additionally deletes:

```text
~/Linux_circular_queue_char_driver
~/.ssh/id_ed25519_github_char_driver_lab
~/.ssh/id_ed25519_github_char_driver_lab.pub
```

Installed RPM packages, Docker itself, the Docker service, and global Git identity are intentionally retained.

The explicit `--yes` argument prevents accidental deletion.

## Recommended lifecycle

```bash
# Initial provisioning and first complete QEMU validation.
./ec2-driver-lab.sh setup

# Inspect state.
./ec2-driver-lab.sh status

# Re-run automated driver validation after source changes.
./ec2-driver-lab.sh test

# Enter the QEMU guest for manual character-driver practice.
cd ~/Linux_circular_queue_char_driver
sudo ./driver-lab.sh shell

# Remove generated Docker/QEMU state while retaining source and credentials.
cd ~
./ec2-driver-lab.sh reset

# Rebuild and re-test from the retained source checkout.
./ec2-driver-lab.sh setup
```

## Notes

- Docker installation on Amazon Linux 2023 is performed with the `docker` package and the Docker service is started with systemd.
- The script continues to invoke Docker through `sudo` during the active setup session. Adding the login account to the `docker` group affects future login sessions and is not relied upon for correctness.
- Host kernel headers are unnecessary for this workflow because kernel modules are compiled for the kernel that QEMU boots inside the Docker-controlled lab.
- A QEMU/ACPI log line containing `failed` is not equivalent to a test failure. Structured test counters and final PASS markers determine success.
