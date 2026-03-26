/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"
#include "aesd_ioctl.h"

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/device.h>

int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Aaron Van"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;
static struct class *aesd_class;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */
    filp->private_data = container_of(inode->i_cdev, struct aesd_dev, cdev);
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */
    return 0;
}

static size_t aesd_total_size(struct aesd_dev *dev)
{
    size_t total = 0;
    uint8_t count;
    uint8_t index;

    count = dev->buffer.full ? AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED :
        ((dev->buffer.in_offs + AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED -
          dev->buffer.out_offs) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED);

    index = dev->buffer.out_offs;

    while (count--) {
        total += dev->buffer.entry[index].size;
        index = (index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    return total;
}

static uint8_t aesd_valid_entry_count(struct aesd_dev *dev)
{
    if (dev->buffer.full) {
        return AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    return (dev->buffer.in_offs + AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED -
            dev->buffer.out_offs) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
}

static long aesd_seekto_fpos(struct aesd_dev *dev, uint32_t write_cmd,
                             uint32_t write_cmd_offset)
{
    uint8_t count;
    uint8_t index;
    uint32_t cmd_index = 0;
    loff_t pos = 0;

    if (dev->buffer.full) {
        count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    } else {
        count = (dev->buffer.in_offs + AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED -
                 dev->buffer.out_offs) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    index = dev->buffer.out_offs;

    while (count--) {
        struct aesd_buffer_entry *entry = &dev->buffer.entry[index];

        if (cmd_index == write_cmd) {
            if (write_cmd_offset >= entry->size) {
                return -EINVAL;
            }
            return pos + write_cmd_offset;
        }

        pos += entry->size;
        cmd_index++;
        index = (index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    return -EINVAL;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    size_t entry_offset;
    struct aesd_buffer_entry *entry;

    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    while (count > 0) {
        entry = aesd_circular_buffer_find_entry_offset_for_fpos(
            &dev->buffer, *f_pos, &entry_offset);

        if (!entry) {
            break;
        }

        {
            size_t bytes_available = entry->size - entry_offset;
            size_t bytes_to_copy = min(count, bytes_available);

            if (copy_to_user(buf + retval, entry->buffptr + entry_offset, bytes_to_copy)) {
                retval = -EFAULT;
                break;
            }

            *f_pos += bytes_to_copy;
            retval += bytes_to_copy;
            count -= bytes_to_copy;
        }
    }

    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    char *newbuf;
    struct aesd_buffer_entry new_entry;
    const char *oldptr;
    char *newline_pos;

    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    newbuf = krealloc((void *)dev->pending_write.buffptr,
                      dev->pending_write.size + count,
                      GFP_KERNEL);
    if (!newbuf) {
        goto out;
    }

    dev->pending_write.buffptr = newbuf;

    if (copy_from_user((char *)dev->pending_write.buffptr + dev->pending_write.size, buf, count)) {
        retval = -EFAULT;
        goto out;
    }

    dev->pending_write.size += count;
    retval = count;

    newline_pos = memchr(dev->pending_write.buffptr, '\n', dev->pending_write.size);
    if (newline_pos) {
        new_entry.buffptr = dev->pending_write.buffptr;
        new_entry.size = dev->pending_write.size;

        oldptr = aesd_circular_buffer_add_entry(&dev->buffer, &new_entry);
        if (oldptr) {
            kfree(oldptr);
        }

        dev->pending_write.buffptr = NULL;
        dev->pending_write.size = 0;
    }

out:
    mutex_unlock(&dev->lock);
    return retval;
}

loff_t aesd_llseek(struct file *filp, loff_t off, int whence)
{
    loff_t retval;
    struct aesd_dev *dev = filp->private_data;
    size_t size;

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    size = aesd_total_size(dev);
    retval = fixed_size_llseek(filp, off, whence, size);

    mutex_unlock(&dev->lock);
    return retval;
}

static long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    long newpos;

    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC) {
        return -ENOTTY;
    }

    if (_IOC_NR(cmd) > AESDCHAR_IOC_MAXNR) {
        return -ENOTTY;
    }

    switch (cmd) {
    case AESDCHAR_IOCSEEKTO:
        if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto))) {
            return -EFAULT;
        }

        if (mutex_lock_interruptible(&dev->lock)) {
            return -ERESTARTSYS;
        }

        newpos = aesd_seekto_fpos(dev, seekto.write_cmd, seekto.write_cmd_offset);
        if (newpos < 0) {
            mutex_unlock(&dev->lock);
            return newpos;
        }

        filp->f_pos = newpos;
        mutex_unlock(&dev->lock);
        return 0;

    default:
        return -ENOTTY;
    }
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
    .llseek =   aesd_llseek,
    .unlocked_ioctl = aesd_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */

    aesd_circular_buffer_init(&aesd_device.buffer);
    mutex_init(&aesd_device.lock);
    aesd_device.pending_write.buffptr = NULL;
    aesd_device.pending_write.size = 0;

    result = aesd_setup_cdev(&aesd_device);

    if (result) {
        unregister_chrdev_region(dev, 1);
        return result;
    }

    aesd_class = class_create("aesdchar_class");
    if (IS_ERR(aesd_class)) {
        cdev_del(&aesd_device.cdev);
        unregister_chrdev_region(dev, 1);
        return PTR_ERR(aesd_class);
    }

    if (IS_ERR(device_create(aesd_class, NULL, dev, NULL, "aesdchar"))) {
        class_destroy(aesd_class);
        cdev_del(&aesd_device.cdev);
        unregister_chrdev_region(dev, 1);
        return -1;
    }

    return 0;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    device_destroy(aesd_class, devno);
    class_destroy(aesd_class);
    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */
    uint8_t count;
    uint8_t index;

    if (aesd_device.pending_write.buffptr) {
        kfree(aesd_device.pending_write.buffptr);
        aesd_device.pending_write.buffptr = NULL;
        aesd_device.pending_write.size = 0;
    }

    count = aesd_device.buffer.full ? AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED :
         ((aesd_device.buffer.in_offs + AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED -
         aesd_device.buffer.out_offs) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED);

    index = aesd_device.buffer.out_offs;

    while (count--) {
        kfree((void *)aesd_device.buffer.entry[index].buffptr);
        aesd_device.buffer.entry[index].buffptr = NULL;
        aesd_device.buffer.entry[index].size = 0;
        index = (index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
