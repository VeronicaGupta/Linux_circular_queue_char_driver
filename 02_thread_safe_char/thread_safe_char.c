// SPDX-License-Identifier: GPL-2.0
/* Thread-safe bounded byte FIFO exposed through a miscellaneous device. */

#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "thread_safe_char.h"

struct thread_safe_device {
	char *buffer;
	size_t capacity;
	size_t read_index;
	size_t write_index;
	size_t data_count;
	struct mutex lock;
	wait_queue_head_t read_wait;
	wait_queue_head_t write_wait;
};

static struct thread_safe_device queue_device;

static size_t thread_safe_copy_out(struct thread_safe_device *device,
				   char *destination, size_t count)
{
	size_t first_part;

	first_part = min(count, device->capacity - device->read_index);
	memcpy(destination, device->buffer + device->read_index, first_part);

	if (count > first_part)
		memcpy(destination + first_part, device->buffer, count - first_part);

	return count;
}

static size_t thread_safe_copy_in(struct thread_safe_device *device,
				  const char *source, size_t count)
{
	size_t first_part;

	first_part = min(count, device->capacity - device->write_index);
	memcpy(device->buffer + device->write_index, source, first_part);

	if (count > first_part)
		memcpy(device->buffer, source + first_part, count - first_part);

	return count;
}

static ssize_t thread_safe_read(struct file *file, char __user *user_buffer,
				size_t count, loff_t *offset)
{
	struct thread_safe_device *device = file->private_data;
	char *temporary_buffer;
	size_t bytes_to_read;
	int result;

	if (count == 0)
		return 0;

	result = mutex_lock_interruptible(&device->lock);
	if (result)
		return -ERESTARTSYS;

	while (device->data_count == 0) {
		mutex_unlock(&device->lock);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		result = wait_event_interruptible(device->read_wait,
						  READ_ONCE(device->data_count) > 0);
		if (result)
			return -ERESTARTSYS;

		result = mutex_lock_interruptible(&device->lock);
		if (result)
			return -ERESTARTSYS;
	}

	bytes_to_read = min(count, device->data_count);
	temporary_buffer = kmalloc(bytes_to_read, GFP_KERNEL);
	if (!temporary_buffer) {
		mutex_unlock(&device->lock);
		return -ENOMEM;
	}

	thread_safe_copy_out(device, temporary_buffer, bytes_to_read);

	if (copy_to_user(user_buffer, temporary_buffer, bytes_to_read)) {
		kfree(temporary_buffer);
		mutex_unlock(&device->lock);
		return -EFAULT;
	}

	device->read_index = (device->read_index + bytes_to_read) % device->capacity;
	device->data_count -= bytes_to_read;

	kfree(temporary_buffer);
	mutex_unlock(&device->lock);
	wake_up_interruptible(&device->write_wait);

	return bytes_to_read;
}

static ssize_t thread_safe_write(struct file *file,
				 const char __user *user_buffer,
				 size_t count, loff_t *offset)
{
	struct thread_safe_device *device = file->private_data;
	char *temporary_buffer;
	size_t free_space;
	size_t bytes_to_write;
	int result;

	if (count == 0)
		return 0;

	result = mutex_lock_interruptible(&device->lock);
	if (result)
		return -ERESTARTSYS;

	while (device->data_count == device->capacity) {
		mutex_unlock(&device->lock);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		result = wait_event_interruptible(device->write_wait,
						  READ_ONCE(device->data_count) < device->capacity);
		if (result)
			return -ERESTARTSYS;

		result = mutex_lock_interruptible(&device->lock);
		if (result)
			return -ERESTARTSYS;
	}

	free_space = device->capacity - device->data_count;
	bytes_to_write = min(count, free_space);

	temporary_buffer = memdup_user(user_buffer, bytes_to_write);
	if (IS_ERR(temporary_buffer)) {
		result = PTR_ERR(temporary_buffer);
		mutex_unlock(&device->lock);
		return result;
	}

	thread_safe_copy_in(device, temporary_buffer, bytes_to_write);
	device->write_index = (device->write_index + bytes_to_write) % device->capacity;
	device->data_count += bytes_to_write;

	kfree(temporary_buffer);
	mutex_unlock(&device->lock);
	wake_up_interruptible(&device->read_wait);

	return bytes_to_write;
}

static int thread_safe_open(struct inode *inode, struct file *file)
{
	file->private_data = &queue_device;
	return 0;
}

static const struct file_operations thread_safe_fops = {
	.owner = THIS_MODULE,
	.open = thread_safe_open,
	.read = thread_safe_read,
	.write = thread_safe_write,
	.llseek = no_llseek,
};

static struct miscdevice thread_safe_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = THREAD_SAFE_DEVICE_NAME,
	.fops = &thread_safe_fops,
	.mode = 0666,
};

static int __init thread_safe_init(void)
{
	int result;

	queue_device.capacity = THREAD_SAFE_CAPACITY;
	queue_device.buffer = kmalloc(queue_device.capacity, GFP_KERNEL);
	if (!queue_device.buffer)
		return -ENOMEM;

	mutex_init(&queue_device.lock);
	init_waitqueue_head(&queue_device.read_wait);
	init_waitqueue_head(&queue_device.write_wait);

	result = misc_register(&thread_safe_misc_device);
	if (result) {
		kfree(queue_device.buffer);
		return result;
	}

	pr_info("thread_safe_char: loaded /dev/%s\n", THREAD_SAFE_DEVICE_NAME);
	return 0;
}

static void __exit thread_safe_exit(void)
{
	misc_deregister(&thread_safe_misc_device);
	kfree(queue_device.buffer);
	pr_info("thread_safe_char: unloaded\n");
}

module_init(thread_safe_init);
module_exit(thread_safe_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux character device project");
MODULE_DESCRIPTION("Thread-safe circular character device using mutex and wait queues");
