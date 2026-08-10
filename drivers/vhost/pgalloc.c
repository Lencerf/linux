// SPDX-License-Identifier: GPL-2.0-only
/*
 * vhost-pgalloc: host kernel backend for virtio-pgalloc
 *
 * Receives page block requests from the guest on the requestq: Free
 * requests mark the backing shmem folios as discardable (reclaim drops
 * them without writeback), Alloc requests clear the marks again when the
 * guest reallocates a reported block. The eventq is reserved for
 * host -> guest reclaim events (P4).
 *
 * Copyright (c) 2026 The virtio-pgalloc project
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/eventfd.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/vhost.h>
#include <linux/vhost_iotlb.h>
#include <linux/virtio_pgalloc.h>
#include <linux/vmalloc.h>

#include "vhost.h"

/* Max number of requests processed before requeueing the job. */
#define VHOST_PGALLOC_WEIGHT		0x80000
#define VHOST_PGALLOC_PKT_WEIGHT	256

static const int vhost_pgalloc_bits[] = {
	VHOST_FEATURES,
	VIRTIO_PGALLOC_F_PAGEBLOCK_SIZE,
	VIRTIO_PGALLOC_F_ACPI_PXM,
};

#define VHOST_PGALLOC_FEATURES \
	(VHOST_FEATURES_U64(vhost_pgalloc_bits, 0) & \
	 ~(1ULL << VIRTIO_RING_F_INDIRECT_DESC))

/*
 * INDIRECT_DESC is deliberately not negotiated: the guest sends Alloc
 * notifications from the page allocator hook, where kmalloc() of an indirect
 * descriptor table would recurse into the allocator (stack overflow). With
 * direct descriptor chains the virtqueue_add_sgs() path is allocation-free.
 */

enum {
	VHOST_PGALLOC_BACKEND_FEATURES = (1ULL << VHOST_BACKEND_F_IOTLB_MSG_V2)
};

enum {
	VHOST_PGALLOC_VQ_REQUESTQ = 0,
	VHOST_PGALLOC_VQ_EVENTQ = 1,
};

struct vhost_pgalloc {
	struct vhost_dev dev;
	struct vhost_virtqueue vqs[2];

	/* managed region as negotiated with the VMM (VHOST_PGALLOC_SET_CONFIG) */
	u64 pageblock_size;
	u64 addr;
	u64 region_size;
};

/* Translate a guest physical address to a host virtual address. */
static u64 vhost_pgalloc_gpa_to_hva(struct vhost_pgalloc *pg, u64 gpa, u64 len)
{
	struct vhost_iotlb_map *map;
	u64 last = gpa + len - 1;

	if (!pg->dev.umem)
		return 0;

	map = vhost_iotlb_itree_first(pg->dev.umem, gpa, last);
	if (!map || gpa < map->start || last > map->last) {
		pr_debug("no map for [%llx,%llx] (umem=%px)\n",
			 gpa, last, pg->dev.umem);
		return 0;
	}

	return map->addr + (gpa - map->start);
}

/*
 * Mark (on=1) or clear (on=0) the discardable state of the shmem folios
 * backing the guest physical range [gpa, gpa + len).
 */
static int vhost_pgalloc_mark_range(struct vhost_pgalloc *pg, u64 gpa, u64 len,
				    bool on)
{
	struct vm_area_struct *vma;
	struct file *file = NULL;
	struct address_space *mapping;
	pgoff_t start, end;
	u64 hva, offset;
	int ret = -EINVAL;

	if (!pg->dev.mm || !len)
		return -EINVAL;

	hva = vhost_pgalloc_gpa_to_hva(pg, gpa, len);
	if (!hva)
		return -EINVAL;

	/* The file offset is captured while holding the mmap lock; the vma
	 * must not be dereferenced after unlocking. */
	mmap_read_lock(pg->dev.mm);
	vma = find_vma(pg->dev.mm, hva);
	if (!vma) {
		pr_debug("no vma for hva %llx\n", hva);
		goto out_unlock;
	}
	if (hva < vma->vm_start || hva + len > vma->vm_end ||
	    !vma->vm_file) {
		pr_debug("vma range/file mismatch for hva %llx\n", hva);
		goto out_unlock;
	}
	file = get_file(vma->vm_file);
	offset = (vma->vm_pgoff << PAGE_SHIFT) + (hva - vma->vm_start);
	mmap_read_unlock(pg->dev.mm);

	if (!shmem_file(file)) {
		pr_debug("backing file is not shmem (file=%px)\n", file);
		goto out_file;
	}

	mapping = file->f_mapping;
	start = offset >> PAGE_SHIFT;
	end = (offset + len - 1) >> PAGE_SHIFT;

	shmem_mark_discardable(mapping, start, end, on);
	ret = 0;

out_file:
	fput(file);
	return ret;
out_unlock:
	mmap_read_unlock(pg->dev.mm);
	return ret;
}

