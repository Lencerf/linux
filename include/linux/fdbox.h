/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024-2025 Amazon.com Inc. or its affiliates.
 *
 * Author: Pratyush Yadav <ptyadav@amazon.de>
 * Author: Alexander Graf <graf@amazon.com>
 */
#ifndef _LINUX_FDBOX_H
#define _LINUX_FDBOX_H

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <uapi/linux/fdbox.h>

/**
 * struct fdbox - A box of FDs.
 * @name: Name of the box. Must be unique.
 * @rwsem: Used to ensure exclusive access to the box during SEAL/UNSEAL
 *         operations.
 * @dev: Backing device for the character device.
 * @cdev: Character device which accepts ioctls from userspace.
 * @fd_list: List of FDs in the box.
 * @sealed: Whether the box is sealed or not.
 */
struct fdbox {
	char				name[FDBOX_NAME_LEN];
	/*
	 * Taken as read when non-exclusive access is needed and the box can be
	 * in mutable state. For example, the GET_FD and PUT_FD operations use
	 * it as read when adding or removing FDs from the box.
	 *
	 * Taken as write when exclusive access is needed and the box should be
	 * in a stable, non-mutable state. For example, the SEAL and UNSEAL
	 * operations use it as write because they need the list of FDs to be
	 * stable.
	 */
	struct rw_semaphore		rwsem;
	struct device			dev;
	struct cdev			cdev;
	struct xarray			fd_list;
	struct xarray			pending_fds;
	bool				sealed;
};

/**
 * struct fdbox_fd - An FD in a box.
 * @name: Name of the FD. Must be unique in the box.
 * @file: Underlying file for the FD.
 * @flags: Box flags. Currently, no flags are allowed.
 * @box: The box to which this FD belongs.
 */
struct fdbox_fd {
	char				name[FDBOX_NAME_LEN];
	struct file			*file;
	int				flags;
	struct fdbox			*box;
};

/**
 * struct fdbox_file_ops - operations for files that can be put into a fdbox.
 */
struct fdbox_file_ops {
	/**
	 * @kho_write: write fd to KHO FDT.
	 *
	 * box_fd: Box FD to be serialized.
	 *
	 * fdt: KHO FDT
	 *
	 * This is called during KHO activation phase to serialize all data
	 * needed for a FD to be preserved across a KHO.
	 *
	 * Returns: 0 on success, -errno on failure. Error here causes KHO
	 * activation failure.
	 */
	int (*kho_write)(struct fdbox_fd *box_fd, void *fdt);
	/**
	 * @seal: seal the box
	 *
	 * box: Box which is going to be sealed.
	 *
	 * This can be set if a file has a dependency on other files. At seal
	 * time, all the FDs in the box can be inspected to ensure all the
	 * dependencies are met.
	 */
	int (*seal)(struct fdbox *box);
	/**
	 * @unseal: unseal the box
	 *
	 * box: Box which is going to be sealed.
	 *
	 * The opposite of seal. This can be set if a file has a dependency on
	 * other files. At unseal time, all the FDs in the box can be inspected
	 * to ensure all the dependencies are met. This can help ensure all
	 * necessary FDs made it through after a KHO for example.
	 */
	int (*unseal)(struct fdbox *box);
};

/**
 * fdbox_register_handler - register a handler for recovering Box FDs after KHO.
 * @compatible: compatible string in the KHO FDT node.
 * @handler: function to parse the FDT at offset 'offset'.
 *
 * After KHO, the FDs in the KHO FDT must be deserialized by the underlying
 * modules or file systems. Since module initialization can be in any order,
 * including after FDBox has been initialized, handler registration allows
 * modules to queue their parsing functions, and FDBox will execute them when it
 * can.
 *
 * Returns: 0 on success, -errno otherwise.
 */
int fdbox_register_handler(const char *compatible,
			   struct file *(*handler)(const void *fdt, int offset));
#endif /* _LINUX_FDBOX_H */
