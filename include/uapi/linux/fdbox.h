/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This file contains definitions and structures for fdbox ioctls.
 *
 * Copyright (C) 2024-2025 Amazon.com Inc. or its affiliates.
 *
 * Author: Pratyush Yadav <ptyadav@amazon.de>
 * Author: Alexander Graf <graf@amazon.com>
 */
#ifndef _UAPI_LINUX_FDBOX_H
#define _UAPI_LINUX_FDBOX_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define FDBOX_NAME_LEN			256

#define FDBOX_TYPE	('.')
#define FDBOX_BASE	0

/* Ioctls on /dev/fdbox/fdbox */

/* Create a box. */
#define FDBOX_CREATE_BOX	_IO(FDBOX_TYPE, FDBOX_BASE + 0)
struct fdbox_create_box {
	__u64 flags;
	__u8 name[FDBOX_NAME_LEN];
};

/* Delete a box. */
#define FDBOX_DELETE_BOX	_IO(FDBOX_TYPE, FDBOX_BASE + 1)
struct fdbox_delete_box {
	__u64 flags;
	__u8 name[FDBOX_NAME_LEN];
};

/* Ioctls on /dev/fdbox/$BOXNAME */

/* Put FD into box. This unmaps the FD from the calling process. */
#define FDBOX_PUT_FD	_IO(FDBOX_TYPE, FDBOX_BASE + 2)
struct fdbox_put_fd {
	__u64 flags;
	__u32 fd;
	__u32 pad;
	__u8 name[FDBOX_NAME_LEN];
};

/* Get the FD from box. This maps the FD into the calling process. */
#define FDBOX_GET_FD	_IO(FDBOX_TYPE, FDBOX_BASE + 3)
struct fdbox_get_fd {
	__u64 flags;
	__u32 pad;
	__u8 name[FDBOX_NAME_LEN];
};

/* Seal the box. After this, no FDs can be put in or taken out of the box. */
#define FDBOX_SEAL	_IO(FDBOX_TYPE, FDBOX_BASE + 4)
/* Unseal the box. Opposite of seal. */
#define FDBOX_UNSEAL	_IO(FDBOX_TYPE, FDBOX_BASE + 5)

#endif /* _UAPI_LINUX_FDBOX_H */
