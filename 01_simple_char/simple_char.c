// SPDX-License-Identifier: GPL-2.0
/* Minimal unsynchronized character driver used to learn Linux char-device plumbing. */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include "simple_char.h"

struct simple_char_device {
	dev_t device_number;
	struct cdev cdev;
	struct class *device_class;
	char buffer[SIMPLE_CHAR_CAPACITY];
	size_t data_size;
};

static struct simple_char_device simple_device;

static int simple_char_open(struct inode *inode, struct file *file)
{
	file->private_data = &simple_device;
	pr_debug("simple_char: open\n");
	return 0;
}

static int simple_char_release(struct inode *inode, struct file *file)
{
	pr_debug("simple_char: release\n");
	return 0;
}

static ssize_t simple_char_read(struct file *file, char __user *user_buffer,
				size_t requested, loff_t *offset)
{
	struct simple_char_device *device = file->private_data;
	size_t available;
	size_t bytes_to_copy;

	if (requested == 0)
		return 0;

	if (*offset >= device->data_size)
		return 0;

	available = device->data_size - (size_t)*offset;
	bytes_to_copy = min(requested, available);

	if (copy_to_user(user_buffer, device->buffer + *offset, bytes_to_copy))
		return -EFAULT;

	*offset += bytes_to_copy;
	return bytes_to_copy;
}

static ssize_t simple_char_write(struct file *file,
				 const char __user *user_buffer,
				 size_t requested, loff_t *offset)
{
	struct simple_char_device *device = file->private_data;
	size_t available;
	size_t bytes_to_copy;

	if (requested == 0)
		return 0;

	if (*offset >= SIMPLE_CHAR_CAPACITY)
		return -ENOSPC;

	available = SIMPLE_CHAR_CAPACITY - (size_t)*offset;
	bytes_to_copy = min(requested, available);

	if (copy_from_user(device->buffer + *offset, user_buffer, bytes_to_copy))
		return -EFAULT;

	*offset += bytes_to_copy;
	if ((size_t)*offset > device->data_size)
		device->data_size = (size_t)*offset;

	return bytes_to_copy;
}

static const struct file_operations simple_char_operations = {
	.owner = THIS_MODULE,
	.open = simple_char_open,
	.release = simple_char_release,
	.read = simple_char_read,
	.write = simple_char_write,
};

static int __init simple_char_init(void)
{
	int result;

	result = alloc_chrdev_region(&simple_device.device_number, 0, 1,
				     SIMPLE_CHAR_DEVICE_NAME);
	if (result)
		return result;

	cdev_init(&simple_device.cdev, &simple_char_operations);
	simple_device.cdev.owner = THIS_MODULE;

	result = cdev_add(&simple_device.cdev, simple_device.device_number, 1);
	if (result)
		goto unregister_number;

	simple_device.device_class = class_create(SIMPLE_CHAR_CLASS_NAME);
	if (IS_ERR(simple_device.device_class)) {
		result = PTR_ERR(simple_device.device_class);
		goto delete_cdev;
	}

	if (IS_ERR(device_create(simple_device.device_class, NULL,
				 simple_device.device_number, NULL,
				 SIMPLE_CHAR_DEVICE_NAME))) {
		result = -ENODEV;
		goto destroy_class;
	}

	pr_info("simple_char: loaded major=%u minor=%u\n",
		MAJOR(simple_device.device_number), MINOR(simple_device.device_number));
	return 0;

destroy_class:
	class_destroy(simple_device.device_class);
delete_cdev:
	cdev_del(&simple_device.cdev);
unregister_number:
	unregister_chrdev_region(simple_device.device_number, 1);
	return result;
}

static void __exit simple_char_exit(void)
{
	device_destroy(simple_device.device_class, simple_device.device_number);
	class_destroy(simple_device.device_class);
	cdev_del(&simple_device.cdev);
	unregister_chrdev_region(simple_device.device_number, 1);
	pr_info("simple_char: unloaded\n");
}

module_init(simple_char_init);
module_exit(simple_char_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux Character Driver Lab");
MODULE_DESCRIPTION("Minimal unsynchronized Linux character device");
