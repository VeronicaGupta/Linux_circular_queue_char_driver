#!/bin/busybox sh
set -e

/bin/busybox --install -s /bin
mkdir -p /proc /sys /dev /tmp
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

fail() {
    echo "DRIVER LAB RESULT: FAIL"
    dmesg | tail -100
    poweroff -f
}
trap fail EXIT

echo "============================================================"
echo " Linux Character Driver Lab - automated tests"
echo "============================================================"
uname -a

# Minimal driver: registration and byte-buffer behavior.
insmod /modules/simple_char.ko
[ -c /dev/simple_char ]
/bin/char_device_demo simple roundtrip HELLO
/bin/boundary_tests simple
rmmod simple_char

# Mutex + wait-queue MPMC FIFO.
insmod /modules/thread_safe_char.ko
[ -c /dev/thread_safe_char ]
/bin/char_device_demo safe roundtrip HELLO
/bin/boundary_tests safe
/bin/stress_tests stream safe 8 1024
/bin/stress_tests mpmc 4 4 10000
rmmod thread_safe_char

# SPSC lock-free FIFO.
insmod /modules/lock_free_char.ko
[ -c /dev/lock_free_char ]
/bin/char_device_demo lockfree roundtrip HELLO
/bin/boundary_tests lockfree
/bin/stress_tests stream lockfree 8 1024
rmmod lock_free_char

echo "--- kernel log tail ---"
dmesg | tail -80

echo "DRIVER LAB RESULT: PASS"
trap - EXIT
poweroff -f
