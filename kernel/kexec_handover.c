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
#include <linux/count_zeros.h>
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
#define PROP_PRESERVED_MEMORY_MAP "preserved-memory-map"

struct kho_serialization {
	void *fdt;
	struct kho_mem_track *tracker;
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

	err = kho_preserve_phys(ser, fdt_phys, PAGE_SIZE);
	if (err)
		return err;

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

	/* First chunk of serialized preserved memory map */
	struct khoser_mem_chunk *preserved_mem_map;
	void *fdt;
	bool finalized;
};

static struct kho_out kho_out = {
	.chain_head = BLOCKING_NOTIFIER_INIT(kho_out.chain_head),
	.lock = __MUTEX_INITIALIZER(kho_out.lock),
	.finalized = false,
};

struct kho_in {
	struct debugfs_blob_wrapper fdt_wrapper;
	struct dentry *dir;
	phys_addr_t kho_scratch_phys;
	phys_addr_t fdt_phys;
};

static struct kho_in kho_in;

static const void *kho_get_fdt(void)
{
	return kho_in.fdt_phys ? phys_to_virt(kho_in.fdt_phys) : NULL;
}

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

/**
 * kho_retrieve_fdt - retrieve a child FDT saved by kho_add_fdt().
 * @name: the name of the child FDT.
 *
 * Return: pointer to the FDT on success, NULL if no such FDT is passed from
 * the previous kernel, error code on failure.
 */
const void *kho_retrieve_fdt(const char *name)
{
	const void *fdt = kho_get_fdt();
	void *sub_fdt;
	int offset = 0, size, sub_fdt_size;
	const phys_addr_t *fdt_phys;

	if (!fdt)
		return NULL;

	offset = fdt_subnode_offset(fdt, 0, name);
	if (offset < 0)
		return NULL;

	fdt_phys = fdt_getprop(fdt, offset, PROP_KHO_FDT, &size);
	if (!fdt_phys)
		return ERR_PTR(-ENOENT);
	if (size != sizeof(phys_addr_t))
		return ERR_PTR(-EINVAL);
	if (!PAGE_ALIGNED(*fdt_phys))
		return ERR_PTR(-EINVAL);

	sub_fdt = phys_to_virt(*fdt_phys);
	sub_fdt_size = fdt_totalsize(sub_fdt);
	if (sub_fdt_size > PAGE_SIZE)
		return ERR_PTR(-E2BIG);

	return kho_restore_phys(*fdt_phys, PAGE_SIZE);
}
EXPORT_SYMBOL_GPL(kho_retrieve_fdt);

/*
 * Keep track of memory that is to be preserved across KHO.
 *
 * The serializing side uses two levels of xarrays to manage chunks of per-order
 * 512 byte bitmaps. For instance the entire 1G order of a 1TB system would fit
 * inside a single 512 byte bitmap. For order 0 allocations each bitmap will
 * cover 16M of address space. Thus, for 16G of memory at most 512K
 * of bitmap memory will be needed for order 0.
 *
 * This approach is fully incremental, as the serialization progresses folios
 * can continue be aggregated to the tracker. The final step, immediately prior
 * to kexec would serialize the xarray information into a linked list for the
 * successor kernel to parse.
 */

#define PRESERVE_BITS (512 * 8)

struct kho_mem_phys_bits {
	DECLARE_BITMAP(preserve, PRESERVE_BITS);
};

struct kho_mem_phys {
	/*
	 * Points to kho_mem_phys_bits, a sparse bitmap array. Each bit is sized
	 * to order.
	 */
	struct xarray phys_bits;
};

struct kho_mem_track {
	/* Points to kho_mem_phys, each order gets its own bitmap tree */
	struct xarray orders;
};

static struct kho_mem_track kho_mem_track = {
	.orders = XARRAY_INIT(kho_mem_track.orders, 0),
};

static void *xa_load_or_alloc(struct xarray *xa, unsigned long index, size_t sz)
{
	void *elm, *res;

	elm = xa_load(xa, index);
	if (elm)
		return elm;

	elm = kzalloc(sz, GFP_KERNEL);
	if (!elm)
		return ERR_PTR(-ENOMEM);

	res = xa_cmpxchg(xa, index, NULL, elm, GFP_KERNEL);
	if (xa_is_err(res))
		res = ERR_PTR(xa_err(res));

	if (res) {
		kfree(elm);
		return res;
	}

	return elm;
}

