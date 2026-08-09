// SPDX-License-Identifier: GPL-2.0-only
/*
 * Core hooks for virtio-pgalloc (mm side).
 *
 * Mirrors the mm/page_reporting.c pattern: a static key keeps the page
 * allocator overhead at zero when the virtio-pgalloc driver is not
 * active, and an RCU-protected ops pointer lets the driver register a
 * callback for the "reported block reallocated" notification.
 *
 * Copyright (c) 2026 The virtio-pgalloc project
 */
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/rcupdate.h>
#include <linux/static_key.h>
#include <linux/virtio_pgalloc_ops.h>

DEFINE_STATIC_KEY_FALSE(virtio_pgalloc_enabled);
EXPORT_SYMBOL_GPL(virtio_pgalloc_enabled);

static struct virtio_pgalloc_ops __rcu *virtio_pgalloc_ops;
static void *virtio_pgalloc_ops_data;

void virtio_pgalloc_register_ops(struct virtio_pgalloc_ops *ops, void *data)
{
	WRITE_ONCE(virtio_pgalloc_ops_data, data);
	rcu_assign_pointer(virtio_pgalloc_ops, ops);
	static_branch_enable(&virtio_pgalloc_enabled);
}
EXPORT_SYMBOL_GPL(virtio_pgalloc_register_ops);

void virtio_pgalloc_unregister_ops(struct virtio_pgalloc_ops *ops, void *data)
{
	if (rcu_access_pointer(virtio_pgalloc_ops) != ops)
		return;
	RCU_INIT_POINTER(virtio_pgalloc_ops, NULL);
	synchronize_rcu();
	WRITE_ONCE(virtio_pgalloc_ops_data, NULL);
	static_branch_disable(&virtio_pgalloc_enabled);
}
EXPORT_SYMBOL_GPL(virtio_pgalloc_unregister_ops);

/*
 * Called after every allocation when the static key is enabled. Checks
 * whether the containing page block is still marked PG_reported (i.e.
 * unbacked); if so, clears the bit and notifies the driver so the host
 * can clear the discardable marks of the backing folios.
 *
 * The test-and-clear is intentionally not atomic: concurrent allocators
 * of the same block may both observe the bit and both notify, which is
 * harmless (the host treats Alloc requests idempotently).
 */
void virtio_pgalloc_check_alloc(struct page *page, unsigned int order)
{
	struct virtio_pgalloc_ops *ops;
	unsigned long pfn = page_to_pfn(page);
	struct page *head;

	/* Only pageblock-aligned heads carry PG_reported. */
	head = pfn_to_page(ALIGN_DOWN(pfn, pageblock_nr_pages));
	/*
	 * PG_reported aliases PG_uptodate (page-flags.h), so the bit is also
	 * set on live page-cache pages. Only act if the head is still a buddy
	 * page (a reported block being partially reallocated), or the page we
	 * just allocated is the head itself (the whole block was taken);
	 * otherwise we would clear the uptodate bit of live data, corrupting
	 * page-cache contents.
	 */
	if (!PageReported(head) || (!PageBuddy(head) && head != page))
		return;

	__ClearPageReported(head);

	/* TEMP DEBUG */
	pr_info_ratelimited(
		"virtio_pgalloc DBG: realloc of reported block phys=%llx order=%u pfn=%lx head=%px\n",
		(u64)page_to_phys(head), order, pfn, head);

	rcu_read_lock();
	ops = rcu_dereference(virtio_pgalloc_ops);
	if (ops)
		ops->unreport(READ_ONCE(virtio_pgalloc_ops_data), head);
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(virtio_pgalloc_check_alloc);
