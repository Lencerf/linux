// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Virtio-pgalloc device driver (guest side)
 *
 * virtio-pgalloc lets the guest page allocator cooperate with the host
 * memory management through the vhost framework:
 *
 *  - Free page blocks (pageblock granularity, 2 MiB on x86_64) are reported
 *    to the host via the standard free page reporting mechanism. The driver
 *    waits for the host ACK before the blocks are returned to the buddy
 *    system as "reported" (unbacked) -- see struct page_reporting_dev_info.
 *    The host marks the backing folios discardable; reclaim drops them
 *    without writeback under host memory pressure.
 *
 *  - Allocations from reported blocks notify the host synchronously
 *    (unreport_sync, waiting for the ACK before the guest writes) in
 *    blocking contexts; non-blocking allocation paths skip reported
 *    blocks entirely, so every unbacked block is re-backed before use.
 *  - A DISABLE request clears all discardable marks on the host before
 *    the driver is torn down.
 *  - (future) The host emits reclaim events on the eventq.
 *
 * Copyright (c) 2026 The virtio-pgalloc project
 */
#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_pgalloc.h>
#include <linux/virtio_pgalloc_ops.h>
#include <linux/virtio_ring.h>
#include <linux/page_reporting.h>
#include <linux/slab.h>
#include <linux/mm.h>

/* How long to wait for a host response before giving up. */
#define VIRTIO_PGALLOC_RESP_TIMEOUT	(5 * HZ)

/* Fire-and-forget Alloc notification slots. */
#define VIRTIO_PGALLOC_ALLOC_SLOTS	64

/*
 * Always write the doorbell instead of virtqueue_kick(), which consults the
 * EVENT_IDX avail_event field. With a vhost backend that field is only
 * initialized by the first vhost_enable_notify(), i.e. after the first kick
 * has already been processed; before that it holds uninitialized vring
 * memory, so virtqueue_kick_prepare() may suppress the very first kick and
 * the request stalls forever (observed: all requestq/eventq kicks dead until
 * a forced notify). Always notifying is spec-legal (suppression is only an
 * optimization) and the request rate -- one per reported 2 MiB block --
 * makes the extra doorbell writes negligible.
 */
static void virtio_pgalloc_notify(struct virtqueue *vq)
{
	virtqueue_notify(vq);
}

struct virtio_pgalloc_alloc_slot {
	struct virtio_pgalloc_req req;
	struct virtio_pgalloc_resp resp;
	/*
	 * Synchronous (wait-for-ACK) usage: the IRQ handler sets @done and the
	 * waiter clears the slot's used bit itself, so a slot is never
	 * reacquired while its waiter is still reading @resp. Fire-and-forget
	 * usage: the IRQ handler clears the used bit directly.
	 */
	bool sync;
	bool done;
};

struct virtio_pgalloc {
	struct virtio_device *vdev;

	/* guest -> host request queue (requests and responses) */
	struct virtqueue *requestq;
	/* host -> guest event queue */
	struct virtqueue *eventq;

	/* set to unblock pending waiters on teardown */
	bool broken;
	/* wait for a host response on the requestq */
	wait_queue_head_t req_done;

	/*
	 * One request/response slot for the synchronous Free path. The page
	 * reporting workqueue calls report() strictly serially, so a single
	 * slot is sufficient.
	 */
	struct virtio_pgalloc_req req;
	struct virtio_pgalloc_resp resp;
	/* set by the interrupt handler when the Free response was consumed */
	bool free_acked;

	/*
	 * Slots for fire-and-forget Alloc notifications sent from the page
	 * allocator hook (any context, must not sleep). Freed again by the
	 * interrupt handler when the host has consumed the buffer.
	 */
	struct virtio_pgalloc_alloc_slot alloc_slots[VIRTIO_PGALLOC_ALLOC_SLOTS];
	unsigned long alloc_slots_used[BITS_TO_LONGS(VIRTIO_PGALLOC_ALLOC_SLOTS)];
	spinlock_t alloc_slots_lock;

	/* managed region as communicated by the device config */
	u64 addr;
	u64 region_size;
	u64 pageblock_size;

	/* free page reporting device */
	struct page_reporting_dev_info pr_dev_info;

	/* eventq buffers */
	struct virtio_pgalloc_event *events;
	unsigned int eventq_capacity;
};