static void __kho_unpreserve(struct kho_mem_track *tracker, unsigned long pfn,
			     unsigned int order)
{
	struct kho_mem_phys_bits *bits;
	struct kho_mem_phys *physxa;
	unsigned long pfn_high = pfn >> order;

	physxa = xa_load(&tracker->orders, order);
	if (!physxa)
		return;

	bits = xa_load(&physxa->phys_bits, pfn_high / PRESERVE_BITS);
	if (!bits)
		return;

	clear_bit(pfn_high % PRESERVE_BITS, bits->preserve);
}

static int __kho_preserve(struct kho_mem_track *tracker, unsigned long pfn,
			  unsigned int order)
{
	struct kho_mem_phys_bits *bits;
	struct kho_mem_phys *physxa;
	unsigned long pfn_high = pfn >> order;

	might_sleep();

	physxa = xa_load_or_alloc(&tracker->orders, order, sizeof(*physxa));
	if (IS_ERR(physxa))
		return PTR_ERR(physxa);

	bits = xa_load_or_alloc(&physxa->phys_bits, pfn_high / PRESERVE_BITS,
				sizeof(*bits));
	if (IS_ERR(bits))
		return PTR_ERR(bits);

	set_bit(pfn_high % PRESERVE_BITS, bits->preserve);

	return 0;
}

/**
 * kho_preserve_folio - preserve a folio across KHO.
 * @ser: the `struct kho_serialization *` passed by KHO notifiers.
 * @folio: folio to preserve
 *
 * Records that the entire folio is preserved across KHO. The order
 * will be preserved as well.
 *
 * Return: 0 on success, error code on failure
 */
int kho_preserve_folio(struct kho_serialization *ser, struct folio *folio)
{
	unsigned long pfn = folio_pfn(folio);
	unsigned int order = folio_order(folio);

	return __kho_preserve(ser->tracker, pfn, order);
}
EXPORT_SYMBOL_GPL(kho_preserve_folio);

/**
 * kho_preserve_phys - preserve a physically contiguous range across KHO.
 * @ser: the `struct kho_serialization *` passed by KHO notifiers.
 * @phys: physical address of the range
 * @size: size of the range
 *
 * Records that the entire range from @phys to @phys + @size is preserved
 * across KHO.
 *
 * Return: 0 on success, error code on failure
 */
int kho_preserve_phys(struct kho_serialization *ser, phys_addr_t phys,
		      size_t size)
{
	unsigned long pfn = PFN_DOWN(phys);
	const unsigned long end_pfn = PFN_UP(phys + size);
	unsigned long failed_pfn = 0;
	int err = 0;

	while (pfn < end_pfn) {
		unsigned int order =
			min(count_trailing_zeros(pfn), ilog2(end_pfn - pfn));

		err = __kho_preserve(ser->tracker, pfn, order);
		if (err) {
			failed_pfn = pfn;
			break;
		}

		pfn += (1 << order);
	}

	if (err) {
		pfn = PFN_DOWN(phys);
		while (pfn < failed_pfn) {
			unsigned int order = min(count_trailing_zeros(pfn),
						 ilog2(end_pfn - pfn));

			__kho_unpreserve(ser->tracker, pfn, order);
			pfn += (1 << order);
		}
	}

	return err;
}
EXPORT_SYMBOL_GPL(kho_preserve_phys);

/* almost as free_reserved_page(), just don't free the page */
static void kho_restore_page(struct page *page)
{
	ClearPageReserved(page);
	init_page_count(page);
	adjust_managed_page_count(page, 1);
}

struct folio *kho_restore_folio(phys_addr_t phys)
{
	struct page *page = pfn_to_online_page(PHYS_PFN(phys));
	unsigned long order = page->private;

	if (!page)
		return NULL;

	order = page->private;
	if (order)
		prep_compound_page(page, order);
	else
		kho_restore_page(page);

	return page_folio(page);
}
EXPORT_SYMBOL_GPL(kho_restore_folio);

