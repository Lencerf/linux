// SPDX-License-Identifier: GPL-2.0
/*
 * fdbox.c - framework to preserve file descriptors across
 *           process lifetime and kexec
 *
 * Copyright (C) 2024-2025 Amazon.com Inc. or its affiliates.
 *
 * Author: Pratyush Yadav <ptyadav@amazon.de>
 * Author: Alexander Graf <graf@amazon.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/anon_inodes.h>
#include <linux/cdev.h>
#include <linux/miscdevice.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/kexec.h>
#include <linux/kexec_handover.h>
#include <linux/libfdt.h>
#include <linux/fdbox.h>

static struct miscdevice fdbox_dev;

static struct {
	struct class			*class;
	dev_t				box_devt;
	struct xarray			box_list;
	struct xarray			handlers;
	struct rw_semaphore		recover_sem;
	bool				recover_done;
} priv = {
	.box_list = XARRAY_INIT(fdbox.box_list, XA_FLAGS_ALLOC),
	.handlers = XARRAY_INIT(fdbox.handlers, XA_FLAGS_ALLOC),
	.recover_sem = __RWSEM_INITIALIZER(priv.recover_sem),
};

struct fdbox_handler {
	const char *compatible;
	struct file *(*fn)(const void *fdt, int offset);
};

static struct fdbox *fdbox_remove_box(char *name)
{
	struct xarray *boxlist = &priv.box_list;
	unsigned long box_idx;
	struct fdbox *box;

	xa_lock(boxlist);
	xa_for_each(boxlist, box_idx, box) {
		if (!strcmp(box->name, name)) {
			__xa_erase(boxlist, box_idx);
			break;
		}
	}
	xa_unlock(boxlist);

	return box;
}

static struct fdbox_fd *fdbox_remove_fd(struct fdbox *box, char *name)
{
	struct xarray *fdlist = &box->fd_list;
	struct fdbox_fd *box_fd;
	unsigned long idx;

	xa_lock(fdlist);
	xa_for_each(fdlist, idx, box_fd) {
		if (!strncmp(box_fd->name, name, sizeof(box_fd->name))) {
			__xa_erase(fdlist, idx);
			break;
		}
	}
	xa_unlock(fdlist);

	return box_fd;
}

/* Must be called with box->rwsem held. */
static struct fdbox_fd *fdbox_put_file(struct fdbox *box, const char *name,
				       struct file *file)
{
	struct fdbox_fd *box_fd __free(kfree) = NULL, *cmp;
	struct xarray *fdlist = &box->fd_list;
	unsigned long idx;
	u32 newid;
	int ret;

	/* Only files that set f_fdbox_op are allowed in the box. */
	if (!file->f_fdbox_op)
		return ERR_PTR(-EOPNOTSUPP);

	box_fd = kzalloc(sizeof(*box_fd), GFP_KERNEL);
	if (!box_fd)
		return ERR_PTR(-ENOMEM);

	if (strscpy_pad(box_fd->name, name, sizeof(box_fd->name)) < 0)
		/* Name got truncated. This means the name is not NUL-terminated. */
		return ERR_PTR(-EINVAL);

	box_fd->file = file;
	box_fd->box = box;

	xa_lock(fdlist);
	xa_for_each(fdlist, idx, cmp) {
		/* Look for name collisions. */
		if (!strcmp(box_fd->name, cmp->name)) {
			xa_unlock(fdlist);
			return ERR_PTR(-EEXIST);
		}
	}

	ret = __xa_alloc(fdlist, &newid, box_fd, xa_limit_32b, GFP_KERNEL);
	xa_unlock(fdlist);
	if (ret)
		return ERR_PTR(ret);

	return_ptr(box_fd);
}

