#!/bin/busybox sh

# Automated PID 1 for the QEMU guest.
# Loads the three drivers, runs boundary/stress tests, checks the kernel log,
# and powers the guest off with a machine-readable result marker.

/bin/busybox --install -s /bin
mkdir -p /proc /sys /dev /tmp
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

fail()
{
    echo
    echo "DRIVER LAB RESULT: FAIL"
    sync
    poweroff -f
}

run()
{
    echo "+ $*"
    "$@" || fail
}

echo "============================================================"
echo " Linux Character Driver Lab - automated QEMU test"
echo "============================================================"
uname -a

echo
echo "== Load modules =="
run insmod /modules/simple_char.ko
run insmod /modules/thread_safe_char.ko
run insmod /modules/lock_free_char.ko

run lsmod

echo
echo "== Device nodes =="
run ls -l /dev/simple_char /dev/thread_safe_char /dev/lock_free_char

echo
echo "== Basic user-space usage =="
run /bin/char_device_demo simple roundtrip SIMPLE_OK
run /bin/char_device_demo safe roundtrip SAFE_OK
run /bin/char_device_demo lockfree roundtrip LOCKFREE_OK

echo
echo "== Boundary tests =="
run /bin/boundary_tests all

echo
echo "== SPSC reliability/performance =="
run /bin/stress_tests stream safe 8 1024
run /bin/stress_tests stream lockfree 8 1024

echo
echo "== Thread-safe MPMC reliability =="
run /bin/stress_tests mpmc 4 4 10000

echo
echo "== Unsynchronized basic-driver race observation =="
run /bin/stress_tests simple-race 4 10

echo
echo "== Kernel diagnostics =="
dmesg | tail -80

if dmesg | grep -E "BUG:|Oops:|Kernel panic|general protection fault|KASAN:|WARNING:.*lock" >/dev/null 2>&1; then
    echo "Kernel diagnostic failure pattern detected."
    fail
fi

echo
echo "== Unload modules =="
run rmmod lock_free_char
run rmmod thread_safe_char
run rmmod simple_char

echo
echo "DRIVER LAB RESULT: PASS"
sync
poweroff -f
