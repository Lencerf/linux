// SPDX-License-Identifier: GPL-2.0-only
/*
 * kexec_handover.c - kexec handover metadata processing
 * Copyright (C) 2023 Alexander Graf <graf@amazon.com>
 * Copyright (C) 2025 Microsoft Corporation, Mike Rapoport <rppt@kernel.org>
 * Copyright (C) 2025 Google LLC, Changyuan Lyu <changyuanl@google.com>
 */

#define pr_fmt(fmt) "KHO: " fmt

#include <linux/cma.h>
#include <linux/kexec.h>
#include <linux/libfdt.h>
#include <linux/debugfs.h>
#include <linux/memblock.h>
#include <linux/notifier.h>
#include <linux/kexec_handover.h>
#include <linux/page-isolation.h>
/*
 * KHO is tightly coupled with mm init and needs access to some of mm
 * internal APIs.
 */
#include "../mm/internal.h"
#include "kexec_internal.h"

static bool kho_enable __ro_after_init;

bool kho_is_enabled(void)
{
	return kho_enable;
}
EXPORT_SYMBOL_GPL(kho_is_enabled);

static int __init kho_parse_enable(char *p)
{
	return kstrtobool(p, &kho_enable);
}
early_param("kho", kho_parse_enable);

/* Helper functions for KHO state tree */

#define PROP_KHO_FDT "fdt"

struct kho_serialization {
	void *fdt;
};

/**
 * kho_add_fdt - Record a child FDT in KHO.
 * @ser: the `struct kho_serialization *` passed by KHO notifiers.
 * @name: the name of the child FDT.
 * @fdt: pointer to the child FDT.
 *
 * The child FDT must not exceed 1 page, and @fdt must be page aligned.
 *
 * Return: 0 on success, error code on failure.
 */
int kho_add_fdt(struct kho_serialization *ser, const char *name, void *fdt)
{
	int err = 0;
	phys_addr_t fdt_phys = virt_to_phys(fdt);
	size_t size = fdt_totalsize(fdt);

	if (!PAGE_ALIGNED(fdt_phys) || size < 0 || size > PAGE_SIZE)
		return -EINVAL;

	err |= fdt_begin_node(ser->fdt, name);
	err |= fdt_property(ser->fdt, PROP_KHO_FDT, &fdt_phys,
			    sizeof(fdt_phys));
	err |= fdt_end_node(ser->fdt);

	return err ? -EINVAL : 0;
}
EXPORT_SYMBOL_GPL(kho_add_fdt);

/*
 * With KHO enabled, memory can become fragmented because KHO regions may
 * be anywhere in physical address space. The scratch regions give us a
 * safe zones that we will never see KHO allocations from. This is where we
 * can later safely load our new kexec images into and then use the scratch
 * area for early allocations that happen before page allocator is
 * initialized.
 */
static struct kho_scratch *kho_scratch;
static unsigned int kho_scratch_cnt;

static struct dentry *debugfs_root;

struct kho_out {
	struct blocking_notifier_head chain_head;

	struct debugfs_blob_wrapper fdt_wrapper;
	struct dentry *fdt_file;
	struct dentry *dir;

	struct mutex lock; /* protects KHO FDT finalization */

	void *fdt;
	bool finalized;
};

static struct kho_out kho_out = {
	.chain_head = BLOCKING_NOTIFIER_INIT(kho_out.chain_head),
	.lock = __MUTEX_INITIALIZER(kho_out.lock),
	.finalized = false,
};

int register_kho_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&kho_out.chain_head, nb);
}
EXPORT_SYMBOL_GPL(register_kho_notifier);

int unregister_kho_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&kho_out.chain_head, nb);
}
EXPORT_SYMBOL_GPL(unregister_kho_notifier);

static int kho_out_update_debugfs_fdt(void)
{
	int err = 0;

	if (kho_out.finalized) {
		kho_out.fdt_wrapper.data = kho_out.fdt;
		kho_out.fdt_wrapper.size = fdt_totalsize(kho_out.fdt);
		kho_out.fdt_file = debugfs_create_blob("fdt", 0400, kho_out.dir,
						       &kho_out.fdt_wrapper);
		if (IS_ERR(kho_out.fdt_file))
			err = -ENOENT;
	} else {
		debugfs_remove(kho_out.fdt_file);
	}

	return err;
}

static int kho_abort(void)
{
	int err;

	err = blocking_notifier_call_chain(&kho_out.chain_head, KEXEC_KHO_ABORT,
					   NULL);
	err = notifier_to_errno(err);

	if (err)
		pr_err("Failed to abort KHO finalization: %d\n", err);

	return err;
}