void *kho_restore_phys(phys_addr_t phys, size_t size)
{
	unsigned long start_pfn, end_pfn, pfn;
	void *va = __va(phys);

	start_pfn = PFN_DOWN(phys);
	end_pfn = PFN_UP(phys + size);

	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		struct page *page = pfn_to_online_page(pfn);

		if (!page)
			return NULL;
		kho_restore_page(page);
	}

	return va;
}
EXPORT_SYMBOL_GPL(kho_restore_phys);

struct khoser_mem_bitmap_ptr {
	phys_addr_t phys_start;
	DECLARE_KHOSER_PTR(bitmap, struct kho_mem_phys_bits *);
};

struct khoser_mem_chunk;

struct khoser_mem_chunk_hdr {
	DECLARE_KHOSER_PTR(next, struct khoser_mem_chunk *);
	unsigned int order;
	unsigned int num_elms;
};

#define KHOSER_BITMAP_SIZE                                   \
	((PAGE_SIZE - sizeof(struct khoser_mem_chunk_hdr)) / \
	 sizeof(struct khoser_mem_bitmap_ptr))

struct khoser_mem_chunk {
	struct khoser_mem_chunk_hdr hdr;
	struct khoser_mem_bitmap_ptr bitmaps[KHOSER_BITMAP_SIZE];
};

static_assert(sizeof(struct khoser_mem_chunk) == PAGE_SIZE);

static struct khoser_mem_chunk *new_chunk(struct khoser_mem_chunk *cur_chunk,
					  unsigned long order)
{
	struct khoser_mem_chunk *chunk;

	chunk = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!chunk)
		return NULL;
	chunk->hdr.order = order;
	if (cur_chunk)
		KHOSER_STORE_PTR(cur_chunk->hdr.next, chunk);
	return chunk;
}

static void kho_mem_ser_free(struct khoser_mem_chunk *first_chunk)
{
	struct khoser_mem_chunk *chunk = first_chunk;

	while (chunk) {
		struct khoser_mem_chunk *tmp = chunk;

		chunk = KHOSER_LOAD_PTR(chunk->hdr.next);
		kfree(tmp);
	}
}

/*
 * Record all the bitmaps in a linked list of pages for the next kernel to
 * process. Each chunk holds bitmaps of the same order and each block of bitmaps
 * starts at a given physical address. This allows the bitmaps to be sparse. The
 * xarray is used to store them in a tree while building up the data structure,
 * but the KHO successor kernel only needs to process them once in order.
 *
 * All of this memory is normal kmalloc() memory and is not marked for
 * preservation. The successor kernel will remain isolated to the scratch space
 * until it completes processing this list. Once processed all the memory
 * storing these ranges will be marked as free.
 */
static struct khoser_mem_chunk *kho_mem_serialize(void)
{
	struct khoser_mem_chunk *first_chunk = NULL;
	struct khoser_mem_chunk *chunk = NULL;
	struct kho_mem_phys *physxa;
	unsigned long order;

	xa_for_each(&kho_mem_track.orders, order, physxa) {
		struct kho_mem_phys_bits *bits;
		unsigned long phys;

		chunk = new_chunk(chunk, order);
		if (!chunk)
			goto err_free;

		if (!first_chunk)
			first_chunk = chunk;

		xa_for_each(&physxa->phys_bits, phys, bits) {
			struct khoser_mem_bitmap_ptr *elm;

			if (chunk->hdr.num_elms == ARRAY_SIZE(chunk->bitmaps)) {
				chunk = new_chunk(chunk, order);
				if (!chunk)
					goto err_free;
			}

			elm = &chunk->bitmaps[chunk->hdr.num_elms];
			chunk->hdr.num_elms++;
			elm->phys_start = (phys * PRESERVE_BITS)
					  << (order + PAGE_SHIFT);
			KHOSER_STORE_PTR(elm->bitmap, bits);
		}
	}

	return first_chunk;

err_free:
	kho_mem_ser_free(first_chunk);
	return ERR_PTR(-ENOMEM);
}

static void deserialize_bitmap(unsigned int order,
			       struct khoser_mem_bitmap_ptr *elm)
{
	struct kho_mem_phys_bits *bitmap = KHOSER_LOAD_PTR(elm->bitmap);
	unsigned long bit;

