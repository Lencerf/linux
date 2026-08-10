/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Definitions for virtio-pgalloc device.
 *
 * virtio-pgalloc is a research device that lets the guest page allocator
 * cooperate with the host memory management: free page blocks are reported
 * to the host (which may mark them discardable and drop them lazily under
 * memory pressure), allocations from reported blocks are backed on demand,
 * and the host can ask the guest to proactively free memory.
 *
 * NOTE: This is a draft specification; the device ID is not assigned by the
 * virtio TC yet and everything here is subject to change.
 */
#ifndef _UAPI_LINUX_VIRTIO_PGALLOC_H
#define _UAPI_LINUX_VIRTIO_PGALLOC_H

#include <linux/types.h>
#include <linux/virtio_types.h>

/* Feature bits */
#define VIRTIO_PGALLOC_F_PAGEBLOCK_SIZE	0 /* config pageblock_size is valid */
#define VIRTIO_PGALLOC_F_ACPI_PXM	1 /* config node_id is valid */

/* Virtqueue indices */
#define VIRTIO_PGALLOC_VQ_REQUESTQ	0
#define VIRTIO_PGALLOC_VQ_EVENTQ	1

/*
 * Device configuration layout.
 *
 * The device manages the guest physical range [addr, addr + region_size)
 * in units of pageblock_size (typically 2 MiB).
 */
struct virtio_pgalloc_config {
	__le64 pageblock_size; /* management unit size in bytes */
	__le64 addr;           /* guest physical start of the managed region */
	__le64 region_size;    /* size of the managed region in bytes */
	__le16 node_id;        /* NUMA node id, valid with VIRTIO_PGALLOC_F_ACPI_PXM */
	__u8 padding[6];
};

/* Request types on the requestq */
#define VIRTIO_PGALLOC_REQ_ALLOC	0
#define VIRTIO_PGALLOC_REQ_FREE		1
#define VIRTIO_PGALLOC_REQ_DISABLE	2 /* v0.6: clear all marks in the region before
					 * the driver stops notifying (teardown handshake) */

struct virtio_pgalloc_req {
	__le16 type;
	__le16 padding[3];
	__le64 gpa;        /* guest physical address, aligned to pageblock_size */
	__le64 num_blocks; /* number of pageblocks */
};

/* Response status on the requestq */
#define VIRTIO_PGALLOC_RESP_ACK		0 /* request processed successfully */
#define VIRTIO_PGALLOC_RESP_BUSY	1 /* host busy, try again (for Alloc) */
#define VIRTIO_PGALLOC_RESP_ERROR	2 /* alignment or range error */

struct virtio_pgalloc_resp {
	__le16 status;
};

/* Event types on the eventq */
#define VIRTIO_PGALLOC_EVT_RECLAIM	0

struct virtio_pgalloc_event {
	__le16 type;
	__le16 padding[3];
	__le64 target_free_blocks; /* number of pageblocks the guest should free */
};

#endif /* _UAPI_LINUX_VIRTIO_PGALLOC_H */