/* Called on completion of a requestq descriptor chain. */
static void virtio_pgalloc_req_done(struct virtqueue *vq)
{
	struct virtio_pgalloc *pg = vq->vdev->priv;
	unsigned int len;
	void *token;

	while ((token = virtqueue_get_buf(vq, &len)) != NULL) {
		if (token == pg) {
			WRITE_ONCE(pg->free_acked, true);
		} else {
			struct virtio_pgalloc_alloc_slot *slot = token;
			unsigned long flags;

			/*
			 * Defensive: never trust a token that is not one of our
			 * alloc slots. A corrupted ring (or a host writing used
			 * entries at wrong indices) would otherwise hand us a
			 * garbage pointer to __clear_bit() on.
			 */
			if (slot < pg->alloc_slots ||
			    slot >= pg->alloc_slots + VIRTIO_PGALLOC_ALLOC_SLOTS) {
				dev_err(&pg->vdev->dev,
					"invalid requestq token %px (slots [%px,%px)) len=%u\n",
					token, pg->alloc_slots,
					pg->alloc_slots + VIRTIO_PGALLOC_ALLOC_SLOTS,
					len);
				continue;
			}

			spin_lock_irqsave(&pg->alloc_slots_lock, flags);
			if (slot->sync) {
				/* The waiter releases the slot itself after consuming
				 * the response; we only flag completion. */
				WRITE_ONCE(slot->done, true);
			} else {
				__clear_bit(slot - pg->alloc_slots, pg->alloc_slots_used);
			}
			spin_unlock_irqrestore(&pg->alloc_slots_lock, flags);
		}
	}
	wake_up(&pg->req_done);
}

/* (Re)fill the eventq with empty buffers to receive host events. */
static void virtio_pgalloc_eventq_fill(struct virtio_pgalloc *pg)
{
	struct scatterlist sg;
	unsigned int i;
	int err;

	for (i = 0; i < pg->eventq_capacity; i++) {
		sg_init_one(&sg, &pg->events[i], sizeof(pg->events[0]));
		err = virtqueue_add_inbuf(pg->eventq, &sg, 1, &pg->events[i],
					  GFP_ATOMIC);
		if (err)
			break;
	}
	virtio_pgalloc_notify(pg->eventq);
}

/* Called when the host signals used buffers on the eventq. */
static void virtio_pgalloc_eventq_done(struct virtqueue *vq)
{
	struct virtio_pgalloc *pg = vq->vdev->priv;
	struct virtio_pgalloc_event *evt;
	unsigned int len;

	while ((evt = virtqueue_get_buf(pg->eventq, &len)) != NULL) {
		if (le16_to_cpu(evt->type) == VIRTIO_PGALLOC_EVT_RECLAIM) {
			dev_info(&pg->vdev->dev,
				 "reclaim event: target_free_blocks = %llu\n",
				 le64_to_cpu(evt->target_free_blocks));
			/* P4: trigger guest-side memory reclamation. */
		} else {
			dev_info(&pg->vdev->dev, "unknown event type %u\n",
				 le16_to_cpu(evt->type));
		}
	}
	virtio_pgalloc_eventq_fill(pg);
}

/*
 * Send one request on the requestq and wait for the host response.
 *
 * Uses the single request/response slot shared with the reporting path;
 * callers must serialize: the page reporting workqueue calls report()
 * strictly serially, and DISABLE runs only after reporting is stopped.
 */