	for_each_set_bit(bit, bitmap->preserve, PRESERVE_BITS) {
		int sz = 1 << (order + PAGE_SHIFT);
		phys_addr_t phys =
			elm->phys_start + (bit << (order + PAGE_SHIFT));
		struct page *page = phys_to_page(phys);

		memblock_reserve(phys, sz);
		memblock_reserved_mark_noinit(phys, sz);
		page->private = order;
	}
}

static void __init kho_mem_deserialize(void)
{
	struct khoser_mem_chunk *chunk;
	const void *fdt = kho_get_fdt();
	const phys_addr_t *mem;
	int len;

	if (!fdt)
		return;

	mem = fdt_getprop(fdt, 0, PROP_PRESERVED_MEMORY_MAP, &len);

	if (!mem || len != sizeof(*mem)) {
		pr_err("failed to get preserved memory bitmaps\n");
		return;
	}

	chunk = *mem ? phys_to_virt(*mem) : NULL;
	while (chunk) {
		unsigned int i;

		memblock_reserve(virt_to_phys(chunk), sizeof(*chunk));

		for (i = 0; i != chunk->hdr.num_elms; i++)
			deserialize_bitmap(chunk->hdr.order,
					   &chunk->bitmaps[i]);
		chunk = KHOSER_LOAD_PTR(chunk->hdr.next);
	}
}

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
	unsigned long order;
	struct kho_mem_phys *physxa;

	memset(kho_out.fdt, 0, PAGE_SIZE);

	xa_for_each(&kho_mem_track.orders, order, physxa) {
		struct kho_mem_phys_bits *bits;
		unsigned long phys;

		xa_for_each(&physxa->phys_bits, phys, bits)
			kfree(bits);

		xa_destroy(&physxa->phys_bits);
		kfree(physxa);
	}
	xa_destroy(&kho_mem_track.orders);

	if (kho_out.preserved_mem_map) {
		kho_mem_ser_free(kho_out.preserved_mem_map);
		kho_out.preserved_mem_map = NULL;
	}

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
	phys_addr_t *first_chunk_phys;
	void *fdt = kho_out.fdt;

	err |= fdt_create(fdt, PAGE_SIZE);
	err |= fdt_finish_reservemap(fdt);
	err |= fdt_begin_node(fdt, "");
	/**
	 * Reserve the preserved-memory-map property in the root FDT, since
	 * KHO users would add subnodes to the root node.
	 * Devicetree specification 5.4.2 requires that all property definitions
	 * for a particular node precede any subnode definitions for that node.
	 */
	err |= fdt_property_placeholder(fdt, PROP_PRESERVED_MEMORY_MAP,
					sizeof(phys_addr_t),
					(void **)&first_chunk_phys);
	if (err)
		goto abort;

	struct kho_serialization ser = {
		.fdt = fdt,
		.tracker = &kho_mem_track,
	};

	err = kho_preserve_phys(&ser, virt_to_phys(fdt), PAGE_SIZE);
	if (err)
		goto abort;

	err = blocking_notifier_call_chain(&kho_out.chain_head,
					   KEXEC_KHO_FINALIZE, &ser);
	err = notifier_to_errno(err);
	if (err)
		goto abort;

	kho_out.preserved_mem_map = kho_mem_serialize();
	if (IS_ERR(kho_out.preserved_mem_map)) {
		err = PTR_ERR(kho_out.preserved_mem_map);
		kho_out.preserved_mem_map = NULL;
		goto abort;
	}
	*first_chunk_phys = virt_to_phys(kho_out.preserved_mem_map);

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

/* Handling for debugfs/kho/in */

static __init int kho_in_debugfs_init(const void *fdt)
{
	struct dentry *file;
	int err;

	kho_in.dir = debugfs_create_dir("in", debugfs_root);
	if (IS_ERR(kho_in.dir))
		return PTR_ERR(kho_in.dir);

	kho_in.fdt_wrapper.size = fdt_totalsize(fdt);
	kho_in.fdt_wrapper.data = (void *)fdt;
	file = debugfs_create_blob("fdt", 0400, kho_in.dir,
				   &kho_in.fdt_wrapper);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_rmdir;
	}

	return 0;

