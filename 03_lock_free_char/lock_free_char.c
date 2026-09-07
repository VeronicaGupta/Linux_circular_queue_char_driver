// SPDX-License-Identifier: GPL-2.0
/* SPSC lock-free byte FIFO using index ownership and acquire/release ordering. */

#include <linux/atomic.h>
#include <linux/fcntl.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "lock_free_char.h"

static unsigned long buffer_size = LOCK_FREE_DEFAULT_CAPACITY;
module_param(buffer_size, ulong, 0444);
MODULE_PARM_DESC(buffer_size, "Usable FIFO capacity in bytes");

struct lock_free_device {
	char *buffer;
	size_t ring_size;
	size_t read_index;
	size_t write_index;
	wait_queue_head_t read_wait;
	wait_queue_head_t write_wait;
	atomic_t reader_open;
	atomic_t writer_open;
};

struct lock_free_file_context {
	struct lock_free_device *device;
	bool owns_reader;
	bool owns_writer;
};

static struct lock_free_device *lock_free_device;

static size_t lock_free_used(size_t read_index, size_t write_index,
			     size_t ring_size)
{
	return (write_index + ring_size - read_index) % ring_size;
}

static int lock_free_char_open(struct inode *inode, struct file *file)
{
	struct lock_free_file_context *context;
	bool needs_reader = (file->f_mode & FMODE_READ) != 0;
	bool needs_writer = (file->f_mode & FMODE_WRITE) != 0;

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;

	context->device = lock_free_device;

	if (needs_reader) {
		if (atomic_cmpxchg(&lock_free_device->reader_open, 0, 1) != 0) {
			kfree(context);
			return -EBUSY;
		}
		context->owns_reader = true;
	}

	if (needs_writer) {
		if (atomic_cmpxchg(&lock_free_device->writer_open, 0, 1) != 0) {
			if (context->owns_reader)
				atomic_set(&lock_free_device->reader_open, 0);
			kfree(context);
			return -EBUSY;
		}
		context->owns_writer = true;
	}

	file->private_data = context;
	return 0;
}

static int lock_free_char_release(struct inode *inode, struct file *file)
{
	struct lock_free_file_context *context = file->private_data;

	if (context->owns_reader)
		atomic_set(&context->device->reader_open, 0);
	if (context->owns_writer)
		atomic_set(&context->device->writer_open, 0);

	kfree(context);
	return 0;
}

static ssize_t lock_free_char_read(struct file *file, char __user *user_buffer,
				   size_t requested, loff_t *offset)
{
	struct lock_free_file_context *context = file->private_data;
	struct lock_free_device *device = context->device;
	size_t read_index;
	size_t write_index;
	size_t available;
	size_t bytes_to_copy;
	size_t first;

	if (!context->owns_reader)
		return -EBADF;
	if (requested == 0)
		return 0;

	for (;;) {
		read_index = READ_ONCE(device->read_index);
		write_index = smp_load_acquire(&device->write_index);
		if (read_index != write_index)
			break;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(device->read_wait,
				smp_load_acquire(&device->write_index) !=
				READ_ONCE(device->read_index)))
			return -ERESTARTSYS;
	}

	available = lock_free_used(read_index, write_index, device->ring_size);
	bytes_to_copy = min(requested, available);
	first = min(bytes_to_copy, device->ring_size - read_index);

	if (copy_to_user(user_buffer, device->buffer + read_index, first))
		return -EFAULT;
	if (bytes_to_copy > first &&
	    copy_to_user(user_buffer + first, device->buffer,
			 bytes_to_copy - first))
		return -EFAULT;

	read_index = (read_index + bytes_to_copy) % device->ring_size;
	smp_store_release(&device->read_index, read_index);
	wake_up_interruptible(&device->write_wait);
	return bytes_to_copy;
}

