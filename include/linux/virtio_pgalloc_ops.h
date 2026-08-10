/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Kernel-internal interface between the page allocator and the
 * virtio-pgalloc driver.
 *
 * When the driver is active, a static key gates two small changes in the
 * page allocator (mm/page_alloc.c):
 *
 *  - __del_page_from_free_list() does not clear PG_pgalloc_reported
 *    immediately on pageblock-aligned heads, so reported (unbacked) page
 *    blocks keep their state until the block is actually allocated again.
 *
 *  - prep_new_page() calls virtio_pgalloc_check_alloc() after every
 *    allocation; for an allocation from a reported block, the driver is
 *    notified so the host can clear the discardable marks of the backing
 *    folios: blocking allocations wait for the host ACK (unreport_sync),
 *    non-blocking ones must have been skipped by the rmqueue paths and
 *    only take a degraded fire-and-forget fallback (unreport).
 */
#ifndef _LINUX_VIRTIO_PGALLOC_OPS_H
#define _LINUX_VIRTIO_PGALLOC_OPS_H

#include <linux/gfp_types.h>
#include <linux/jump_label.h>
#include <linux/types.h>

struct page;

/* Gate for the page allocator hooks; defined in mm/virtio_pgalloc.c. */
DECLARE_STATIC_KEY_FALSE(virtio_pgalloc_enabled);

struct virtio_pgalloc_ops {
	/*
	 * Called after a reported page block was allocated again, from a
	 * context that may sleep (gfpflags_allow_blocking() was true).
	 * @data: the driver instance registered via virtio_pgalloc_register_ops.
	 * @head: the pageblock-aligned head page of the reallocated block.
	 *
	 * Sends the Alloc request and waits for the host ACK. The caller
	 * clears PG_pgalloc_reported only after this returns 0, so the flag
	 * stays set while the host may still consider the block discardable
	 * (guide §6.6.2 invariant). Returns 0 on ACK, -errno on failure
	 * (timeout / broken device); the caller then degrades.
	 */
	int (*unreport_sync)(void *data, struct page *head);
	/*
	 * Fire-and-forget Alloc notification for contexts that must not sleep
	 * (degraded fallback: the caller clears PG_pgalloc_reported and accepts
	 * the bounded host-stale window; see guide §6.6.2).
	 */
	void (*unreport)(void *data, struct page *head);
};

void virtio_pgalloc_register_ops(struct virtio_pgalloc_ops *ops, void *data);
void virtio_pgalloc_unregister_ops(struct virtio_pgalloc_ops *ops, void *data);
void virtio_pgalloc_check_alloc(struct page *page, unsigned int order,
				gfp_t gfp_flags);

#endif /* _LINUX_VIRTIO_PGALLOC_OPS_H */
