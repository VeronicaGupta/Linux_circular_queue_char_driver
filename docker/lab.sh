#!/usr/bin/env bash
set -euo pipefail

ROOT=/src
OUTPUT="$ROOT/docker/output"
MODE="${1:-test}"

kernel_version()
{
    ls -1 /lib/modules | sort -V | tail -n 1
}

clean_build_artifacts()
{
    local kver
    kver="$(kernel_version)"
    local kdir="/lib/modules/$kver/build"

    for module_dir in 01_simple_char 02_thread_safe_char 03_lock_free_char; do
        make -C "$ROOT/$module_dir" clean KDIR="$kdir" >/dev/null 2>&1 || true
    done

    rm -f "$ROOT/user/char_device_demo" \
          "$ROOT/tests/boundary_tests" \
          "$ROOT/tests/stress_tests"
    rm -rf "$OUTPUT/build" "$OUTPUT/initramfs.cpio.gz" "$OUTPUT/latest.log"
}

build_all()
{
    local kver kdir
    kver="$(kernel_version)"
    kdir="/lib/modules/$kver/build"

    if [[ ! -f "$kdir/Makefile" ]]; then
        echo "ERROR: installed kernel build tree not found: $kdir" >&2
        exit 2
    fi

    echo "== Target guest kernel =="
    echo "KVER=$kver"
    echo "KDIR=$kdir"

    make -C "$ROOT/01_simple_char" KDIR="$kdir"
    make -C "$ROOT/02_thread_safe_char" KDIR="$kdir"
    make -C "$ROOT/03_lock_free_char" KDIR="$kdir"

    # Static binaries are required because the QEMU initramfs contains no glibc.
    gcc -static -Wall -Wextra -Wpedantic -O2 \
        -o "$ROOT/user/char_device_demo" "$ROOT/user/main.c"

    gcc -static -Wall -Wextra -Wpedantic -O2 -std=c11 \
        -o "$ROOT/tests/boundary_tests" "$ROOT/tests/boundary_tests.c" -pthread

    gcc -static -Wall -Wextra -Wpedantic -O2 -std=c11 \
        -o "$ROOT/tests/stress_tests" "$ROOT/tests/stress_tests.c" -pthread

    mkdir -p "$OUTPUT/build/modules" "$OUTPUT/build/bin"
    cp "$ROOT/01_simple_char/simple_char.ko" "$OUTPUT/build/modules/"
    cp "$ROOT/02_thread_safe_char/thread_safe_char.ko" "$OUTPUT/build/modules/"
    cp "$ROOT/03_lock_free_char/lock_free_char.ko" "$OUTPUT/build/modules/"
    cp "$ROOT/user/char_device_demo" "$OUTPUT/build/bin/"
    cp "$ROOT/tests/boundary_tests" "$OUTPUT/build/bin/"
    cp "$ROOT/tests/stress_tests" "$OUTPUT/build/bin/"
    printf '%s\n' "$kver" > "$OUTPUT/build/kernel-version.txt"

    echo
    echo "Build artifacts: $OUTPUT/build"
}

make_initramfs()
{
    local init_script="$1"
    local initroot=/tmp/char-driver-initramfs

    rm -rf "$initroot"
    mkdir -p "$initroot/bin" "$initroot/modules" "$initroot/proc" \
             "$initroot/sys" "$initroot/dev" "$initroot/tmp"

    cp /usr/bin/busybox "$initroot/bin/busybox"
    cp "$ROOT/docker/$init_script" "$initroot/init"
    chmod +x "$initroot/init"

    cp "$OUTPUT/build/modules/"*.ko "$initroot/modules/"
    cp "$OUTPUT/build/bin/char_device_demo" "$initroot/bin/"
    cp "$OUTPUT/build/bin/boundary_tests" "$initroot/bin/"
    cp "$OUTPUT/build/bin/stress_tests" "$initroot/bin/"

    (
        cd "$initroot"
        find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -9
    ) > "$OUTPUT/initramfs.cpio.gz"
}

qemu_command()
{
    local kver
    kver="$(cat "$OUTPUT/build/kernel-version.txt")"

    local runner=()
    if [[ "${QEMU_TIMEOUT_SECONDS:-0}" != "0" ]]; then
        runner=(timeout "${QEMU_TIMEOUT_SECONDS}s")
    fi

    "${runner[@]}" qemu-system-x86_64 \
        -machine q35,accel=tcg \
        -cpu max \
        -smp 2 \
        -m 768M \
        -nodefaults \
        -no-reboot \
        -kernel "/boot/vmlinuz-$kver" \
        -initrd "$OUTPUT/initramfs.cpio.gz" \
        -append "console=ttyS0 rdinit=/init panic=-1" \
        -serial stdio \
        -monitor none \
        -display none
}

run_tests()
{
    build_all
    make_initramfs init-test.sh

    echo
    echo "== Boot QEMU and run tests =="
    set +e
    QEMU_TIMEOUT_SECONDS=240 qemu_command 2>&1 | tee "$OUTPUT/latest.log"
    local qemu_status=${PIPESTATUS[0]}
    set -e

    if grep -q "DRIVER LAB RESULT: PASS" "$OUTPUT/latest.log"; then
        echo
        echo "Docker/QEMU test result: PASS"
        exit 0
    fi

    echo
    echo "Docker/QEMU test result: FAIL (QEMU status=$qemu_status)" >&2
    exit 1
}

run_shell()
{
    build_all
    make_initramfs init-shell.sh
    echo
    echo "== Boot interactive QEMU guest =="
    qemu_command
}

case "$MODE" in
    build)
        build_all
        ;;
    test)
        run_tests
        ;;
    shell|interactive)
        run_shell
        ;;
    clean)
        clean_build_artifacts
        echo "Clean complete."
        ;;
    *)
        echo "Usage: $0 {build|test|shell|clean}" >&2
        exit 2
        ;;
esac
