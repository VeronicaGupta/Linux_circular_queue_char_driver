// SPDX-License-Identifier: GPL-2.0
/* Bounded MPMC byte FIFO using a mutex and wait queues. */

#include <linux/fcntl.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "thread_safe_char.h"

static unsigned long buffer_size = THREAD_SAFE_DEFAULT_CAPACITY;
module_param(buffer_size, ulong, 0444);
MODULE_PARM_DESC(buffer_size, "Usable FIFO capacity in bytes");

struct thread_safe_device {
	char *buffer;
	size_t capacity;
	size_t read_index;
	size_t write_index;
	size_t used;
	struct mutex lock;
	wait_queue_head_t read_wait;
	wait_queue_head_t write_wait;
};

static struct thread_safe_device *safe_device;

static size_t thread_safe_copy_to_user(struct thread_safe_device *device,
				       char __user *user_buffer,
				       size_t bytes)
{
	size_t first = min(bytes, device->capacity - device->read_index);

	if (copy_to_user(user_buffer, device->buffer + device->read_index, first))
		return 0;

	if (bytes > first &&
	    copy_to_user(user_buffer + first, device->buffer, bytes - first))
		return 0;

	return bytes;
}

static size_t thread_safe_copy_from_user(struct thread_safe_device *device,
					 const char __user *user_buffer,
					 size_t bytes)
{
	size_t first = min(bytes, device->capacity - device->write_index);

	if (copy_from_user(device->buffer + device->write_index,
			   user_buffer, first))
		return 0;

	if (bytes > first &&
	    copy_from_user(device->buffer, user_buffer + first, bytes - first))
		return 0;

	return bytes;
}

static ssize_t thread_safe_char_read(struct file *file, char __user *user_buffer,
				     size_t requested, loff_t *offset)
{
	struct thread_safe_device *device = safe_device;
	size_t bytes_to_copy;

	if (requested == 0)
		return 0;

	if (mutex_lock_interruptible(&device->lock))
		return -ERESTARTSYS;

	while (device->used == 0) {
		mutex_unlock(&device->lock);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(device->read_wait,
					     READ_ONCE(device->used) > 0))
			return -ERESTARTSYS;

		if (mutex_lock_interruptible(&device->lock))
			return -ERESTARTSYS;
	}

	bytes_to_copy = min(requested, device->used);
	if (thread_safe_copy_to_user(device, user_buffer, bytes_to_copy) !=
	    bytes_to_copy) {
		mutex_unlock(&device->lock);
		return -EFAULT;
	}

	device->read_index = (device->read_index + bytes_to_copy) % device->capacity;
	device->used -= bytes_to_copy;
	mutex_unlock(&device->lock);

	wake_up_interruptible(&device->write_wait);
	return bytes_to_copy;
}

static ssize_t thread_safe_char_write(struct file *file,
				      const char __user *user_buffer,
				      size_t requested, loff_t *offset)
{
	struct thread_safe_device *device = safe_device;
	size_t free_space;
	size_t bytes_to_copy;

	if (requested == 0)
		return 0;

	if (mutex_lock_interruptible(&device->lock))
		return -ERESTARTSYS;

	while (device->used == device->capacity) {
		mutex_unlock(&device->lock);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(device->write_wait,
					     READ_ONCE(device->used) < device->capacity))
			return -ERESTARTSYS;

		if (mutex_lock_interruptible(&device->lock))
			return -ERESTARTSYS;
	}

	free_space = device->capacity - device->used;
	bytes_to_copy = min(requested, free_space);
	if (thread_safe_copy_from_user(device, user_buffer, bytes_to_copy) !=
	    bytes_to_copy) {
		mutex_unlock(&device->lock);
		return -EFAULT;
	}

	device->write_index = (device->write_index + bytes_to_copy) % device->capacity;
	device->used += bytes_to_copy;
	mutex_unlock(&device->lock);

	wake_up_interruptible(&device->read_wait);
	return bytes_to_copy;
}

static __poll_t thread_safe_char_poll(struct file *file, poll_table *wait)
{
	struct thread_safe_device *device = safe_device;
	__poll_t mask = 0;

	poll_wait(file, &device->read_wait, wait);
	poll_wait(file, &device->write_wait, wait);

	if (READ_ONCE(device->used) > 0)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(device->used) < device->capacity)
		mask |= EPOLLOUT | EPOLLWRNORM;

	return mask;
}

static const struct file_operations thread_safe_operations = {
	.owner = THIS_MODULE,
	.read = thread_safe_char_read,
	.write = thread_safe_char_write,
	.poll = thread_safe_char_poll,
};

static struct miscdevice thread_safe_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = THREAD_SAFE_DEVICE_NAME,
	.fops = &thread_safe_operations,
	.mode = 0666,
};

static int __init thread_safe_char_init(void)
{
	int result;

	if (buffer_size == 0)
		return -EINVAL;

	safe_device = kzalloc(sizeof(*safe_device), GFP_KERNEL);
	if (!safe_device)
		return -ENOMEM;

	safe_device->buffer = kmalloc(buffer_size, GFP_KERNEL);
	if (!safe_device->buffer) {
		kfree(safe_device);
		return -ENOMEM;
	}

	safe_device->capacity = buffer_size;
	mutex_init(&safe_device->lock);
	init_waitqueue_head(&safe_device->read_wait);
	init_waitqueue_head(&safe_device->write_wait);

	result = misc_register(&thread_safe_misc_device);
	if (result) {
		kfree(safe_device->buffer);
		kfree(safe_device);
		return result;
	}

	pr_info("thread_safe_char: loaded capacity=%lu\n", buffer_size);
	return 0;
}

static void __exit thread_safe_char_exit(void)
{
	misc_deregister(&thread_safe_misc_device);
	kfree(safe_device->buffer);
	kfree(safe_device);
	pr_info("thread_safe_char: unloaded\n");
}

module_init(thread_safe_char_init);
module_exit(thread_safe_char_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux Character Driver Lab");
MODULE_DESCRIPTION("Thread-safe MPMC circular character device");