static int virtio_pgalloc_send_sync(struct virtio_pgalloc *pg, u16 type,
				    u64 gpa, u64 num_blocks)
{
	struct virtqueue *vq = pg->requestq;
	struct scatterlist sg_req, sg_resp;
	struct scatterlist *sgs[2];
	int err;

	pg->req.type = cpu_to_le16(type);
	pg->req.gpa = cpu_to_le64(gpa);
	pg->req.num_blocks = cpu_to_le64(num_blocks);

	sg_init_one(&sg_req, &pg->req, sizeof(pg->req));
	sg_init_one(&sg_resp, &pg->resp, sizeof(pg->resp));
	sgs[0] = &sg_req;
	sgs[1] = &sg_resp;

	WRITE_ONCE(pg->free_acked, false);

	err = virtqueue_add_sgs(vq, sgs, 1, 1, pg, GFP_KERNEL);
	if (err) {
		dev_err(&pg->vdev->dev, "failed to queue request: %d\n", err);
		return err;
	}
	virtio_pgalloc_notify(vq);

	/* Wait for the host to process the request and write the response. */
	if (!wait_event_timeout(pg->req_done,
				READ_ONCE(pg->free_acked) || pg->broken,
				VIRTIO_PGALLOC_RESP_TIMEOUT)) {
		dev_warn(&pg->vdev->dev,
			 "timed out waiting for response (type %u gpa %llx)\n",
			 type, gpa);
		return -ETIMEDOUT;
	}
	if (pg->broken)
		return -EIO;
	if (le16_to_cpu(pg->resp.status) != VIRTIO_PGALLOC_RESP_ACK) {
		dev_warn(&pg->vdev->dev, "request type %u rejected (status %u)\n",
			 type, le16_to_cpu(pg->resp.status));
		return -EIO;
	}
	return 0;
}

/*
 * Send one Free request for a page block and wait for the host ACK.
 *
 * Called from the page reporting workqueue context, which may sleep.
 */
static int virtio_pgalloc_send_free_request(struct virtio_pgalloc *pg,
					    struct page *page, u64 block_size)
{
	u64 gpa = page_to_phys(page);
	u64 blocks = block_size / pg->pageblock_size;

	return virtio_pgalloc_send_sync(pg, VIRTIO_PGALLOC_REQ_FREE, gpa, blocks);
}

/*
 * Free page reporting callback: report each page block in the scatterlist
 * to the host and wait for the ACK, so the blocks are only returned to the
 * guest allocator (marked PG_reported) after the host finished processing.
 *
 * Blocks outside the managed region are skipped: they are backed statically
 * and must never be reported as free. Note that page reporting will mark
 * all entries of a successfully reported batch as "reported"; skipping an
 * entry therefore marks it reported without an actual host request, which
 * is harmless (the host never touches it; the flag is cleared on realloc).
 */
static int virtio_pgalloc_report(struct page_reporting_dev_info *pr_dev_info,
				 struct scatterlist *sg, unsigned int nents)
{
	struct virtio_pgalloc *pg =
		container_of(pr_dev_info, struct virtio_pgalloc, pr_dev_info);
	unsigned int i;

	for (i = 0; i < nents; i++) {
		struct page *page = sg_page(&sg[i]);
		u64 gpa = page_to_phys(page);
		u64 block_size = sg[i].length;
		int err;

		if (gpa < pg->addr || gpa + block_size > pg->addr + pg->region_size)
			continue;

		err = virtio_pgalloc_send_free_request(pg, page, block_size);
		if (err)
			return err;
	}
	return 0;
}

/*
 * Only pageblocks inside the managed region are ever marked discardable on
 * the host. page reporting still flags out-of-region free blocks (the drain
 * marks every entry of a reported batch, including ones report() skipped),
 * so the alloc hook can fire for them; there is nothing to notify in that
 * case, and the mm core just clears the stale flag.
 */
static bool virtio_pgalloc_in_region(struct virtio_pgalloc *pg, u64 phys)
{
	u64 len = (u64)PAGE_SIZE << pageblock_order;

	return phys >= pg->addr && phys + len <= pg->addr + pg->region_size;
}

/*
 * Fire-and-forget Alloc notification: the guest reallocated a reported
 * (unbacked) page block, so the host must clear the discardable marks of
 * the backing folios. Called from the page allocator hook and must not
 * sleep; if no slot is available the notification is dropped (the window
 * is bounded by slot recycling and the host treats Alloc idempotently).
 */