err_rmdir:
	debugfs_remove(kho_in.dir);
	return err;
}

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
	const void *fdt = kho_get_fdt();

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

	if (fdt) {
		err = kho_in_debugfs_init(fdt);
		/*
		 * Failure to create /sys/kernel/debug/kho/in does not prevent
		 * reviving state from KHO and setting up KHO for the next
		 * kexec.
		 */
		if (err)
			pr_err("failed exposing handover FDT in debugfs\n");

		kho_scratch = __va(kho_in.kho_scratch_phys);

		return 0;
	}

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

static void __init kho_release_scratch(void)
{
	phys_addr_t start, end;
	u64 i;

	memmap_init_kho_scratch_pages();

	/*
	 * Mark scratch mem as CMA before we return it. That way we
	 * ensure that no kernel allocations happen on it. That means
	 * we can reuse it as scratch memory again later.
	 */
	__for_each_mem_range(i, &memblock.memory, NULL, NUMA_NO_NODE,
			     MEMBLOCK_KHO_SCRATCH, &start, &end, NULL) {
		ulong start_pfn = pageblock_start_pfn(PFN_DOWN(start));
		ulong end_pfn = pageblock_align(PFN_UP(end));
		ulong pfn;

		for (pfn = start_pfn; pfn < end_pfn; pfn += pageblock_nr_pages)
			set_pageblock_migratetype(pfn_to_page(pfn),
						  MIGRATE_CMA);
	}
}

void __init kho_memory_init(void)
{
	if (!kho_get_fdt()) {
		kho_reserve_scratch();
	} else {
		kho_mem_deserialize();
		kho_release_scratch();
	}
}

void __init kho_populate(phys_addr_t handover_fdt_phys,
			 phys_addr_t scratch_phys, u64 scratch_len)
{
	void *handover_fdt;
	struct kho_scratch *scratch;

	/* Determine the real size of the FDT */
	handover_fdt =
		early_memremap(handover_fdt_phys, sizeof(struct fdt_header));
	if (!handover_fdt) {
		pr_warn("setup: failed to memremap kexec FDT (0x%llx)\n",
			handover_fdt_phys);
		return;
	}

	if (fdt_check_header(handover_fdt)) {
		pr_warn("setup: kexec handover FDT is invalid (0x%llx)\n",
			handover_fdt_phys);
		early_memunmap(handover_fdt, sizeof(struct fdt_header));
		return;
	}

	kho_in.fdt_phys = handover_fdt_phys;
	early_memunmap(handover_fdt, sizeof(struct fdt_header));

	kho_in.kho_scratch_phys = scratch_phys;
	kho_scratch_cnt = scratch_len / sizeof(*kho_scratch);
	scratch = early_memremap(scratch_phys, scratch_len);
	if (!scratch) {
		pr_warn("setup: failed to memremap kexec scratch (0x%llx)\n",
			scratch_phys);
		return;
	}

	/*
	 * We pass a safe contiguous blocks of memory to use for early boot
	 * purporses from the previous kernel so that we can resize the
	 * memblock array as needed.
	 */
	for (int i = 0; i < kho_scratch_cnt; i++) {
		struct kho_scratch *area = &scratch[i];
		u64 size = area->size;

		memblock_add(area->addr, size);

		if (WARN_ON(memblock_mark_kho_scratch(area->addr, size))) {
			pr_err("Kexec failed to mark the scratch region. Disabling KHO revival.");
			kho_in.fdt_phys = 0;
			scratch = NULL;
			break;
		}
		pr_debug("Marked 0x%pa+0x%pa as scratch", &area->addr, &size);
	}

	early_memunmap(scratch, scratch_len);

	if (!scratch)
		return;

	memblock_reserve(scratch_phys, scratch_len);

	/*
	 * Now that we have a viable region of scratch memory, let's tell
	 * the memblocks allocator to only use that for any allocations.
	 * That way we ensure that nothing scribbles over in use data while
	 * we initialize the page tables which we will need to ingest all
	 * memory reservations from the previous kernel.
	 */
	memblock_set_kho_scratch_only();

	pr_info("setup: Found kexec handover data. Will skip init for some devices\n");
}
