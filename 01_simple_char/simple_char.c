// SPDX-License-Identifier: GPL-2.0
/* Basic character device using an explicit major/minor device number. */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "simple_char.h"

static dev_t simple_dev_number;
static struct cdev simple_cdev;
static struct class *simple_class;
static struct device *simple_device;

static char simple_buffer[SIMPLE_CHAR_CAPACITY];
static size_t simple_data_size;

static int simple_open(struct inode *inode, struct file *file)
{
	pr_debug("simple_char: open\n");
	return 0;
}

static int simple_release(struct inode *inode, struct file *file)
{
	pr_debug("simple_char: release\n");
	return 0;
}

static ssize_t simple_read(struct file *file, char __user *user_buffer,
			   size_t count, loff_t *offset)
{
	size_t bytes_to_read;

	if (*offset >= simple_data_size)
		return 0;

	bytes_to_read = min(count, simple_data_size - (size_t)*offset);

	if (copy_to_user(user_buffer, simple_buffer + *offset, bytes_to_read))
		return -EFAULT;

	*offset += bytes_to_read;
	return bytes_to_read;
}

static ssize_t simple_write(struct file *file, const char __user *user_buffer,
			    size_t count, loff_t *offset)
{
	size_t bytes_to_write;
	size_t end_offset;

	if (*offset >= SIMPLE_CHAR_CAPACITY)
		return -ENOSPC;

	if (*offset == 0)
		simple_data_size = 0;

	bytes_to_write = min(count, SIMPLE_CHAR_CAPACITY - (size_t)*offset);

	if (copy_from_user(simple_buffer + *offset, user_buffer, bytes_to_write))
		return -EFAULT;

	*offset += bytes_to_write;
	end_offset = (size_t)*offset;
	if (end_offset > simple_data_size)
		simple_data_size = end_offset;

	return bytes_to_write;
}

static const struct file_operations simple_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.release = simple_release,
	.read = simple_read,
	.write = simple_write,
};

static int __init simple_char_init(void)
{
	int result;

	result = alloc_chrdev_region(&simple_dev_number, SIMPLE_CHAR_MINOR,
				     SIMPLE_CHAR_COUNT, SIMPLE_CHAR_DEVICE_NAME);
	if (result)
		return result;

	cdev_init(&simple_cdev, &simple_fops);
	simple_cdev.owner = THIS_MODULE;

	result = cdev_add(&simple_cdev, simple_dev_number, SIMPLE_CHAR_COUNT);
	if (result)
		goto unregister_number;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	simple_class = class_create(SIMPLE_CHAR_CLASS_NAME);
#else
	simple_class = class_create(THIS_MODULE, SIMPLE_CHAR_CLASS_NAME);
#endif
	if (IS_ERR(simple_class)) {
		result = PTR_ERR(simple_class);
		goto delete_cdev;
	}

	simple_device = device_create(simple_class, NULL, simple_dev_number,
				      NULL, SIMPLE_CHAR_DEVICE_NAME);
	if (IS_ERR(simple_device)) {
		result = PTR_ERR(simple_device);
		goto destroy_class;
	}

	pr_info("simple_char: loaded major=%d minor=%d\n",
		MAJOR(simple_dev_number), MINOR(simple_dev_number));
	return 0;

destroy_class:
	class_destroy(simple_class);
delete_cdev:
	cdev_del(&simple_cdev);
unregister_number:
	unregister_chrdev_region(simple_dev_number, SIMPLE_CHAR_COUNT);
	return result;
}

static void __exit simple_char_exit(void)
{
	device_destroy(simple_class, simple_dev_number);
	class_destroy(simple_class);
	cdev_del(&simple_cdev);
	unregister_chrdev_region(simple_dev_number, SIMPLE_CHAR_COUNT);
	pr_info("simple_char: unloaded\n");
}

module_init(simple_char_init);
module_exit(simple_char_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux character device project");
MODULE_DESCRIPTION("Basic major/minor character device");