/*
 * Free: the guest will not touch these blocks until reallocated, so the
 * backing folios can be marked discardable; reclaim then drops them
 * without writeback. The ACK is sent after the marking, so the guest
 * only returns the blocks to its allocator once the host is in a safe
 * state.
 */
static int vhost_pgalloc_handle_free(struct vhost_pgalloc *pg,
				     struct virtio_pgalloc_req *req)
{
	u64 gpa = le64_to_cpu(req->gpa);
	u64 len = le64_to_cpu(req->num_blocks) * pg->pageblock_size;

	if (!pg->pageblock_size || !len)
		return -EINVAL;
	if (gpa < pg->addr || gpa + len > pg->addr + pg->region_size)
		return -EINVAL;

	return vhost_pgalloc_mark_range(pg, gpa, len, true);
}

/*
 * Alloc: the guest reallocated a reported block; clear the discardable
 * marks so reclaim treats the folios as normal again. (P3 adds the
 * synchronous back with faultin; P2 only needs the mark clearing.)
 */
static int vhost_pgalloc_handle_alloc(struct vhost_pgalloc *pg,
				      struct virtio_pgalloc_req *req)
{
	u64 gpa = le64_to_cpu(req->gpa);
	u64 len = le64_to_cpu(req->num_blocks) * pg->pageblock_size;

	if (!pg->pageblock_size || !len)
		return -EINVAL;
	if (gpa < pg->addr || gpa + len > pg->addr + pg->region_size)
		return -EINVAL;

	pr_debug("alloc gpa=%llx len=%llu\n", gpa, len);
	return vhost_pgalloc_mark_range(pg, gpa, len, false);
}

/*
 * Disable: the guest is about to stop using the device (teardown or a
 * switch to another reporting mechanism). Clear every discardable mark in
 * the managed region before ACKing, so no backing folio can be dropped once
 * the guest stops sending Alloc notifications (guide §6.8).
 */
static int vhost_pgalloc_handle_disable(struct vhost_pgalloc *pg)
{
	if (!pg->pageblock_size || !pg->region_size)
		return -EINVAL;

	pr_info("disable: clearing discardable marks for region [%llx, %llx)\n",
		pg->addr, pg->addr + pg->region_size);
	return vhost_pgalloc_mark_range(pg, pg->addr, pg->region_size, false);
}

