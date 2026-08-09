/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Kernel-internal interface between the page allocator and the
 * virtio-pgalloc driver.
 *
 * When the driver is active, a static key gates two small changes in the
 * page allocator (mm/page_alloc.c):
 *
 *  - __del_page_from_free_list() does not clear PG_reported immediately,
 *    so reported (unbacked) page blocks keep their state until the block
 *    is actually allocated again.
 *
 *  - prep_new_page() calls virtio_pgalloc_check_alloc() after every
 *    allocation; for an allocation from a reported block, the bit is
 *    cleared and the driver is notified (fire-and-forget Alloc request)
 *    so the host can clear the discardable marks of the backing folios.
 */
#ifndef _LINUX_VIRTIO_PGALLOC_OPS_H
#define _LINUX_VIRTIO_PGALLOC_OPS_H

#include <linux/jump_label.h>
#include <linux/types.h>

struct page;

/* Gate for the page allocator hooks; defined in mm/virtio_pgalloc.c. */
DECLARE_STATIC_KEY_FALSE(virtio_pgalloc_enabled);

struct virtio_pgalloc_ops {
	/*
	 * Called after a reported page block was allocated again.
	 * @data: the driver instance registered via virtio_pgalloc_register_ops.
	 * @head: the pageblock-aligned head page of the reallocated block.
	 * Must not sleep; only used to send a fire-and-forget notification.
	 */
	void (*unreport)(void *data, struct page *head);
};

void virtio_pgalloc_register_ops(struct virtio_pgalloc_ops *ops, void *data);
void virtio_pgalloc_unregister_ops(struct virtio_pgalloc_ops *ops, void *data);
void virtio_pgalloc_check_alloc(struct page *page, unsigned int order);

#endif /* _LINUX_VIRTIO_PGALLOC_OPS_H */
