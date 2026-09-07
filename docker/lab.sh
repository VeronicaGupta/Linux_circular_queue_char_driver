#!/usr/bin/env bash
set -euo pipefail

ROOT=/workspace
OUTPUT="$ROOT/docker/output"
BUILD="$OUTPUT/build"
ROOTFS="$BUILD/rootfs"
KVER="$(ls -1 /lib/modules | sort -V | tail -n 1)"
KDIR="/lib/modules/$KVER/build"
KERNEL_IMAGE="/boot/vmlinuz-$KVER"

log() { printf '== %s ==\n' "$*"; }

build_all() {
    log "Target guest kernel"
    echo "KVER=$KVER"
    echo "KDIR=$KDIR"

    rm -rf "$BUILD"
    mkdir -p "$BUILD/modules" "$BUILD/bin" "$ROOTFS"

    make -C "$ROOT/01_simple_char" clean KDIR="$KDIR" >/dev/null || true
    make -C "$ROOT/02_thread_safe_char" clean KDIR="$KDIR" >/dev/null || true
    make -C "$ROOT/03_lock_free_char" clean KDIR="$KDIR" >/dev/null || true

    make -C "$ROOT/01_simple_char" KDIR="$KDIR"
    make -C "$ROOT/02_thread_safe_char" KDIR="$KDIR"
    make -C "$ROOT/03_lock_free_char" KDIR="$KDIR"
    make -C "$ROOT/user" clean all STATIC=1
    make -C "$ROOT/tests" clean all STATIC=1

    cp "$ROOT/01_simple_char/simple_char.ko" "$BUILD/modules/"
    cp "$ROOT/02_thread_safe_char/thread_safe_char.ko" "$BUILD/modules/"
    cp "$ROOT/03_lock_free_char/lock_free_char.ko" "$BUILD/modules/"
    cp "$ROOT/user/char_device_demo" "$BUILD/bin/"
    cp "$ROOT/tests/boundary_tests" "$BUILD/bin/"
    cp "$ROOT/tests/stress_tests" "$BUILD/bin/"
    printf '%s\n' "$KVER" > "$BUILD/kernel-version.txt"

    log "Build complete"
    find "$BUILD/modules" "$BUILD/bin" -maxdepth 1 -type f -printf '%p\n'
}

make_initramfs() {
    local mode="$1"
    local init_script
    local archive="$OUTPUT/initramfs-${mode}.cpio.gz"

    if [[ "$mode" == "test" ]]; then
        init_script="$ROOT/docker/init-test.sh"
    else
        init_script="$ROOT/docker/init-shell.sh"
    fi

    rm -rf "$ROOTFS"
    mkdir -p "$ROOTFS"/{bin,dev,proc,sys,tmp,modules}
    cp /bin/busybox "$ROOTFS/bin/busybox"
    cp "$BUILD/bin/char_device_demo" "$ROOTFS/bin/"
    cp "$BUILD/bin/boundary_tests" "$ROOTFS/bin/"
    cp "$BUILD/bin/stress_tests" "$ROOTFS/bin/"
    cp "$BUILD/modules/"*.ko "$ROOTFS/modules/"
    cp "$init_script" "$ROOTFS/init"
    chmod +x "$ROOTFS/init" "$ROOTFS/bin/"*

    (cd "$ROOTFS" && find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9) > "$archive"
    echo "$archive"
}

run_qemu() {
    local mode="$1"
    local initramfs
    local log_file="$OUTPUT/latest.log"

    initramfs="$(make_initramfs "$mode")"
    log "Booting QEMU ($mode)"

    if [[ "$mode" == "test" ]]; then
        set +e
        qemu-system-x86_64 \
            -machine accel=tcg \
            -cpu max \
            -m 768M \
            -smp 2 \
            -kernel "$KERNEL_IMAGE" \
            -initrd "$initramfs" \
            -append "console=ttyS0 rdinit=/init panic=-1" \
            -nographic -no-reboot 2>&1 | tee "$log_file"
        qemu_status=${PIPESTATUS[0]}
        set -e

        if grep -q "DRIVER LAB RESULT: PASS" "$log_file"; then
            echo "Docker/QEMU test result: PASS"
            exit 0
        fi

        echo "Docker/QEMU test result: FAIL"
        exit "${qemu_status:-1}"
    else
        exec qemu-system-x86_64 \
            -machine accel=tcg \
            -cpu max \
            -m 768M \
            -smp 2 \
            -kernel "$KERNEL_IMAGE" \
            -initrd "$initramfs" \
            -append "console=ttyS0 rdinit=/init panic=-1" \
            -nographic -no-reboot
    fi
}

clean_all() {
    rm -rf "$OUTPUT/build" "$OUTPUT"/*.cpio.gz "$OUTPUT"/*.log
    make -C "$ROOT/01_simple_char" clean KDIR="$KDIR" || true
    make -C "$ROOT/02_thread_safe_char" clean KDIR="$KDIR" || true
    make -C "$ROOT/03_lock_free_char" clean KDIR="$KDIR" || true
    make -C "$ROOT/user" clean || true
    make -C "$ROOT/tests" clean || true
}

case "${1:-help}" in
    build)
        build_all
        ;;
    test)
        build_all
        run_qemu test
        ;;
    shell)
        build_all
        run_qemu shell
        ;;
    clean)
        clean_all
        ;;
    help|*)
        cat <<'HELP'
Linux Character Driver Docker/QEMU Lab

Commands:
  build   Compile all three .ko modules and static user/test binaries
  test    Build, boot QEMU, load drivers, and run automated tests
  shell   Build and boot an interactive QEMU Linux shell
  clean   Remove generated build/test artifacts
HELP
        ;;
esac