static void virtio_pgalloc_unreport(void *data, struct page *head)
{
	struct virtio_pgalloc *pg = data;
	struct virtio_pgalloc_alloc_slot *slot = NULL;
	struct scatterlist sg_req, sg_resp;
	struct scatterlist *sgs[2];
	unsigned long flags;
	unsigned int bit;
	int err;

	if (!virtio_pgalloc_in_region(pg, page_to_phys(head)))
		return;

	spin_lock_irqsave(&pg->alloc_slots_lock, flags);
	bit = find_first_zero_bit(pg->alloc_slots_used,
				  VIRTIO_PGALLOC_ALLOC_SLOTS);
	if (bit < VIRTIO_PGALLOC_ALLOC_SLOTS) {
		__set_bit(bit, pg->alloc_slots_used);
		slot = &pg->alloc_slots[bit];
	}
	spin_unlock_irqrestore(&pg->alloc_slots_lock, flags);

	if (!slot) {
		dev_warn_ratelimited(&pg->vdev->dev,
				     "Alloc notification dropped: all slots in use\n");
		return;
	}

	slot->sync = false;
	slot->req.type = cpu_to_le16(VIRTIO_PGALLOC_REQ_ALLOC);
	slot->req.gpa = cpu_to_le64(page_to_phys(head));
	slot->req.num_blocks = cpu_to_le64(((u64)PAGE_SIZE << pageblock_order) /
					   pg->pageblock_size);

	sg_init_one(&sg_req, &slot->req, sizeof(slot->req));
	sg_init_one(&sg_resp, &slot->resp, sizeof(slot->resp));
	sgs[0] = &sg_req;
	sgs[1] = &sg_resp;

	err = virtqueue_add_sgs(pg->requestq, sgs, 1, 1, slot, GFP_ATOMIC);
	if (err) {
		dev_err(&pg->vdev->dev, "failed to queue Alloc notification: %d\n",
			err);
		spin_lock_irqsave(&pg->alloc_slots_lock, flags);
		__clear_bit(slot - pg->alloc_slots, pg->alloc_slots_used);
		spin_unlock_irqrestore(&pg->alloc_slots_lock, flags);
		wake_up(&pg->req_done);
		return;
	}
	virtio_pgalloc_notify(pg->requestq);
}

/*
 * Synchronous Alloc notification: the guest reallocated a reported (unbacked)
 * page block, so the host must clear the discardable marks of the backing
 * folios. Called from the page allocator hook in a context that may sleep;
 * the caller only clears PG_pgalloc_reported after we return 0, so the flag
 * stays set while the host may still consider the block discardable.
 */
static int virtio_pgalloc_unreport_sync(void *data, struct page *head)
{
	struct virtio_pgalloc *pg = data;
	struct virtio_pgalloc_alloc_slot *slot = NULL;
	struct scatterlist sg_req, sg_resp;
	struct scatterlist *sgs[2];
	unsigned long flags;
	unsigned int bit;
	int err;

	if (!virtio_pgalloc_in_region(pg, page_to_phys(head)))
		return 0;

	/* Acquire a slot (sleep while all are in flight). */
	for (;;) {
		spin_lock_irqsave(&pg->alloc_slots_lock, flags);
		bit = find_first_zero_bit(pg->alloc_slots_used,
					  VIRTIO_PGALLOC_ALLOC_SLOTS);
		if (bit < VIRTIO_PGALLOC_ALLOC_SLOTS) {
			__set_bit(bit, pg->alloc_slots_used);
			spin_unlock_irqrestore(&pg->alloc_slots_lock, flags);
			slot = &pg->alloc_slots[bit];
			break;
		}
		spin_unlock_irqrestore(&pg->alloc_slots_lock, flags);
		if (pg->broken)
			return -EIO;
		if (!wait_event_timeout(pg->req_done,
					!bitmap_full(pg->alloc_slots_used,
						     VIRTIO_PGALLOC_ALLOC_SLOTS) ||
					pg->broken, HZ)) {
			dev_warn(&pg->vdev->dev,
				 "Alloc slot starvation (dropped, phys=%llx)\n",
				 (u64)page_to_phys(head));
			return -ETIMEDOUT;
		}
	}

	slot->sync = true;
	WRITE_ONCE(slot->done, false);

	slot->req.type = cpu_to_le16(VIRTIO_PGALLOC_REQ_ALLOC);
	slot->req.gpa = cpu_to_le64(page_to_phys(head));
	slot->req.num_blocks = cpu_to_le64(((u64)PAGE_SIZE << pageblock_order) /
					   pg->pageblock_size);

	sg_init_one(&sg_req, &slot->req, sizeof(slot->req));
	sg_init_one(&sg_resp, &slot->resp, sizeof(slot->resp));
	sgs[0] = &sg_req;
	sgs[1] = &sg_resp;

	err = virtqueue_add_sgs(pg->requestq, sgs, 1, 1, slot, GFP_ATOMIC);
	if (err)
		goto out_release;
	virtio_pgalloc_notify(pg->requestq);

	/* Wait for the host to consume this slot. */
	if (!wait_event_timeout(pg->req_done, READ_ONCE(slot->done) || pg->broken,
				VIRTIO_PGALLOC_RESP_TIMEOUT)) {
		dev_warn(&pg->vdev->dev,
			 "timed out waiting for Alloc ACK (phys=%llx)\n",
			 (u64)page_to_phys(head));
		err = -ETIMEDOUT;
		goto out_release;
	}
	if (pg->broken) {
		err = -EIO;
		goto out_release;
	}
	err = (le16_to_cpu(slot->resp.status) == VIRTIO_PGALLOC_RESP_ACK) ? 0 : -EIO;

out_release:
	/* The waiter owns the slot until here (see virtio_pgalloc_req_done). */
	spin_lock_irqsave(&pg->alloc_slots_lock, flags);
	__clear_bit(bit, pg->alloc_slots_used);
	spin_unlock_irqrestore(&pg->alloc_slots_lock, flags);
	wake_up(&pg->req_done);
	return err;
}