static void vhost_pgalloc_handle_req_kick(struct vhost_work *work)
{
	struct vhost_virtqueue *vq = container_of(work, struct vhost_virtqueue,
						  poll.work);
	struct vhost_pgalloc *pg = container_of(vq->dev, struct vhost_pgalloc,
						 dev);
	int head, pkts = 0;
	unsigned int out, in;
	bool added = false;

	mutex_lock(&vq->mutex);

	if (!vhost_vq_get_backend(vq))
		goto out;

	if (!vq_meta_prefetch(vq))
		goto out;

	vhost_disable_notify(&pg->dev, vq);
	do {
		struct virtio_pgalloc_req req;
		struct virtio_pgalloc_resp resp = {
			.status = cpu_to_le16(VIRTIO_PGALLOC_RESP_ACK),
		};
		struct iov_iter iov_iter;
		size_t len, nbytes;

		head = vhost_get_vq_desc(vq, vq->iov, ARRAY_SIZE(vq->iov),
					 &out, &in, NULL, NULL);
		if (head < 0)
			break;

		if (head == vq->num) {
			if (unlikely(vhost_enable_notify(&pg->dev, vq))) {
				vhost_disable_notify(&pg->dev, vq);
				continue;
			}
			break;
		}

		len = iov_length(vq->iov, out);
		if (out < 1 || in < 1 || len < sizeof(req)) {
			vq_err(vq, "invalid request chain: out %u in %u len %zu\n",
			       out, in, len);
			vhost_add_used(vq, head, 0);
			added = true;
			continue;
		}

		iov_iter_init(&iov_iter, ITER_SOURCE, vq->iov, out, len);
		nbytes = copy_from_iter(&req, sizeof(req), &iov_iter);
		if (nbytes != sizeof(req)) {
			vq_err(vq, "failed to copy request: %zu/%zu\n",
			       nbytes, sizeof(req));
			vhost_add_used(vq, head, 0);
			added = true;
			continue;
		}

		switch (le16_to_cpu(req.type)) {
		case VIRTIO_PGALLOC_REQ_FREE:
			if (vhost_pgalloc_handle_free(pg, &req))
				resp.status = cpu_to_le16(VIRTIO_PGALLOC_RESP_ERROR);
			break;
		case VIRTIO_PGALLOC_REQ_ALLOC:
			if (vhost_pgalloc_handle_alloc(pg, &req))
				resp.status = cpu_to_le16(VIRTIO_PGALLOC_RESP_ERROR);
			break;
		case VIRTIO_PGALLOC_REQ_DISABLE:
			if (vhost_pgalloc_handle_disable(pg))
				resp.status = cpu_to_le16(VIRTIO_PGALLOC_RESP_ERROR);
			break;
		default:
			resp.status = cpu_to_le16(VIRTIO_PGALLOC_RESP_ERROR);
			break;
		}

		nbytes = 0;
		len = iov_length(&vq->iov[out], in);
		if (len >= sizeof(resp)) {
			iov_iter_init(&iov_iter, ITER_DEST, &vq->iov[out], in,
				      len);
			nbytes = copy_to_iter(&resp, sizeof(resp), &iov_iter);
		}
		vhost_add_used(vq, head, nbytes);
		added = true;
	} while (likely(!vhost_exceeds_weight(vq, ++pkts, 0)));

	if (added)
		vhost_signal(&pg->dev, vq);

out:
	mutex_unlock(&vq->mutex);
}

/*
 * The guest kicks the eventq when it has replenished it with empty
 * buffers. P1: nothing to do -- events are only sent from P4 on.
 */
static void vhost_pgalloc_handle_eventq_kick(struct vhost_work *work)
{
	struct vhost_virtqueue *vq = container_of(work, struct vhost_virtqueue,
						  poll.work);

	mutex_lock(&vq->mutex);
	if (!vhost_vq_get_backend(vq))
		goto out;
	pr_debug("eventq buffers available\n");
out:
	mutex_unlock(&vq->mutex);
}

static int vhost_pgalloc_start(struct vhost_pgalloc *pg)
{
	struct vhost_virtqueue *vq;
	size_t i;
	int ret;

	mutex_lock(&pg->dev.mutex);

	ret = vhost_dev_check_owner(&pg->dev);
	if (ret)
		goto err;

	for (i = 0; i < ARRAY_SIZE(pg->vqs); i++) {
		vq = &pg->vqs[i];

		mutex_lock(&vq->mutex);

		if (!vhost_vq_access_ok(vq)) {
			ret = -EFAULT;
			goto err_vq;
		}

		if (!vhost_vq_get_backend(vq)) {
			vhost_vq_set_backend(vq, pg);
			ret = vhost_vq_init_access(vq);
			if (ret)
				goto err_vq;
		}

		mutex_unlock(&vq->mutex);
	}

	mutex_unlock(&pg->dev.mutex);
	return 0;

err_vq:
	vhost_vq_set_backend(vq, NULL);
	mutex_unlock(&vq->mutex);

	for (i = 0; i < ARRAY_SIZE(pg->vqs); i++) {
		vq = &pg->vqs[i];

		mutex_lock(&vq->mutex);
		vhost_vq_set_backend(vq, NULL);
		mutex_unlock(&vq->mutex);
	}
err:
	mutex_unlock(&pg->dev.mutex);
	return ret;
}

