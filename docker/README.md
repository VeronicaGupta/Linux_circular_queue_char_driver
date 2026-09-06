# Docker/QEMU Harness

The Docker image supplies a Linux kernel image, its matching headers, Kbuild tools, static BusyBox, and QEMU. The scripts then build the existing drivers and boot the same kernel under QEMU.

## Windows

```powershell
.\driver-lab.cmd build
.\driver-lab.cmd test
.\driver-lab.cmd shell
```

## Internal flow

```text
run.ps1
  -> docker build
  -> docker run
       -> lab.sh build
            -> Kbuild three modules
            -> static user/tests
       -> create BusyBox initramfs
       -> qemu-system-x86_64
            -> init-test.sh OR init-shell.sh
```

`init-test.sh` is the automated test PID 1. `init-shell.sh` provides the manual practice environment.
