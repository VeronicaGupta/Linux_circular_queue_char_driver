// SPDX-License-Identifier: GPL-2.0
/* SPSC lock-free bounded byte FIFO exposed through a miscellaneous device. */

#include <linux/atomic.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/version.h>

#include "lock_free_char.h"

struct lock_free_device {
	char *buffer;
	size_t read_index;
	size_t write_index;
	wait_queue_head_t read_wait;
	wait_queue_head_t write_wait;
	atomic_t reader_open;
	atomic_t writer_open;
};

static struct lock_free_device queue_device;

static size_t ring_used(size_t read_index, size_t write_index)
{
	if (write_index >= read_index)
		return write_index - read_index;

	return LOCK_FREE_RING_SIZE - (read_index - write_index);
}

static size_t ring_free(size_t read_index, size_t write_index)
{
	return LOCK_FREE_CAPACITY - ring_used(read_index, write_index);
}

static size_t ring_copy_out(char *destination, const char *source,
			    size_t read_index, size_t count)
{
	size_t first_part;

	first_part = min(count, (size_t)LOCK_FREE_RING_SIZE - read_index);
	memcpy(destination, source + read_index, first_part);

	if (count > first_part)
		memcpy(destination + first_part, source, count - first_part);

	return count;
}

static size_t ring_copy_in(char *destination, size_t write_index,
			   const char *source, size_t count)
{
	size_t first_part;

	first_part = min(count, (size_t)LOCK_FREE_RING_SIZE - write_index);
	memcpy(destination + write_index, source, first_part);

	if (count > first_part)
		memcpy(destination, source + first_part, count - first_part);

	return count;
}

static bool lock_free_data_available(struct lock_free_device *device)
{
	size_t read_index = READ_ONCE(device->read_index);
	size_t write_index = smp_load_acquire(&device->write_index);

	return read_index != write_index;
}

static bool lock_free_space_available(struct lock_free_device *device)
{
	size_t write_index = READ_ONCE(device->write_index);
	size_t read_index = smp_load_acquire(&device->read_index);

	return ring_free(read_index, write_index) > 0;
}

static int lock_free_open(struct inode *inode, struct file *file)
{
	bool reader_reserved = false;

	if (file->f_mode & FMODE_READ) {
		if (atomic_cmpxchg(&queue_device.reader_open, 0, 1) != 0)
			return -EBUSY;
		reader_reserved = true;
	}

	if (file->f_mode & FMODE_WRITE) {
		if (atomic_cmpxchg(&queue_device.writer_open, 0, 1) != 0) {
			if (reader_reserved)
				atomic_set(&queue_device.reader_open, 0);
			return -EBUSY;
		}
	}

	file->private_data = &queue_device;
	return 0;
}

static int lock_free_release(struct inode *inode, struct file *file)
{
	if (file->f_mode & FMODE_READ)
		atomic_set(&queue_device.reader_open, 0);

	if (file->f_mode & FMODE_WRITE)
		atomic_set(&queue_device.writer_open, 0);

	return 0;
}

static ssize_t lock_free_read(struct file *file, char __user *user_buffer,
			      size_t count, loff_t *offset)
{
	struct lock_free_device *device = file->private_data;
	char *temporary_buffer;
	size_t read_index;
	size_t write_index;
	size_t bytes_available;
	size_t bytes_to_read;
	size_t new_read_index;
	int result;

	if (count == 0)
		return 0;

	while (!lock_free_data_available(device)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		result = wait_event_interruptible(device->read_wait,
						  lock_free_data_available(device));
		if (result)
			return -ERESTARTSYS;
	}

	read_index = READ_ONCE(device->read_index);
	write_index = smp_load_acquire(&device->write_index);
	bytes_available = ring_used(read_index, write_index);
	bytes_to_read = min(count, bytes_available);

	temporary_buffer = kmalloc(bytes_to_read, GFP_KERNEL);
	if (!temporary_buffer)
		return -ENOMEM;

	ring_copy_out(temporary_buffer, device->buffer, read_index, bytes_to_read);

	if (copy_to_user(user_buffer, temporary_buffer, bytes_to_read)) {
		kfree(temporary_buffer);
		return -EFAULT;
	}

	kfree(temporary_buffer);
	new_read_index = (read_index + bytes_to_read) % LOCK_FREE_RING_SIZE;

	/* Publish freed space after all buffer reads complete. */
	smp_store_release(&device->read_index, new_read_index);
	wake_up_interruptible(&device->write_wait);

	return bytes_to_read;
}