static long fdbox_put_fd(struct fdbox *box, unsigned long arg)
{
	struct fdbox_put_fd put_fd;
	struct fdbox_fd *box_fd;
	struct file *file;
	int ret;

	if (copy_from_user(&put_fd, (void __user *)arg, sizeof(put_fd)))
		return -EFAULT;

	guard(rwsem_read)(&box->rwsem);

	if (box->sealed)
		return -EBUSY;

	file = fget_raw(put_fd.fd);
	if (!file)
		return -EINVAL;

	box_fd = fdbox_put_file(box, put_fd.name, file);
	if (IS_ERR(box_fd)) {
		fput(file);
		return PTR_ERR(box_fd);
	}

	ret = close_fd(put_fd.fd);
	if (ret) {
		struct fdbox_fd *del;

		del = fdbox_remove_fd(box, put_fd.name);
		/*
		 * If we fail to remove from list, it means someone else took
		 * the FD out. In that case, they own the refcount of the file
		 * now.
		 */
		if (del == box_fd)
			fput(file);

		return ret;
	}

	return 0;
}

static long fdbox_seal(struct fdbox *box)
{
	struct fdbox_fd *box_fd;
	unsigned long idx;
	int ret;

	guard(rwsem_write)(&box->rwsem);

	if (box->sealed)
		return -EBUSY;

	xa_for_each(&box->fd_list, idx, box_fd) {
		const struct fdbox_file_ops *fdbox_ops = box_fd->file->f_fdbox_op;

		if (fdbox_ops && fdbox_ops->seal) {
			ret = fdbox_ops->seal(box);
			if (ret)
				return ret;
		}
	}

	box->sealed = true;

	return 0;
}

static long fdbox_unseal(struct fdbox *box)
{
	struct fdbox_fd *box_fd;
	unsigned long idx;
	int ret;

	guard(rwsem_write)(&box->rwsem);

	if (!box->sealed)
		return -EBUSY;

	xa_for_each(&box->fd_list, idx, box_fd) {
		const struct fdbox_file_ops *fdbox_ops = box_fd->file->f_fdbox_op;

		if (fdbox_ops && fdbox_ops->seal) {
			ret = fdbox_ops->seal(box);
			if (ret)
				return ret;
		}
	}

	box->sealed = false;

	return 0;
}

static long fdbox_get_fd(struct fdbox *box, unsigned long arg)
{
	struct fdbox_get_fd get_fd;
	struct fdbox_fd *box_fd;
	int fd;

	guard(rwsem_read)(&box->rwsem);

	if (box->sealed)
		return -EBUSY;

	if (copy_from_user(&get_fd, (void __user *)arg, sizeof(get_fd)))
		return -EFAULT;

	if (get_fd.flags)
		return -EINVAL;

	fd = get_unused_fd_flags(0);
	if (fd < 0)
		return fd;

	box_fd = fdbox_remove_fd(box, get_fd.name);
	if (!box_fd) {
		put_unused_fd(fd);
		return -ENOENT;
	}

	fd_install(fd, box_fd->file);
	kfree(box_fd);
	return fd;
}

