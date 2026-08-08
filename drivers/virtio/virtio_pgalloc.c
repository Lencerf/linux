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
 *    (P1: the host vhost driver only acknowledges the requests; the actual
 *    discardable-marking comes in P2.)
 *
 *  - (future) Allocations from reported blocks trigger synchronous Alloc
 *    requests so the guest thread never stalls in a slow EPT fault.
 *  - (future) The host emits reclaim events on the eventq.
 *
 * Copyright (c) 2026 The virtio-pgalloc project
 */
#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_pgalloc.h>
#include <linux/page_reporting.h>
#include <linux/slab.h>
#include <linux/mm.h>

/* How long to wait for a host response before giving up. */
#define VIRTIO_PGALLOC_RESP_TIMEOUT	(5 * HZ)

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
	 * One request/response slot. The page reporting workqueue calls
	 * report() strictly serially, so a single slot is sufficient.
	 */
	struct virtio_pgalloc_req req;
	struct virtio_pgalloc_resp resp;

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
	virtqueue_kick(pg->eventq);
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
 * Send one Free request for a page block and wait for the host ACK.
 *
 * Called from the page reporting workqueue context, which may sleep.
 */
static int virtio_pgalloc_send_free_request(struct virtio_pgalloc *pg,
					    struct page *page, u64 block_size)
{
	struct virtqueue *vq = pg->requestq;
	struct scatterlist sg_req, sg_resp;
	struct scatterlist *sgs[2];
	unsigned int len;
	int err;

	pg->req.type = cpu_to_le16(VIRTIO_PGALLOC_REQ_FREE);
	pg->req.gpa = cpu_to_le64(page_to_phys(page));
	pg->req.num_blocks = cpu_to_le64(block_size / pg->pageblock_size);

	sg_init_one(&sg_req, &pg->req, sizeof(pg->req));
	sg_init_one(&sg_resp, &pg->resp, sizeof(pg->resp));
	sgs[0] = &sg_req;
	sgs[1] = &sg_resp;

	err = virtqueue_add_sgs(vq, sgs, 1, 1, pg, GFP_NOWAIT);
	if (err)
		return err;
	virtqueue_kick(vq);

	/* Wait for the host to process the request and write the response. */
	if (!wait_event_timeout(pg->req_done,
				virtqueue_get_buf(vq, &len) != NULL ||
				pg->broken,
				VIRTIO_PGALLOC_RESP_TIMEOUT)) {
		dev_warn(&pg->vdev->dev,
			 "timed out waiting for Free response (gpa %llx)\n",
			 le64_to_cpu(pg->req.gpa));
		return -ETIMEDOUT;
	}
	if (pg->broken)
		return -EIO;
	if (le16_to_cpu(pg->resp.status) != VIRTIO_PGALLOC_RESP_ACK) {
		dev_warn(&pg->vdev->dev, "Free request rejected (status %u)\n",
			 le16_to_cpu(pg->resp.status));
		return -EIO;
	}
	return 0;
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
	err = page_reporting_register(&pg->pr_dev_info);
	if (err)
		goto out_free_events;

	virtio_device_ready(vdev);
	virtio_pgalloc_eventq_fill(pg);

	dev_info(&vdev->dev,
		 "virtio-pgalloc: managed region [%llx, %llx), pageblock %llu, node %u\n",
		 pg->addr, pg->addr + pg->region_size, pageblock_size,
		 node_id);
	return 0;

out_free_events:
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

	/* Unblock any waiter stuck on the requestq. */
	pg->broken = true;
	wake_up_all(&pg->req_done);

	/*
	 * Wait for a possibly running report pass; with broken set, the
	 * report callback returns immediately.
	 */
	page_reporting_unregister(&pg->pr_dev_info);

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