static int kho_finalize(void)
{
	int err = 0;
	void *fdt = kho_out.fdt;

	err |= fdt_create(fdt, PAGE_SIZE);
	err |= fdt_finish_reservemap(fdt);
	err |= fdt_begin_node(fdt, "");
	if (err)
		goto abort;

	struct kho_serialization ser = {
		.fdt = fdt,
	};

	err = blocking_notifier_call_chain(&kho_out.chain_head,
					   KEXEC_KHO_FINALIZE, &ser);
	err = notifier_to_errno(err);
	if (err)
		goto abort;

	err |= fdt_end_node(fdt);
	err |= fdt_finish(fdt);

abort:
	if (err) {
		pr_err("Failed to convert KHO state tree: %d\n", err);
		kho_abort();
	}

	return err;
}

/* Handling for debug/kho/out */
static int kho_out_finalize_get(void *data, u64 *val)
{
	mutex_lock(&kho_out.lock);
	*val = kho_out.finalized;
	mutex_unlock(&kho_out.lock);

	return 0;
}

static int kho_out_finalize_set(void *data, u64 _val)
{
	int ret = 0;
	bool val = !!_val;

	mutex_lock(&kho_out.lock);

	if (val == kho_out.finalized) {
		if (kho_out.finalized)
			ret = -EEXIST;
		else
			ret = -ENOENT;
		goto unlock;
	}

	if (val)
		ret = kho_finalize();
	else
		ret = kho_abort();

	if (ret)
		goto unlock;

	kho_out.finalized = val;
	ret = kho_out_update_debugfs_fdt();

unlock:
	mutex_unlock(&kho_out.lock);
	return ret;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_kho_out_finalize, kho_out_finalize_get,
			 kho_out_finalize_set, "%llu\n");

static int scratch_phys_show(struct seq_file *m, void *v)
{
	for (int i = 0; i < kho_scratch_cnt; i++)
		seq_printf(m, "0x%llx\n", kho_scratch[i].addr);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(scratch_phys);

static int scratch_len_show(struct seq_file *m, void *v)
{
	for (int i = 0; i < kho_scratch_cnt; i++)
		seq_printf(m, "0x%llx\n", kho_scratch[i].size);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(scratch_len);

static __init int kho_out_debugfs_init(void)
{
	struct dentry *dir, *f;

	dir = debugfs_create_dir("out", debugfs_root);
	if (IS_ERR(dir))
		return -ENOMEM;

	f = debugfs_create_file("scratch_phys", 0400, dir, NULL,
				&scratch_phys_fops);
	if (IS_ERR(f))
		goto err_rmdir;

	f = debugfs_create_file("scratch_len", 0400, dir, NULL,
				&scratch_len_fops);
	if (IS_ERR(f))
		goto err_rmdir;

	f = debugfs_create_file("finalize", 0600, dir, NULL,
				&fops_kho_out_finalize);
	if (IS_ERR(f))
		goto err_rmdir;

	kho_out.dir = dir;
	return 0;

err_rmdir:
	debugfs_remove_recursive(dir);
	return -ENOENT;
}

static __init int kho_init(void)
{
	int err = 0;

	if (!kho_enable)
		return 0;

	kho_out.fdt = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!kho_out.fdt) {
		err = -ENOMEM;
		goto err_free_scratch;
	}

	debugfs_root = debugfs_create_dir("kho", NULL);
	if (IS_ERR(debugfs_root)) {
		err = -ENOENT;
		goto err_free_fdt;
	}

	err = kho_out_debugfs_init();
	if (err)
		goto err_free_fdt;

	for (int i = 0; i < kho_scratch_cnt; i++) {
		unsigned long base_pfn = PHYS_PFN(kho_scratch[i].addr);
		unsigned long count = kho_scratch[i].size >> PAGE_SHIFT;
		unsigned long pfn;

		for (pfn = base_pfn; pfn < base_pfn + count;
		     pfn += pageblock_nr_pages)
			init_cma_reserved_pageblock(pfn_to_page(pfn));
	}

	return 0;

err_free_fdt:
	kfree(kho_out.fdt);
	kho_out.fdt = NULL;
err_free_scratch:
	for (int i = 0; i < kho_scratch_cnt; i++) {
		void *start = __va(kho_scratch[i].addr);
		void *end = start + kho_scratch[i].size;

		free_reserved_area(start, end, -1, "");
	}
	kho_enable = false;
	return err;
}
late_initcall(kho_init);

/*
 * The scratch areas are scaled by default as percent of memory allocated from
 * memblock. A user can override the scale with command line parameter:
 *
 * kho_scratch=N%
 *
 * It is also possible to explicitly define size for a lowmem, a global and
 * per-node scratch areas:
 *
 * kho_scratch=l[KMG],n[KMG],m[KMG]
 *
 * The explicit size definition takes precedence over scale definition.
 */
static unsigned int scratch_scale __initdata = 200;
static phys_addr_t scratch_size_global __initdata;
static phys_addr_t scratch_size_pernode __initdata;
static phys_addr_t scratch_size_lowmem __initdata;

static int __init kho_parse_scratch_size(char *p)
{
	size_t len;
	unsigned long sizes[3];
	int i;

	if (!p)
		return -EINVAL;

	len = strlen(p);
	if (!len)
		return -EINVAL;

	/* parse nn% */
	if (p[len - 1] == '%') {
		/* unsigned int max is 4,294,967,295, 10 chars */
		char s_scale[11] = {};
		int ret = 0;

		if (len > ARRAY_SIZE(s_scale))
			return -EINVAL;

		memcpy(s_scale, p, len - 1);
		ret = kstrtouint(s_scale, 10, &scratch_scale);
		if (!ret)
			pr_notice("scratch scale is %d%%\n", scratch_scale);
		return ret;
	}

	/* parse ll[KMG],mm[KMG],nn[KMG] */
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		char *endp = p;

		if (i > 0) {
			if (*p != ',')
				return -EINVAL;
			p += 1;
		}

		sizes[i] = memparse(p, &endp);
		if (!sizes[i] || endp == p)
			return -EINVAL;
		p = endp;
	}

	scratch_size_lowmem = sizes[0];
	scratch_size_global = sizes[1];
	scratch_size_pernode = sizes[2];
	scratch_scale = 0;

	pr_notice("scratch areas: lowmem: %lluMiB global: %lluMiB pernode: %lldMiB\n",
		  (u64)(scratch_size_lowmem >> 20),
		  (u64)(scratch_size_global >> 20),
		  (u64)(scratch_size_pernode >> 20));

	return 0;
}
early_param("kho_scratch", kho_parse_scratch_size);