static struct virtio_pgalloc_ops virtio_pgalloc_ops = {
	.unreport_sync = virtio_pgalloc_unreport_sync,
	.unreport = virtio_pgalloc_unreport,
};

static int virtio_pgalloc_probe(struct virtio_device *vdev)
{
	struct virtio_pgalloc *pg;
	struct virtqueue_info vqs_info[2] = {
		{ "requestq", virtio_pgalloc_req_done },
		{ "eventq", virtio_pgalloc_eventq_done },
	};
	struct virtqueue *vqs[2];
	u64 pageblock_size = 0, guest_pageblock_size;
	u16 node_id = 0;
	int err;

	pg = kzalloc(sizeof(*pg), GFP_KERNEL);
	if (!pg)
		return -ENOMEM;

	pg->vdev = vdev;
	init_waitqueue_head(&pg->req_done);
	spin_lock_init(&pg->alloc_slots_lock);
	vdev->priv = pg;

	err = virtio_find_vqs(vdev, 2, vqs, vqs_info, NULL);
	if (err)
		goto out_free;
	pg->requestq = vqs[VIRTIO_PGALLOC_VQ_REQUESTQ];
	pg->eventq = vqs[VIRTIO_PGALLOC_VQ_EVENTQ];

	virtio_cread_le(vdev, struct virtio_pgalloc_config, addr, &pg->addr);
	virtio_cread_le(vdev, struct virtio_pgalloc_config, region_size,
			&pg->region_size);
	if (virtio_has_feature(vdev, VIRTIO_PGALLOC_F_PAGEBLOCK_SIZE))
		virtio_cread_le(vdev, struct virtio_pgalloc_config,
				pageblock_size, &pageblock_size);
	if (virtio_has_feature(vdev, VIRTIO_PGALLOC_F_ACPI_PXM))
		virtio_cread_le(vdev, struct virtio_pgalloc_config, node_id,
				&node_id);

	if (!pageblock_size)
		pageblock_size = (u64)PAGE_SIZE << pageblock_order;
	pg->pageblock_size = pageblock_size;

	/*
	 * Requests are expressed in device pageblock units (num_blocks). The
	 * guest page reporting only produces blocks at pageblock_order, so
	 * the device unit must be a divisor of the guest pageblock size
	 * (both are powers of two, i.e. device unit <= guest pageblock).
	 */
	guest_pageblock_size = (u64)PAGE_SIZE << pageblock_order;
	if (pageblock_size > guest_pageblock_size ||
	    !IS_ALIGNED(guest_pageblock_size, pageblock_size)) {
		dev_err(&vdev->dev,
			"device pageblock size %llu is not a divisor of the guest pageblock size %lu\n",
			pageblock_size, (unsigned long)guest_pageblock_size);
		err = -EINVAL;
		goto out_del_vqs;
	}
	if (!pg->region_size || !IS_ALIGNED(pg->addr, pageblock_size)) {
		dev_err(&vdev->dev,
			"invalid managed region: addr %llx size %llu\n",
			pg->addr, pg->region_size);
		err = -EINVAL;
		goto out_del_vqs;
	}

	pg->eventq_capacity = virtqueue_get_vring_size(pg->eventq);
	pg->events = kcalloc(pg->eventq_capacity, sizeof(*pg->events),
			     GFP_KERNEL);
	if (!pg->events) {
		err = -ENOMEM;
		goto out_del_vqs;
	}

	pg->pr_dev_info.report = virtio_pgalloc_report;
	pg->pr_dev_info.order = pageblock_order;
	pg->pr_dev_info.capacity = virtqueue_get_vring_size(pg->requestq);

	/*
	 * Register the alloc-side notification first: it gates the deferred
	 * PG_pgalloc_reported clear and the alloc hook, both of which must be
	 * active before free page reporting starts.
	 */
	virtio_pgalloc_register_ops(&virtio_pgalloc_ops, pg);

	err = page_reporting_register(&pg->pr_dev_info);
	if (err)
		goto out_unregister_ops;

	virtio_device_ready(vdev);
	virtio_pgalloc_eventq_fill(pg);

	dev_info(&vdev->dev,
		 "virtio-pgalloc: managed region [%llx, %llx), pageblock %llu, node %u\n",
		 pg->addr, pg->addr + pg->region_size, pageblock_size,
		 node_id);
	return 0;

out_unregister_ops:
	virtio_pgalloc_unregister_ops(&virtio_pgalloc_ops, pg);
	kfree(pg->events);
out_del_vqs:
	vdev->config->del_vqs(vdev);
out_free:
	kfree(pg);
	vdev->priv = NULL;
	return err;
}