static int vhost_pgalloc_stop(struct vhost_pgalloc *pg, bool check_owner)
{
	size_t i;
	int ret = 0;

	mutex_lock(&pg->dev.mutex);

	if (check_owner) {
		ret = vhost_dev_check_owner(&pg->dev);
		if (ret)
			goto err;
	}

	for (i = 0; i < ARRAY_SIZE(pg->vqs); i++) {
		struct vhost_virtqueue *vq = &pg->vqs[i];

		mutex_lock(&vq->mutex);
		vhost_vq_set_backend(vq, NULL);
		mutex_unlock(&vq->mutex);
	}

err:
	mutex_unlock(&pg->dev.mutex);
	return ret;
}

static int vhost_pgalloc_set_features(struct vhost_pgalloc *pg, u64 features)
{
	struct vhost_virtqueue *vq;
	int i;

	if (features & ~VHOST_PGALLOC_FEATURES)
		return -EOPNOTSUPP;

	mutex_lock(&pg->dev.mutex);
	if ((features & (1 << VHOST_F_LOG_ALL)) &&
	    !vhost_log_access_ok(&pg->dev))
		goto err;

	if ((features & (1ULL << VIRTIO_F_ACCESS_PLATFORM))) {
		if (vhost_init_device_iotlb(&pg->dev))
			goto err;
	}

	for (i = 0; i < ARRAY_SIZE(pg->vqs); i++) {
		vq = &pg->vqs[i];
		mutex_lock(&vq->mutex);
		vq->acked_features = features;
		mutex_unlock(&vq->mutex);
	}
	mutex_unlock(&pg->dev.mutex);
	return 0;

err:
	mutex_unlock(&pg->dev.mutex);
	return -EFAULT;
}

static void vhost_pgalloc_free(struct vhost_pgalloc *pg)
{
	kvfree(pg);
}

static int vhost_pgalloc_dev_open(struct inode *inode, struct file *file)
{
	struct vhost_virtqueue **vqs;
	struct vhost_pgalloc *pg;
	int ret;

	/* This struct is large and allocation could fail, fall back to vmalloc
	 * if there is no other way.
	 */
	pg = kvmalloc_obj(*pg, GFP_KERNEL | __GFP_RETRY_MAYFAIL);
	if (!pg)
		return -ENOMEM;

	vqs = kmalloc_objs(*vqs, ARRAY_SIZE(pg->vqs));
	if (!vqs) {
		ret = -ENOMEM;
		goto out;
	}

	vqs[VHOST_PGALLOC_VQ_REQUESTQ] = &pg->vqs[VHOST_PGALLOC_VQ_REQUESTQ];
	vqs[VHOST_PGALLOC_VQ_EVENTQ] = &pg->vqs[VHOST_PGALLOC_VQ_EVENTQ];
	pg->vqs[VHOST_PGALLOC_VQ_REQUESTQ].handle_kick =
		vhost_pgalloc_handle_req_kick;
	pg->vqs[VHOST_PGALLOC_VQ_EVENTQ].handle_kick =
		vhost_pgalloc_handle_eventq_kick;

	vhost_dev_init(&pg->dev, vqs, ARRAY_SIZE(pg->vqs),
		       UIO_MAXIOV, VHOST_PGALLOC_PKT_WEIGHT,
		       VHOST_PGALLOC_WEIGHT, true, NULL);

	file->private_data = pg;
	return 0;

out:
	vhost_pgalloc_free(pg);
	return ret;
}