static void __init scratch_size_update(void)
{
	phys_addr_t size;

	if (!scratch_scale)
		return;

	size = memblock_reserved_kern_size(ARCH_LOW_ADDRESS_LIMIT,
					   NUMA_NO_NODE);
	size = size * scratch_scale / 100;
	scratch_size_lowmem = round_up(size, CMA_MIN_ALIGNMENT_BYTES);

	size = memblock_reserved_kern_size(MEMBLOCK_ALLOC_ANYWHERE,
					   NUMA_NO_NODE);
	size = size * scratch_scale / 100 - scratch_size_lowmem;
	scratch_size_global = round_up(size, CMA_MIN_ALIGNMENT_BYTES);
}

static phys_addr_t __init scratch_size_node(int nid)
{
	phys_addr_t size;

	if (scratch_scale) {
		size = memblock_reserved_kern_size(MEMBLOCK_ALLOC_ANYWHERE,
						   nid);
		size = size * scratch_scale / 100;
	} else {
		size = scratch_size_pernode;
	}

	return round_up(size, CMA_MIN_ALIGNMENT_BYTES);
}

/**
 * kho_reserve_scratch - Reserve a contiguous chunk of memory for kexec
 *
 * With KHO we can preserve arbitrary pages in the system. To ensure we still
 * have a large contiguous region of memory when we search the physical address
 * space for target memory, let's make sure we always have a large CMA region
 * active. This CMA region will only be used for movable pages which are not a
 * problem for us during KHO because we can just move them somewhere else.
 */
static void __init kho_reserve_scratch(void)
{
	phys_addr_t addr, size;
	int nid, i = 0;

	if (!kho_enable)
		return;

	scratch_size_update();

	/* FIXME: deal with node hot-plug/remove */
	kho_scratch_cnt = num_online_nodes() + 2;
	size = kho_scratch_cnt * sizeof(*kho_scratch);
	kho_scratch = memblock_alloc(size, PAGE_SIZE);
	if (!kho_scratch)
		goto err_disable_kho;

	/*
	 * reserve scratch area in low memory for lowmem allocations in the
	 * next kernel
	 */
	size = scratch_size_lowmem;
	addr = memblock_phys_alloc_range(size, CMA_MIN_ALIGNMENT_BYTES, 0,
					 ARCH_LOW_ADDRESS_LIMIT);
	if (!addr)
		goto err_free_scratch_desc;

	kho_scratch[i].addr = addr;
	kho_scratch[i].size = size;
	i++;

	/* reserve large contiguous area for allocations without nid */
	size = scratch_size_global;
	addr = memblock_phys_alloc(size, CMA_MIN_ALIGNMENT_BYTES);
	if (!addr)
		goto err_free_scratch_areas;

	kho_scratch[i].addr = addr;
	kho_scratch[i].size = size;
	i++;

	for_each_online_node(nid) {
		size = scratch_size_node(nid);
		addr = memblock_alloc_range_nid(size, CMA_MIN_ALIGNMENT_BYTES,
						0, MEMBLOCK_ALLOC_ACCESSIBLE,
						nid, true);
		if (!addr)
			goto err_free_scratch_areas;

		kho_scratch[i].addr = addr;
		kho_scratch[i].size = size;
		i++;
	}

	return;

err_free_scratch_areas:
	for (i--; i >= 0; i--)
		memblock_phys_free(kho_scratch[i].addr, kho_scratch[i].size);
err_free_scratch_desc:
	memblock_free(kho_scratch, kho_scratch_cnt * sizeof(*kho_scratch));
err_disable_kho:
	kho_enable = false;
}

void __init kho_memory_init(void)
{
	kho_reserve_scratch();
}