static void virtio_pgalloc_remove(struct virtio_device *vdev)
{
	struct virtio_pgalloc *pg = vdev->priv;

	/*
	 * 1. Stop free page reporting first: after page_reporting_unregister()
	 * (which drains the reporting workqueue) no Free request is in flight
	 * or will be sent.
	 */
	page_reporting_unregister(&pg->pr_dev_info);

	/*
	 * 2. DISABLE handshake (guide §6.8): the host clears every discardable
	 * mark in the managed region before ACKing, so no backing folio can be
	 * dropped once we stop notifying reallocations.
	 */
	if (!pg->broken) {
		int err = virtio_pgalloc_send_sync(pg, VIRTIO_PGALLOC_REQ_DISABLE,
						   0, 0);

		if (err)
			dev_warn(&vdev->dev,
				 "DISABLE failed (%d); host may still drop discardable folios\n",
				 err);
	}

	/* 3. Unblock any waiter stuck on the requestq. */
	pg->broken = true;
	wake_up_all(&pg->req_done);

	/*
	 * 4. Stop the alloc-side hook; this waits for in-flight hook calls
	 * (including sleeping sync Alloc waiters) before returning.
	 */
	virtio_pgalloc_unregister_ops(&virtio_pgalloc_ops, pg);

	virtio_reset_device(vdev);
	vdev->config->del_vqs(vdev);
	kfree(pg->events);
	kfree(pg);
	vdev->priv = NULL;
}

static const struct virtio_device_id virtio_pgalloc_id_table[] = {
	{ VIRTIO_ID_PGALLOC, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

/*
 * All device-specific feature bits the driver may check with
 * virtio_has_feature() must be listed here: for bits below
 * VIRTIO_TRANSPORT_F_START, virtio_has_feature() BUGs unless the
 * bit is declared in the driver feature table.
 */
static unsigned int virtio_pgalloc_features[] = {
	VIRTIO_PGALLOC_F_PAGEBLOCK_SIZE,
	VIRTIO_PGALLOC_F_ACPI_PXM,
};

static struct virtio_driver virtio_pgalloc_driver = {
	.feature_table = virtio_pgalloc_features,
	.feature_table_size = ARRAY_SIZE(virtio_pgalloc_features),
	.driver.name = KBUILD_MODNAME,
	.driver.owner = THIS_MODULE,
	.id_table = virtio_pgalloc_id_table,
	.probe = virtio_pgalloc_probe,
	.remove = virtio_pgalloc_remove,
};

module_virtio_driver(virtio_pgalloc_driver);

MODULE_DEVICE_TABLE(virtio, virtio_pgalloc_id_table);
MODULE_AUTHOR("The virtio-pgalloc project");
MODULE_DESCRIPTION("Virtio pgalloc driver");
MODULE_LICENSE("GPL-2.0-or-later");
