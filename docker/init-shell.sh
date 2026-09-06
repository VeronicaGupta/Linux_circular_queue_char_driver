#!/bin/busybox sh

# Interactive PID 1 for manual driver practice inside the QEMU guest.

/bin/busybox --install -s /bin
mkdir -p /proc /sys /dev /tmp
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

cat <<'BANNER'
============================================================
 Linux Character Driver Lab - interactive guest
============================================================

Modules are intentionally NOT loaded.

Suggested practice:

  uname -a
  ls /modules

  insmod /modules/simple_char.ko
  lsmod
  ls -l /dev/simple_char
  cat /proc/devices
  /bin/char_device_demo simple roundtrip HELLO
  dmesg | tail -30
  rmmod simple_char

  insmod /modules/thread_safe_char.ko
  /bin/char_device_demo safe read 5 &
  /bin/char_device_demo safe write HELLO
  wait
  /bin/boundary_tests safe
  /bin/stress_tests stream safe 8 1024
  /bin/stress_tests mpmc 4 4 10000
  rmmod thread_safe_char

  insmod /modules/lock_free_char.ko
  /bin/char_device_demo lockfree read 5 &
  /bin/char_device_demo lockfree write HELLO
  wait
  /bin/boundary_tests lockfree
  /bin/stress_tests stream lockfree 8 1024
  rmmod lock_free_char

  dmesg | tail -80
  poweroff -f

BANNER

exec /bin/sh
