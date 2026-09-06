#!/usr/bin/env bash
set -u

kernel_release="$(uname -r)"
default_kdir="/lib/modules/${kernel_release}/build"

printf '%s\n' "Linux character-driver environment"
printf '%-22s %s\n' "Kernel:" "$kernel_release"
printf '%-22s %s\n' "Architecture:" "$(uname -m)"

if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    printf '%-22s %s\n' "User space:" "${PRETTY_NAME:-unknown}"
fi

if command -v systemd-detect-virt >/dev/null 2>&1; then
    virtualization="$(systemd-detect-virt 2>/dev/null || true)"
    printf '%-22s %s\n' "Virtualization:" "${virtualization:-none detected}"
fi

printf '%-22s %s\n' "Default KDIR:" "$default_kdir"

if [[ -f "$default_kdir/Makefile" ]]; then
    printf '%-22s %s\n' "Kernel build tree:" "READY"
    printf '%s\n' "Kernel modules can be compiled with: make modules"
else
    printf '%-22s %s\n' "Kernel build tree:" "NOT AVAILABLE"
    cat <<'TEXT'

Available in the current environment:
  make
  make fydeos
  make -C user
  make -C tests

Kernel-module compilation requires the exact prepared target-kernel tree:
  make modules KDIR=/absolute/path/to/kernel-build-tree

A .ko file built for another kernel release must not be loaded into this kernel.
TEXT
fi
