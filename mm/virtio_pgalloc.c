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
#include <linux/wait.h>

DEFINE_STATIC_KEY_FALSE(virtio_pgalloc_enabled);
EXPORT_SYMBOL_GPL(virtio_pgalloc_enabled);

static struct virtio_pgalloc_ops __rcu *virtio_pgalloc_ops;
static void *virtio_pgalloc_ops_data;

/*
 * Number of hook calls currently inside the driver callbacks. The sync
 * Alloc path sleeps inside unreport_sync(), so unregister_ops must wait
 * for this to drain before the driver can free its state.
 */
static atomic_t virtio_pgalloc_inflight = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(virtio_pgalloc_waitq);

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
	/*
	 * After the grace period no new hook call can have fetched @ops.
	 * Wait for the calls that did (including sleeping sync Alloc waits)
	 * to leave the callbacks before the driver frees its state.
	 */
	synchronize_rcu();
	wait_event(virtio_pgalloc_waitq,
		   atomic_read(&virtio_pgalloc_inflight) == 0);
	WRITE_ONCE(virtio_pgalloc_ops_data, NULL);
	static_branch_disable(&virtio_pgalloc_enabled);
}
EXPORT_SYMBOL_GPL(virtio_pgalloc_unregister_ops);

/*
 * Called after every allocation when the static key is enabled. If the
 * containing page block still carries PG_pgalloc_reported (i.e. it is
 * unbacked: the host was told it is free and may still consider the
 * backing folios discardable), tell the host that the block is being
 * reallocated.
 *
 * Blocking allocations (gfpflags_allow_blocking()) use unreport_sync() and
 * clear the flag only after the host ACK -- the guide §6.6.2 invariant:
 * PG_pgalloc_reported set <=> the host has not yet processed the
 * reallocation. Non-blocking allocations cannot wait; the rmqueue paths
 * must have skipped reported blocks for them (guide §6.6.2), so the
 * fire-and-forget path below is only a degraded fallback.
 */
void virtio_pgalloc_check_alloc(struct page *page, unsigned int order,
				gfp_t gfp_flags)
{
	struct virtio_pgalloc_ops *ops;
	unsigned long pfn = page_to_pfn(page);
	struct page *head;

	/* Only pageblock-aligned heads carry PG_pgalloc_reported. */
	head = pfn_to_page(ALIGN_DOWN(pfn, pageblock_nr_pages));
	if (!PagePgallocReported(head))
		return;

	rcu_read_lock();
	ops = rcu_dereference(virtio_pgalloc_ops);
	if (ops)
		atomic_inc(&virtio_pgalloc_inflight);
	rcu_read_unlock();

	if (!ops) {
		/* Driver is going away; just drop the stale mark. */
		__ClearPagePgallocReported(head);
		return;
	}

	if (gfpflags_allow_blocking(gfp_flags)) {
		if (ops->unreport_sync(READ_ONCE(virtio_pgalloc_ops_data), head))
			pr_warn_ratelimited(
				"virtio_pgalloc: Alloc sync failed for block phys=%llx (host may still consider it discardable)\n",
				(u64)page_to_phys(head));
	} else {
		/* Degraded: should not happen with the rmqueue atomic skip. */
		ops->unreport(READ_ONCE(virtio_pgalloc_ops_data), head);
	}

	__ClearPagePgallocReported(head);
	atomic_dec(&virtio_pgalloc_inflight);
	wake_up(&virtio_pgalloc_waitq);
}
EXPORT_SYMBOL_GPL(virtio_pgalloc_check_alloc);