static ssize_t lock_free_char_write(struct file *file,
				    const char __user *user_buffer,
				    size_t requested, loff_t *offset)
{
	struct lock_free_file_context *context = file->private_data;
	struct lock_free_device *device = context->device;
	size_t read_index;
	size_t write_index;
	size_t used;
	size_t free_space;
	size_t bytes_to_copy;
	size_t first;

	if (!context->owns_writer)
		return -EBADF;
	if (requested == 0)
		return 0;

	for (;;) {
		write_index = READ_ONCE(device->write_index);
		read_index = smp_load_acquire(&device->read_index);
		used = lock_free_used(read_index, write_index, device->ring_size);
		free_space = device->ring_size - 1 - used;
		if (free_space > 0)
			break;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(device->write_wait,
				smp_load_acquire(&device->read_index) !=
				((READ_ONCE(device->write_index) + 1) % device->ring_size)))
			return -ERESTARTSYS;
	}

	bytes_to_copy = min(requested, free_space);
	first = min(bytes_to_copy, device->ring_size - write_index);

	if (copy_from_user(device->buffer + write_index, user_buffer, first))
		return -EFAULT;
	if (bytes_to_copy > first &&
	    copy_from_user(device->buffer, user_buffer + first,
			   bytes_to_copy - first))
		return -EFAULT;

	write_index = (write_index + bytes_to_copy) % device->ring_size;
	smp_store_release(&device->write_index, write_index);
	wake_up_interruptible(&device->read_wait);
	return bytes_to_copy;
}

static __poll_t lock_free_char_poll(struct file *file, poll_table *wait)
{
	struct lock_free_file_context *context = file->private_data;
	struct lock_free_device *device = context->device;
	size_t read_index;
	size_t write_index;
	size_t used;
	__poll_t mask = 0;

	poll_wait(file, &device->read_wait, wait);
	poll_wait(file, &device->write_wait, wait);

	read_index = smp_load_acquire(&device->read_index);
	write_index = smp_load_acquire(&device->write_index);
	used = lock_free_used(read_index, write_index, device->ring_size);

	if (context->owns_reader && used > 0)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (context->owns_writer && used < device->ring_size - 1)
		mask |= EPOLLOUT | EPOLLWRNORM;

	return mask;
}

static const struct file_operations lock_free_operations = {
	.owner = THIS_MODULE,
	.open = lock_free_char_open,
	.release = lock_free_char_release,
	.read = lock_free_char_read,
	.write = lock_free_char_write,
	.poll = lock_free_char_poll,
};

static struct miscdevice lock_free_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = LOCK_FREE_DEVICE_NAME,
	.fops = &lock_free_operations,
	.mode = 0666,
};

static int __init lock_free_char_init(void)
{
	int result;

	if (buffer_size == 0 || buffer_size == ULONG_MAX)
		return -EINVAL;

	lock_free_device = kzalloc(sizeof(*lock_free_device), GFP_KERNEL);
	if (!lock_free_device)
		return -ENOMEM;

	lock_free_device->ring_size = buffer_size + 1;
	lock_free_device->buffer = kmalloc(lock_free_device->ring_size, GFP_KERNEL);
	if (!lock_free_device->buffer) {
		kfree(lock_free_device);
		return -ENOMEM;
	}

	init_waitqueue_head(&lock_free_device->read_wait);
	init_waitqueue_head(&lock_free_device->write_wait);
	atomic_set(&lock_free_device->reader_open, 0);
	atomic_set(&lock_free_device->writer_open, 0);

	result = misc_register(&lock_free_misc_device);
	if (result) {
		kfree(lock_free_device->buffer);
		kfree(lock_free_device);
		return result;
	}

	pr_info("lock_free_char: loaded usable_capacity=%lu SPSC\n", buffer_size);
	return 0;
}

static void __exit lock_free_char_exit(void)
{
	misc_deregister(&lock_free_misc_device);
	kfree(lock_free_device->buffer);
	kfree(lock_free_device);
	pr_info("lock_free_char: unloaded\n");
}

module_init(lock_free_char_init);
module_exit(lock_free_char_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux Character Driver Lab");
MODULE_DESCRIPTION("SPSC lock-free circular character device");
