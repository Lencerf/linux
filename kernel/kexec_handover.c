// SPDX-License-Identifier: GPL-2.0-only
/*
 * kexec_handover.c - kexec handover metadata processing
 * Copyright (C) 2023 Alexander Graf <graf@amazon.com>
 * Copyright (C) 2025 Microsoft Corporation, Mike Rapoport <rppt@kernel.org>
 * Copyright (C) 2024 Google LLC, Changyuan Lyu <changyuanl@google.com>
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

#define KHO_RECURSIVE_FDT_PROP "kho,recursive-fdt"

static int __kho_init_fdt(void *fdt)
{
	int err = 0;

	err |= fdt_create(fdt, PAGE_SIZE);
	err |= fdt_finish_reservemap(fdt);

	return err;
}

/**
 * kho_free_fdt - free the underlying page of a KHO FDT fragment
 * @fdt: the fdt to be freed.
 */
void kho_free_fdt(struct kho_fdt *fdt)
{
	if (!fdt || !fdt->fdt)
		return;

	free_page((unsigned long)fdt->fdt);
	fdt->fdt = NULL;
}
EXPORT_SYMBOL_GPL(kho_free_fdt);

/**
 * kho_new_fdt - allocate the memory for a KHO fragment and init the FDT
 * @fdt: the fdt to be initialized.
 *
 * Return: 0 on success, and @fdt is initialized, or error code on failure.
 */
int kho_new_fdt(struct kho_fdt *fdt)
{
	int err = 0;

	fdt->fdt = (void *)get_zeroed_page(GFP_KERNEL);

	if (!fdt->fdt)
		return -ENOMEM;

	err = __kho_init_fdt(fdt->fdt);

	if (err)
		kho_free_fdt(fdt);

	return err;
}
EXPORT_SYMBOL_GPL(kho_new_fdt);

/**
 * kho_reset_fdt - wipe out the KHO and reset it
 * @fdt: the fdt to be reset
 *
 * Once done, @fdt is like just being initialized by kho_new_fdt().
 *
 * Return: 0 on success, or error code on failure.
 */
int kho_reset_fdt(struct kho_fdt *fdt)
{
	if (!fdt || !fdt->fdt)
		return -EINVAL;

	memset(fdt->fdt, 0, PAGE_SIZE);

	return __kho_init_fdt(fdt->fdt);
}
EXPORT_SYMBOL_GPL(kho_reset_fdt);

int kho_finish_fdt(struct kho_fdt *fdt)
{
	int err = 0;

	err |= fdt_finish(fdt->fdt);
	err |= fdt_check_header(fdt->fdt);

	return err;
}
EXPORT_SYMBOL_GPL(kho_finish_fdt);

int kho_validate_fdt(struct kho_fdt *fdt)
{
	if (!fdt || !fdt->fdt)
		return -ENOENT;
	return fdt_check_header(fdt->fdt);
}
EXPORT_SYMBOL_GPL(kho_validate_fdt);

/**
 * kho_link_fdt - link a child FDT fragment to a parent as a subnode
 * @parent: the parent FDT.
 * @name: the sub node name in the parent FDT to hold the child FDT address.
 * @child: the child FDT to be linked to the parent.
 *
 * Return: 0 on success, error code on failure.
 */
int kho_link_fdt(struct kho_fdt *parent, const char *name,
		 const struct kho_fdt *child)
{
	int err = 0;
	phys_addr_t fdt_phys = virt_to_phys(child->fdt);

	err |= fdt_begin_node(parent->fdt, name);
	err |= fdt_property(parent->fdt, KHO_RECURSIVE_FDT_PROP, &fdt_phys,
			    sizeof(fdt_phys));
	err |= fdt_end_node(parent->fdt);

	return err;
}
EXPORT_SYMBOL_GPL(kho_link_fdt);

int kho_begin_node(struct kho_fdt *fdt, const char *name)
{
	return fdt_begin_node(fdt->fdt, name);
}
EXPORT_SYMBOL_GPL(kho_begin_node);

int kho_end_node(struct kho_fdt *fdt)
{
	return fdt_end_node(fdt->fdt);
}
EXPORT_SYMBOL_GPL(kho_end_node);

int kho_add_prop(struct kho_fdt *fdt, const char *key, const void *val,
		 int size)
{
	return fdt_property(fdt->fdt, key, val, size);
}
EXPORT_SYMBOL_GPL(kho_add_prop);

int kho_add_string_prop(struct kho_fdt *fdt, const char *key, const char *val)
{
	return kho_add_prop(fdt, key, val, strlen(val) + 1);
}
EXPORT_SYMBOL_GPL(kho_add_string_prop);

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

	struct kho_fdt root_fdt;
	bool finalized;
};

static struct kho_out kho_out = {
	.chain_head = BLOCKING_NOTIFIER_INIT(kho_out.chain_head),
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
		kho_out.fdt_wrapper.data = kho_out.root_fdt.fdt;
		kho_out.fdt_wrapper.size = fdt_totalsize(kho_out.root_fdt.fdt);
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

	kho_reset_fdt(&kho_out.root_fdt);

	err = blocking_notifier_call_chain(&kho_out.chain_head, KEXEC_KHO_ABORT,
					   NULL);

	return notifier_to_errno(err);
}

static int kho_finalize(void)
{
	int err = 0;
	struct khoser_mem_chunk *first_chunk;
	struct kho_fdt *root = &kho_out.root_fdt;
	phys_addr_t *first_chunk_phys;

	err = kho_begin_node(root, "");
	if (err)
		goto unfreeze;

	struct kho_serialization ser = {
		.fdt = &kho_out.root_fdt,
	};
	err = blocking_notifier_call_chain(&kho_out.chain_head,
					   KEXEC_KHO_FINALIZE, &ser);
	err = notifier_to_errno(err);
	if (err)
		goto unfreeze;

	err = kho_end_node(root);
	if (err)
		goto unfreeze;

	err = kho_finish_fdt(root);

unfreeze:
	if (err) {
		int abort_err;

		pr_err("Failed to convert KHO state tree: %d\n", err);

		abort_err = kho_abort();
		if (abort_err)
			pr_err("Failed to abort KHO state tree: %d\n",
			       abort_err);
	}

	return err;
}

/* Handling for debug/kho/out */
static int kho_out_finalize_get(void *data, u64 *val)
{
	*val = kho_out.finalized;

	return 0;
}

static int kho_out_finalize_set(void *data, u64 _val)
{
	int ret = 0;
	bool val = !!_val;

	if (!kexec_trylock())
		return -EBUSY;

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
	kexec_unlock();
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
	int err;

	if (!kho_enable)
		return 0;

	err = kho_new_fdt(&kho_out.root_fdt);
	if (err) {
		pr_err("failed to init KHO root FDT: %d\n", err);
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
	kho_free_fdt(&kho_out.root_fdt);
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
	size_t p_len;
	unsigned long sizes[3];
	int i;

	if (!p)
		return -EINVAL;

	p_len = strlen(p);
	if (!p_len)
		return -EINVAL;

	/* parse nn% */
	if (p[p_len - 1] == '%') {
		/* unsigned int max is 4,294,967,295, 10 chars */
		char s_scale[11] = {};
		int ret = 0;

		if (p_len > ARRAY_SIZE(s_scale))
			return -EINVAL;

		memcpy(s_scale, p, p_len - 1);
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