static ssize_t lock_free_write(struct file *file,
			       const char __user *user_buffer,
			       size_t count, loff_t *offset)
{
	struct lock_free_device *device = file->private_data;
	char *temporary_buffer;
	size_t read_index;
	size_t write_index;
	size_t free_space;
	size_t bytes_to_write;
	size_t new_write_index;
	int result;

	if (count == 0)
		return 0;

	while (!lock_free_space_available(device)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		result = wait_event_interruptible(device->write_wait,
						  lock_free_space_available(device));
		if (result)
			return -ERESTARTSYS;
	}

	write_index = READ_ONCE(device->write_index);
	read_index = smp_load_acquire(&device->read_index);
	free_space = ring_free(read_index, write_index);
	bytes_to_write = min(count, free_space);

	temporary_buffer = memdup_user(user_buffer, bytes_to_write);
	if (IS_ERR(temporary_buffer))
		return PTR_ERR(temporary_buffer);

	ring_copy_in(device->buffer, write_index, temporary_buffer, bytes_to_write);
	kfree(temporary_buffer);

	new_write_index = (write_index + bytes_to_write) % LOCK_FREE_RING_SIZE;

	/* Publish new data after all buffer writes complete. */
	smp_store_release(&device->write_index, new_write_index);
	wake_up_interruptible(&device->read_wait);

	return bytes_to_write;
}

static __poll_t lock_free_poll(struct file *file, poll_table *wait)
{
	struct lock_free_device *device = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &device->read_wait, wait);
	poll_wait(file, &device->write_wait, wait);

	if (lock_free_data_available(device))
		mask |= EPOLLIN | EPOLLRDNORM;

	if (lock_free_space_available(device))
		mask |= EPOLLOUT | EPOLLWRNORM;

	return mask;
}

static const struct file_operations lock_free_fops = {
	.owner = THIS_MODULE,
	.open = lock_free_open,
	.release = lock_free_release,
	.read = lock_free_read,
	.write = lock_free_write,
	.poll = lock_free_poll,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	.llseek = no_llseek,
#endif
};

static struct miscdevice lock_free_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = LOCK_FREE_DEVICE_NAME,
	.fops = &lock_free_fops,
	.mode = 0666,
};

static int __init lock_free_init(void)
{
	int result;

	queue_device.buffer = kmalloc(LOCK_FREE_RING_SIZE, GFP_KERNEL);
	if (!queue_device.buffer)
		return -ENOMEM;

	init_waitqueue_head(&queue_device.read_wait);
	init_waitqueue_head(&queue_device.write_wait);
	atomic_set(&queue_device.reader_open, 0);
	atomic_set(&queue_device.writer_open, 0);

	result = misc_register(&lock_free_misc_device);
	if (result) {
		kfree(queue_device.buffer);
		return result;
	}

	pr_info("lock_free_char: loaded /dev/%s (SPSC)\n", LOCK_FREE_DEVICE_NAME);
	return 0;
}

static void __exit lock_free_exit(void)
{
	misc_deregister(&lock_free_misc_device);
	kfree(queue_device.buffer);
	pr_info("lock_free_char: unloaded\n");
}

module_init(lock_free_init);
module_exit(lock_free_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux character device project");
MODULE_DESCRIPTION("SPSC lock-free circular character device");