static long box_fops_unl_ioctl(struct file *filep,
			       unsigned int cmd, unsigned long arg)
{
	struct fdbox *box = filep->private_data;
	long ret = -EINVAL;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	switch (cmd) {
	case FDBOX_PUT_FD:
		ret = fdbox_put_fd(box, arg);
		break;
	case FDBOX_UNSEAL:
		ret = fdbox_unseal(box);
		break;
	case FDBOX_SEAL:
		ret = fdbox_seal(box);
		break;
	case FDBOX_GET_FD:
		ret = fdbox_get_fd(box, arg);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int box_fops_open(struct inode *inode, struct file *filep)
{
	struct fdbox *box = container_of(inode->i_cdev, struct fdbox, cdev);

	filep->private_data = box;

	return 0;
}

static const struct file_operations box_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= box_fops_unl_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.open		= box_fops_open,
};

static void fdbox_device_release(struct device *dev)
{
	struct fdbox *box = container_of(dev, struct fdbox, dev);
	struct xarray *fdlist = &box->fd_list;
	struct fdbox_fd *box_fd;
	unsigned long idx;

	unregister_chrdev_region(box->dev.devt, 1);

	xa_for_each(fdlist, idx, box_fd) {
		xa_erase(fdlist, idx);
		fput(box_fd->file);
		kfree(box_fd);
	}

	xa_destroy(fdlist);
	kfree(box);
}

static struct fdbox *_fdbox_create_box(const char *name)
{
	struct fdbox *box;
	int ret = 0;
	u32 id;

	box = kzalloc(sizeof(*box), GFP_KERNEL);
	if (!box)
		return ERR_PTR(-ENOMEM);

	xa_init_flags(&box->fd_list, XA_FLAGS_ALLOC);
	xa_init_flags(&box->pending_fds, XA_FLAGS_ALLOC);
	init_rwsem(&box->rwsem);

	if (strscpy_pad(box->name, name, sizeof(box->name)) < 0) {
		/* Name got truncated. This means the name is not NUL-terminated. */
		kfree(box);
		return ERR_PTR(-EINVAL);
	}

	dev_set_name(&box->dev, "fdbox/%s", name);

	ret = alloc_chrdev_region(&box->dev.devt, 0, 1, name);
	if (ret) {
		kfree(box);
		return ERR_PTR(ret);
	}

	box->dev.release = fdbox_device_release;
	device_initialize(&box->dev);

	cdev_init(&box->cdev, &box_fops);
	box->cdev.owner = THIS_MODULE;
	kobject_set_name(&box->cdev.kobj, "fdbox/%s", name);

	ret = cdev_device_add(&box->cdev, &box->dev);
	if (ret)
		goto err_dev;

	ret = xa_alloc(&priv.box_list, &id, box, xa_limit_32b, GFP_KERNEL);
	if (ret)
		goto err_cdev;

	return box;

err_cdev:
	cdev_device_del(&box->cdev, &box->dev);
err_dev:
	/*
	 * This should free the box and chrdev region via
	 * fdbox_device_release().
	 */
	put_device(&box->dev);

	return ERR_PTR(ret);
}

static long fdbox_create_box(unsigned long arg)
{
	struct fdbox_create_box create_box;

	if (copy_from_user(&create_box, (void __user *)arg, sizeof(create_box)))
		return -EFAULT;

	if (create_box.flags)
		return -EINVAL;

	return PTR_ERR_OR_ZERO(_fdbox_create_box(create_box.name));
}

static void _fdbox_delete_box(struct fdbox *box)
{
	cdev_device_del(&box->cdev, &box->dev);
	unregister_chrdev_region(box->dev.devt, 1);
	put_device(&box->dev);
}

static long fdbox_delete_box(unsigned long arg)
{
	struct fdbox_delete_box delete_box;
	struct fdbox *box;

	if (copy_from_user(&delete_box, (void __user *)arg, sizeof(delete_box)))
		return -EFAULT;

	if (delete_box.flags)
		return -EINVAL;

	box = fdbox_remove_box(delete_box.name);
	if (!box)
		return -ENOENT;

	_fdbox_delete_box(box);
	return 0;
}

static long fdbox_fops_unl_ioctl(struct file *filep,
				 unsigned int cmd, unsigned long arg)
{
	long ret = -EINVAL;

	switch (cmd) {
	case FDBOX_CREATE_BOX:
		ret = fdbox_create_box(arg);
		break;
	case FDBOX_DELETE_BOX:
		ret = fdbox_delete_box(arg);
		break;
	}

	return ret;
}

static const struct file_operations fdbox_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= fdbox_fops_unl_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static struct miscdevice fdbox_dev = {
	.minor = FDBOX_MINOR,
	.name = "fdbox",
	.fops = &fdbox_fops,
	.nodename = "fdbox/fdbox",
	.mode = 0600,
};

static char *fdbox_devnode(const struct device *dev, umode_t *mode)
{
	char *ret = kasprintf(GFP_KERNEL, "fdbox/%s", dev_name(dev));
	return ret;
}

static int fdbox_kho_write_fds(void *fdt, struct fdbox *box)
{
	struct fdbox_fd *box_fd;
	struct file *file;
	unsigned long idx;
	int err = 0;

	xa_for_each(&box->fd_list, idx, box_fd) {
		file = box_fd->file;

		if (!file->f_fdbox_op->kho_write) {
			pr_info("box '%s' FD '%s' has no KHO method. It won't be saved across kexec\n",
				box->name, box_fd->name);
			continue;
		}

		err = fdt_begin_node(fdt, box_fd->name);
		if (err) {
			pr_err("failed to begin node for box '%s' FD '%s'\n",
			       box->name, box_fd->name);
			return err;
		}

		inode_lock(file_inode(file));
		err = file->f_fdbox_op->kho_write(box_fd, fdt);
		inode_unlock(file_inode(file));
		if (err) {
			pr_err("kho_write failed for box '%s' FD '%s': %d\n",
			       box->name, box_fd->name, err);
			return err;
		}

		err = fdt_end_node(fdt);
		if (err) {
			/* TODO: This leaks all pages reserved by kho_write(). */
			pr_err("failed to end node for box '%s' FD '%s'\n",
			       box->name, box_fd->name);
			return err;
		}
	}

	return err;
}

static int fdbox_kho_write_boxes(void *fdt)
{
	static const char compatible[] = "fdbox,box-v1";
	struct fdbox *box;
	unsigned long idx;
	int err = 0;

	xa_for_each(&priv.box_list, idx, box) {
		if (!box->sealed)
			continue;

		err |= fdt_begin_node(fdt, box->name);
		err |= fdt_property(fdt, "compatible", compatible, sizeof(compatible));
		err |= fdbox_kho_write_fds(fdt, box);
		err |= fdt_end_node(fdt);
	}

	return err;
}

static int fdbox_kho_notifier(struct notifier_block *self,
			      unsigned long cmd,
			      void *v)
{
	static const char compatible[] = "fdbox-v1";
	void *fdt = v;
	int err = 0;

	switch (cmd) {
	case KEXEC_KHO_ABORT:
		return NOTIFY_DONE;
	case KEXEC_KHO_DUMP:
		/* Handled below */
		break;
	default:
		return NOTIFY_BAD;
	}

	err |= fdt_begin_node(fdt, "fdbox");
	err |= fdt_property(fdt, "compatible", compatible, sizeof(compatible));
	err |= fdbox_kho_write_boxes(fdt);
	err |= fdt_end_node(fdt);

	return err ? NOTIFY_BAD : NOTIFY_DONE;
}

static struct notifier_block fdbox_kho_nb = {
	.notifier_call = fdbox_kho_notifier,
};

static void fdbox_recover_fd(const void *fdt, int offset, struct fdbox *box,
			     struct file *(*fn)(const void *fdt, int offset))
{
	struct fdbox_fd *box_fd;
	struct file *file;
	const char *name;

	name = fdt_get_name(fdt, offset, NULL);
	if (!name) {
		pr_err("no name in FDT for FD at offset %d\n", offset);
		return;
	}

	file = fn(fdt, offset);
	if (!file)
		return;

	scoped_guard(rwsem_read, &box->rwsem) {
		box_fd = fdbox_put_file(box, name, file);
		if (IS_ERR(box_fd)) {
			pr_err("failed to put fd '%s' into box '%s': %ld\n",
			       box->name, name, PTR_ERR(box_fd));
			fput(file);
			return;
		}
	}
}

static void fdbox_kho_recover(void)
{
	const void *fdt = kho_get_fdt();
	const char *path = "/fdbox";
	int off, box, fd;
	int err;

	/* Not a KHO boot */
	if (!fdt)
		return;

	/*
	 * When adding handlers this is taken as read. Taking it as write here
	 * ensures no handlers get added while nodes are being processed,
	 * eliminating the race of a handler getting added after its node is
	 * processed, but before the whole recover is done.
	 */
	guard(rwsem_write)(&priv.recover_sem);

	off = fdt_path_offset(fdt, path);
	if (off < 0) {
		pr_debug("could not find '%s' in DT", path);
		return;
	}

	err = fdt_node_check_compatible(fdt, off, "fdbox-v1");
	if (err) {
		pr_err("invalid top level compatible\n");
		return;
	}

	fdt_for_each_subnode(box, fdt, off) {
		struct fdbox *new_box;

		err = fdt_node_check_compatible(fdt, box, "fdbox,box-v1");
		if (err) {
			pr_err("invalid compatible for box '%s'\n",
			       fdt_get_name(fdt, box, NULL));
			continue;
		}

		new_box = _fdbox_create_box(fdt_get_name(fdt, box, NULL));
		if (IS_ERR(new_box)) {
			pr_warn("could not create box '%s'\n",
				fdt_get_name(fdt, box, NULL));
			continue;
		}

		fdt_for_each_subnode(fd, fdt, box) {
			struct fdbox_handler *handler;
			const char *compatible;
			unsigned long idx;

			compatible = fdt_getprop(fdt, fd, "compatible", NULL);
			if (!compatible) {
				pr_warn("failed to get compatible for FD '%s'. Skipping.\n",
					fdt_get_name(fdt, fd, NULL));
				continue;
			}

			xa_for_each(&priv.handlers, idx, handler) {
				if (!strcmp(handler->compatible, compatible))
					break;
			}

			if (handler) {
				fdbox_recover_fd(fdt, fd, new_box, handler->fn);
			} else {
				u32 id;

				pr_debug("found no handler for compatible %s. Queueing for later.\n",
					 compatible);

				if (xa_alloc(&new_box->pending_fds, &id,
					     xa_mk_value(fd), xa_limit_32b,
					     GFP_KERNEL)) {
					pr_warn("failed to queue pending FD '%s' to list\n",
						fdt_get_name(fdt, fd, NULL));
				}
			}
		}

		new_box->sealed = true;
	}

	priv.recover_done = true;
}

static void fdbox_recover_pending(struct fdbox_handler *handler)
{
	const void *fdt = kho_get_fdt();
	unsigned long bid, pid;
	struct fdbox *box;
	void *pending;

	if (WARN_ON(!fdt))
		return;

	xa_for_each(&priv.box_list, bid, box) {
		xa_for_each(&box->pending_fds, pid, pending) {
			int off = xa_to_value(pending);

			if (fdt_node_check_compatible(fdt, off, handler->compatible) == 0) {
				fdbox_recover_fd(fdt, off, box, handler->fn);
				xa_erase(&box->pending_fds, pid);
			}
		}
	}
}

int fdbox_register_handler(const char *compatible,
			   struct file *(*fn)(const void *fdt, int offset))
{
	struct xarray *handlers = &priv.handlers;
	struct fdbox_handler *handler, *cmp;
	unsigned long idx;
	int ret;
	u32 id;

	/* See comment in fdbox_kho_recover(). */
	guard(rwsem_read)(&priv.recover_sem);

	handler = kmalloc(sizeof(*handler), GFP_KERNEL);
	if (!handler)
		return -ENOMEM;

	handler->compatible = compatible;
	handler->fn = fn;

	xa_lock(handlers);
	xa_for_each(handlers, idx, cmp) {
		if (!strcmp(cmp->compatible, compatible)) {
			xa_unlock(handlers);
			kfree(handler);
			return -EEXIST;
		}
	}

	ret = __xa_alloc(handlers, &id, handler, xa_limit_32b, GFP_KERNEL);
	xa_unlock(handlers);
	if (ret) {
		kfree(handler);
		return ret;
	}

	if (priv.recover_done)
		fdbox_recover_pending(handler);

	return 0;
}

static int __init fdbox_init(void)
{
	int ret = 0;

	/* /dev/fdbox/$NAME */
	priv.class = class_create("fdbox");
	if (IS_ERR(priv.class))
		return PTR_ERR(priv.class);

	priv.class->devnode = fdbox_devnode;

	ret = alloc_chrdev_region(&priv.box_devt, 0, 1, "fdbox");
	if (ret)
		goto err_class;

	ret = misc_register(&fdbox_dev);
	if (ret) {
		pr_err("fdbox: misc device register failed\n");
		goto err_chrdev;
	}

	if (IS_ENABLED(CONFIG_KEXEC_HANDOVER)) {
		register_kho_notifier(&fdbox_kho_nb);
		fdbox_kho_recover();
	}

	return 0;

err_chrdev:
	unregister_chrdev_region(priv.box_devt, 1);
	priv.box_devt = 0;
err_class:
	class_destroy(priv.class);
	priv.class = NULL;
	return ret;
}
module_init(fdbox_init);