static int vhost_pgalloc_dev_release(struct inode *inode, struct file *file)
{
	struct vhost_pgalloc *pg = file->private_data;

	/* Don't check the owner, we are in the release path. */
	vhost_pgalloc_stop(pg, false);
	vhost_dev_flush(&pg->dev);
	vhost_dev_stop(&pg->dev);
	vhost_dev_cleanup(&pg->dev);
	kfree(pg->dev.vqs);
	vhost_pgalloc_free(pg);
	return 0;
}

static long vhost_pgalloc_dev_ioctl(struct file *f, unsigned int ioctl,
				    unsigned long arg)
{
	struct vhost_pgalloc *pg = f->private_data;
	void __user *argp = (void __user *)arg;
	u64 features;
	int start;
	int r;

	switch (ioctl) {
	case VHOST_PGALLOC_SET_CONFIG: {
		struct vhost_pgalloc_config cfg;

		if (copy_from_user(&cfg, argp, sizeof(cfg)))
			return -EFAULT;
		if (!cfg.pageblock_size ||
		    !is_power_of_2(cfg.pageblock_size) ||
		    !IS_ALIGNED(cfg.addr, cfg.pageblock_size))
			return -EINVAL;
		/* set before VHOST_PGALLOC_SET_RUNNING; the kick handlers read
		 * the fields without locking */
		mutex_lock(&pg->dev.mutex);
		pg->pageblock_size = cfg.pageblock_size;
		pg->addr = cfg.addr;
		pg->region_size = cfg.region_size;
		mutex_unlock(&pg->dev.mutex);
		return 0;
	}
	case VHOST_PGALLOC_SET_RUNNING:
		if (copy_from_user(&start, argp, sizeof(start)))
			return -EFAULT;
		if (start)
			return vhost_pgalloc_start(pg);
		else
			return vhost_pgalloc_stop(pg, true);
	case VHOST_GET_FEATURES:
		features = VHOST_PGALLOC_FEATURES;
		if (copy_to_user(argp, &features, sizeof(features)))
			return -EFAULT;
		return 0;
	case VHOST_SET_FEATURES:
		if (copy_from_user(&features, argp, sizeof(features)))
			return -EFAULT;
		return vhost_pgalloc_set_features(pg, features);
	case VHOST_GET_BACKEND_FEATURES:
		features = VHOST_PGALLOC_BACKEND_FEATURES;
		if (copy_to_user(argp, &features, sizeof(features)))
			return -EFAULT;
		return 0;
	case VHOST_SET_BACKEND_FEATURES:
		if (copy_from_user(&features, argp, sizeof(features)))
			return -EFAULT;
		if (features & ~VHOST_PGALLOC_BACKEND_FEATURES)
			return -EOPNOTSUPP;
		vhost_set_backend_features(&pg->dev, features);
		return 0;
	default:
		mutex_lock(&pg->dev.mutex);
		r = vhost_dev_ioctl(&pg->dev, ioctl, argp);
		if (r == -ENOIOCTLCMD)
			r = vhost_vring_ioctl(&pg->dev, ioctl, argp);
		else
			vhost_dev_flush(&pg->dev);
		mutex_unlock(&pg->dev.mutex);
		return r;
	}
}

static const struct file_operations vhost_pgalloc_fops = {
	.owner          = THIS_MODULE,
	.open           = vhost_pgalloc_dev_open,
	.release        = vhost_pgalloc_dev_release,
	.llseek		= noop_llseek,
	.unlocked_ioctl = vhost_pgalloc_dev_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

static struct miscdevice vhost_pgalloc_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "vhost-pgalloc",
	.fops = &vhost_pgalloc_fops,
	/* Dev convenience: world rw so no chmod is needed on test machines. */
	.mode = 0666,
};

static int __init vhost_pgalloc_init(void)
{
	return misc_register(&vhost_pgalloc_misc);
}

static void __exit vhost_pgalloc_exit(void)
{
	misc_deregister(&vhost_pgalloc_misc);
}

module_init(vhost_pgalloc_init);
module_exit(vhost_pgalloc_exit);

MODULE_AUTHOR("The virtio-pgalloc project");
MODULE_DESCRIPTION("vhost backend for virtio-pgalloc");
MODULE_LICENSE("GPL-2.0-only");
