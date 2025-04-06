// SPDX-License-Identifier: GPL-2.0-only
/*
 * kexec_handover.c - kexec handover metadata processing
 * Copyright (C) 2023 Alexander Graf <graf@amazon.com>
 * Copyright (C) 2025 Microsoft Corporation, Mike Rapoport <rppt@kernel.org>
 * Copyright (C) 2025 Google LLC, Changyuan Lyu <changyuanl@google.com>
 */

#define pr_fmt(fmt) "KHO: " fmt

#include <linux/bitops.h>
#include <linux/cma.h>
#include <linux/count_zeros.h>
#include <linux/debugfs.h>
#include <linux/kexec.h>
#include <linux/kexec_handover.h>
#include <linux/libfdt.h>
#include <linux/list.h>
#include <linux/memblock.h>
#include <linux/notifier.h>
#include <linux/page-isolation.h>
/*
 * KHO is tightly coupled with mm init and needs access to some of mm
 * internal APIs.
 */
#include "../mm/internal.h"

#define PROP_PRESERVED_MEMORY_MAP "preserved-memory-map"

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

struct kho_serialization {
	void *fdt;
	struct list_head fdt_folios;
	struct dentry *fdt_folios_dir;
	struct kho_mem_track track;
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

static void __kho_unpreserve(struct kho_mem_track *track, unsigned long pfn,
			     unsigned long end_pfn)
{
	struct kho_mem_phys_bits *bits;
	struct kho_mem_phys *physxa;

	while (pfn < end_pfn) {
		unsigned int order =
			min(count_trailing_zeros(pfn), ilog2(end_pfn - pfn));
		unsigned long pfn_high = pfn >> order;

		physxa = xa_load(&track->orders, order);
		if (!physxa)
			continue;

		bits = xa_load(&physxa->phys_bits, pfn_high / PRESERVE_BITS);
		if (!bits)
			continue;

		clear_bit(pfn_high % PRESERVE_BITS, bits->preserve);

		pfn += 1 << order;
	}
}

static int __kho_preserve(struct kho_mem_track *track, unsigned long pfn,
			  unsigned long end_pfn)
{
	struct kho_mem_phys_bits *bits;
	struct kho_mem_phys *physxa;
	unsigned long failed_pfn = 0;
	int err = 0;
	const unsigned long start_pfn = pfn;

	might_sleep();

	while (pfn < end_pfn) {
		unsigned int order =
			min(count_trailing_zeros(pfn), ilog2(end_pfn - pfn));
		unsigned long pfn_high = pfn >> order;

		physxa = xa_load_or_alloc(&track->orders, order,
					  sizeof(*physxa));
		if (IS_ERR(physxa)) {
			err = PTR_ERR(physxa);
			failed_pfn = pfn;
			break;
		}

		bits = xa_load_or_alloc(&physxa->phys_bits,
					pfn_high / PRESERVE_BITS,
					sizeof(*bits));
		if (IS_ERR(bits)) {
			err = PTR_ERR(bits);
			failed_pfn = pfn;
			break;
		}

		set_bit(pfn_high % PRESERVE_BITS, bits->preserve);

		pfn += 1 << order;
	}

	if (err)
		__kho_unpreserve(track, start_pfn, failed_pfn);

	return err;
}

/**
 * kho_preserve_folio - preserve a folio across KHO.
 * @ser: the ``struct kho_serialization *`` passed by KHO notifiers.
 * @folio: folio to preserve
 *
 * Instructs KHO to preserve the whole folio across KHO. The order
 * will be preserved as well.
 *
 * Return: 0 on success, error code on failure
 */
int kho_preserve_folio(struct kho_serialization *ser, struct folio *folio)
{
	unsigned long pfn = folio_pfn(folio);
	unsigned int end_pfn = folio_pfn(folio) + folio_nr_pages(folio);

	return __kho_preserve(&ser->track, pfn, end_pfn);
}
EXPORT_SYMBOL_GPL(kho_preserve_folio);

/**
 * kho_preserve_phys - preserve a physically contiguous range across KHO.
 * @ser: the ``struct kho_serialization *`` passed by KHO notifiers.
 * @phys: physical address of the range
 * @size: size of the range
 *
 * Instructs KHO to preserve the memory range from @phys to @phys + @size
 * across kexec.
 *
 * Return: 0 on success, error code on failure
 */
int kho_preserve_phys(struct kho_serialization *ser, phys_addr_t phys,
		      size_t size)
{
	unsigned long pfn = PHYS_PFN(phys);
	unsigned long end_pfn = PHYS_PFN(phys + size);

	if (!PAGE_ALIGNED(phys) || !PAGE_ALIGNED(size))
		return -EINVAL;

	return __kho_preserve(&ser->track, pfn, end_pfn);
}
EXPORT_SYMBOL_GPL(kho_preserve_phys);

static const void *kho_folio_type_prop(enum kho_folio_t type)
{
	switch (type) {
	case KHO_FOLIO:
		return "folio";
	case KHO_FOLIO_FDT:
		return "fdt";
	default:
		return ERR_PTR(-EINVAL);
	}
}

static int kho_out_add_fdt_folio(struct kho_serialization *ser,
				 const char *name, struct folio *folio);

/**
 * kho_record_folio - record a preserved folio in KHO root FDT.
 * @ser: the ``struct kho_serialization *`` passed by KHO notifiers.
 * @name: name of the folio.
 * @folio: the struct folio to be recorded.
 * @type: the type of data stored in the folio.
 *
 * Creates a new child node named @name in KHO root FDT and records
 * the physical address of @folio. @folio must also be preserved
 * by kho_preserve_folio(). The folio physical address can be retrieved
 * using @name by the new kernel after kexec.
 *
 * If @type is KHO_FOLIO_FDT, a debugfs blob entry is also created at
 * ``/sys/kernel/debug/kho/out/fdt_folios/@name``.
 *
 * This function is intended for KHO users to record the folio
 * containing their own root FDT blob. Every KHO user only
 * calls this function once.
 *
 * Return: 0 on success, error code on failure
 */
int kho_record_folio(struct kho_serialization *ser, char *name,
		     struct folio *folio, enum kho_folio_t type)
{
	int err = 0;
	const char *prop = kho_folio_type_prop(type);
	u64 phys = (u64)PFN_PHYS(folio_pfn(folio));

	if (IS_ERR(prop))
		return PTR_ERR(prop);

	err |= fdt_begin_node(ser->fdt, name);
	err |= fdt_property(ser->fdt, prop, &phys, sizeof(phys));
	err |= fdt_end_node(ser->fdt);

	if (type == KHO_FOLIO_FDT && !err)
		err = kho_out_add_fdt_folio(ser, name, folio);

	return err;
}
EXPORT_SYMBOL_GPL(kho_record_folio);

/* almost as free_reserved_page(), just don't free the page */
static void kho_restore_page(struct page *page)
{
	ClearPageReserved(page);
	init_page_count(page);
	adjust_managed_page_count(page, 1);
}

/**
 * kho_restore_folio - recreates the folio from the preserved memory.
 * @phys: physical address of the folio.
 *
 * Return: pointer to the struct folio on success, NULL on failure.
 */
struct folio *kho_restore_folio(phys_addr_t phys)
{
	struct page *page = pfn_to_online_page(PHYS_PFN(phys));
	unsigned long order = page->private;

	if (!page)
		return NULL;

	order = page->private;
	if (order) {
		if (order > MAX_PAGE_ORDER)
			return NULL;

		prep_compound_page(page, order);
	} else {
		kho_restore_page(page);
	}

	return page_folio(page);
}
EXPORT_SYMBOL_GPL(kho_restore_folio);

/* Serialize and deserialize struct kho_mem_phys across kexec
 *
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

static struct khoser_mem_chunk *kho_mem_serialize(struct kho_mem_track *track)
{
	struct khoser_mem_chunk *first_chunk = NULL;
	struct khoser_mem_chunk *chunk = NULL;
	struct kho_mem_phys *physxa;
	unsigned long order;

	xa_for_each(&track->orders, order, physxa) {
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

static void __init kho_mem_deserialize(const void *fdt)
{
	struct khoser_mem_chunk *chunk;
	const phys_addr_t *mem;
	int len;

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

struct fdt_folio {
	struct list_head list;
	struct debugfs_blob_wrapper wrapper;
	struct dentry *file;
};

static int kho_add_fdt_folio(struct list_head *list, struct dentry *dir,
			     const char *name, void *fdt)
{
	struct fdt_folio *ff;
	struct dentry *file;

	ff = kmalloc(sizeof(*ff), GFP_KERNEL);
	if (!ff)
		return -ENOMEM;

	ff->wrapper.data = fdt;
	ff->wrapper.size = fdt_totalsize(fdt);

	file = debugfs_create_blob(name, 0400, dir, &ff->wrapper);
	if (IS_ERR(file)) {
		kfree(ff);
		return PTR_ERR(file);
	}

	ff->file = file;
	list_add(&ff->list, list);

	return 0;
}

struct kho_out {
	struct blocking_notifier_head chain_head;

	struct debugfs_blob_wrapper fdt_wrapper;
	struct dentry *fdt_file;
	struct dentry *dir;

	struct mutex lock; /* protects KHO FDT finalization */

	/* First chunk of serialized preserved memory map */
	struct khoser_mem_chunk *preserved_mem_map;
	struct kho_serialization ser;
	bool finalized;
};

static struct kho_out kho_out = {
	.chain_head = BLOCKING_NOTIFIER_INIT(kho_out.chain_head),
	.lock = __MUTEX_INITIALIZER(kho_out.lock),
	.ser = {
		.track = {
			.orders = XARRAY_INIT(kho_out.ser.track.orders, 0),
		},
		.fdt_folios = LIST_HEAD_INIT(kho_out.ser.fdt_folios),
	},
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

/* Handling for debug/kho/out */

static struct dentry *debugfs_root;

static int kho_out_add_fdt_folio(struct kho_serialization *ser,
				 const char *name, struct folio *folio)
{
	void *fdt = page_to_virt(folio_page(folio, 0));

	return kho_add_fdt_folio(&ser->fdt_folios, ser->fdt_folios_dir, name,
				 fdt);
}

static int kho_out_update_debugfs_fdt(void)
{
	int err = 0;
	void *fdt = kho_out.ser.fdt;

	if (kho_out.finalized) {
		kho_out.fdt_wrapper.data = fdt;
		kho_out.fdt_wrapper.size = fdt_totalsize(fdt);
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
	struct fdt_folio *ff, *tmp;

	memset(kho_out.ser.fdt, 0, PAGE_SIZE);

	xa_for_each(&kho_out.ser.track.orders, order, physxa) {
		struct kho_mem_phys_bits *bits;
		unsigned long phys;

		xa_for_each(&physxa->phys_bits, phys, bits)
			kfree(bits);

		xa_destroy(&physxa->phys_bits);
		kfree(physxa);
	}
	xa_destroy(&kho_out.ser.track.orders);

	if (kho_out.preserved_mem_map) {
		kho_mem_ser_free(kho_out.preserved_mem_map);
		kho_out.preserved_mem_map = NULL;
	}

	list_for_each_entry_safe(ff, tmp, &kho_out.ser.fdt_folios, list) {
		debugfs_remove(ff->file);
		list_del(&ff->list);
		kfree(ff);
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
	void *fdt = kho_out.ser.fdt;

	err |= fdt_create(fdt, PAGE_SIZE);
	err |= fdt_finish_reservemap(fdt);
	err |= fdt_begin_node(fdt, "");
	err |= fdt_property_string(fdt, "compatible", "kho-v1");
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

	err = kho_preserve_phys(&kho_out.ser, virt_to_phys(fdt), PAGE_SIZE);
	if (err)
		goto abort;

	err = blocking_notifier_call_chain(&kho_out.chain_head,
					   KEXEC_KHO_FINALIZE, &kho_out.ser);
	err = notifier_to_errno(err);
	if (err)
		goto abort;

	kho_out.preserved_mem_map = kho_mem_serialize(&kho_out.ser.track);
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
	struct dentry *dir, *f, *ff_dir;

	dir = debugfs_create_dir("out", debugfs_root);
	if (IS_ERR(dir))
		return -ENOMEM;

	ff_dir = debugfs_create_dir("fdt_folios", dir);
	if (IS_ERR(ff_dir))
		goto err_rmdir;

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
	kho_out.ser.fdt_folios_dir = ff_dir;
	return 0;

err_rmdir:
	debugfs_remove_recursive(dir);
	return -ENOENT;
}

struct kho_in {
	struct debugfs_blob_wrapper fdt_wrapper;
	struct dentry *dir;
	phys_addr_t fdt_phys;
	phys_addr_t scratch_phys;
	struct list_head fdt_folios;
};

static struct kho_in kho_in = {
	.fdt_folios = LIST_HEAD_INIT(kho_in.fdt_folios),
};

static const void *kho_get_fdt(void)
{
	return kho_in.fdt_phys ? phys_to_virt(kho_in.fdt_phys) : NULL;
}

/**
 * kho_retrieve_folio - retrieve a preserved folio by its name.
 * @name: the name of the folio passed to kho_record_folio().
 * @type: the @type passed to kho_record_folio().
 * @phys: if found, the physical address of the folio is stored in @phys.
 *
 * Retrieve a preserved folio named @name and store its physical
 * address in @phys.
 *
 * Return: 0 on success, error code on failure
 */
int kho_retrieve_folio(const char *name, enum kho_folio_t type,
		       phys_addr_t *phys)
{
	const void *fdt = kho_get_fdt();
	const void *prop = kho_folio_type_prop(type);
	const u64 *val;
	int offset, len;

	if (!fdt)
		return -ENOENT;

	if (!phys)
		return -EINVAL;

	if (IS_ERR(prop))
		return PTR_ERR(prop);

	offset = fdt_subnode_offset(fdt, 0, name);
	if (offset < 0)
		return -ENOENT;

	val = fdt_getprop(fdt, offset, prop, &len);
	if (!val || len != sizeof(*val))
		return -EINVAL;

	*phys = *val;
	if (!PAGE_ALIGNED(*phys))
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_GPL(kho_retrieve_folio);

/* Handling for debugfs/kho/in */

static __init int kho_in_debugfs_init(const void *fdt)
{
	struct dentry *file, *ff_dir;
	int err, child;

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

	ff_dir = debugfs_create_dir("fdt_folios", kho_in.dir);
	if (IS_ERR(ff_dir)) {
		err = PTR_ERR(file);
		goto err_rmdir;
	}

	fdt_for_each_subnode(child, fdt, 0) {
		int len = 0;
		const char *name = fdt_get_name(fdt, child, NULL);
		const u64 *fdt_phys;

		fdt_phys = fdt_getprop(fdt, child, "fdt", &len);
		if (!fdt_phys)
			continue;
		if (len != sizeof(*fdt_phys)) {
			pr_warn("node `%s`'s prop fdt has invalid length: %d.\n",
				name, len);
			continue;
		}
		err = kho_add_fdt_folio(&kho_in.fdt_folios, ff_dir, name,
					phys_to_virt(*fdt_phys));
		if (err) {
			pr_warn("failed to add fdt `%s` to debugfs: %d\n", name,
				err);
			continue;
		}
	}

	return 0;

err_rmdir:
	debugfs_remove_recursive(kho_in.dir);
	return err;
}

static __init int kho_init(void)
{
	int err = 0;
	const void *fdt = kho_get_fdt();

	if (!kho_enable)
		return 0;

	kho_out.ser.fdt = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!kho_out.ser.fdt) {
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
			pr_err("failed exposing handover FDT in debugfs: %d\n",
			       err);

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
	kfree(kho_out.ser.fdt);
	kho_out.ser.fdt = NULL;
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
	if (kho_in.scratch_phys) {
		kho_scratch = phys_to_virt(kho_in.scratch_phys);
		kho_release_scratch();

		kho_mem_deserialize(kho_get_fdt());
	} else {
		kho_reserve_scratch();
	}
}

void __init kho_populate(phys_addr_t fdt_phys, phys_addr_t scratch_phys,
			 u64 scratch_len)
{
	void *fdt = NULL;
	struct kho_scratch *scratch = NULL;
	int err = 0;
	unsigned int scratch_cnt = scratch_len / sizeof(*kho_scratch);

	/* Validate the input FDT */
	fdt = early_memremap(fdt_phys, sizeof(struct fdt_header));
	if (!fdt) {
		pr_warn("setup: failed to memremap FDT (0x%llx)\n", fdt_phys);
		err = -EFAULT;
		goto out;
	}
	err = fdt_check_header(fdt);
	if (fdt_check_header(fdt)) {
		pr_warn("setup: handover FDT (0x%llx) is invalid: %d\n",
			fdt_phys, err);
		err = -EINVAL;
		goto out;
	}

	scratch = early_memremap(scratch_phys, scratch_len);
	if (!scratch) {
		pr_warn("setup: failed to memremap scratch (phys=0x%llx, len=%lld)\n",
			scratch_phys, scratch_len);
		err = -EFAULT;
		goto out;
	}

	/*
	 * We pass a safe contiguous blocks of memory to use for early boot
	 * purporses from the previous kernel so that we can resize the
	 * memblock array as needed.
	 */
	for (int i = 0; i < scratch_cnt; i++) {
		struct kho_scratch *area = &scratch[i];
		u64 size = area->size;

		memblock_add(area->addr, size);
		err = memblock_mark_kho_scratch(area->addr, size);
		if (WARN_ON(err)) {
			pr_err("failed to mark the scratch region 0x%pa+0x%pa: %d",
			       &area->addr, &size, err);
			goto out;
		}
		pr_debug("Marked 0x%pa+0x%pa as scratch", &area->addr, &size);
	}

	memblock_reserve(scratch_phys, scratch_len);

	/*
	 * Now that we have a viable region of scratch memory, let's tell
	 * the memblocks allocator to only use that for any allocations.
	 * That way we ensure that nothing scribbles over in use data while
	 * we initialize the page tables which we will need to ingest all
	 * memory reservations from the previous kernel.
	 */
	memblock_set_kho_scratch_only();

out:
	if (fdt)
		early_memunmap(fdt, sizeof(struct fdt_header));
	if (scratch)
		early_memunmap(scratch, scratch_len);
	if (err) {
		pr_err("disabling KHO revival: %d\n", err);
		return;
	}

	pr_info("found kexec handover data. Will skip init for some devices\n");

	kho_in.fdt_phys = fdt_phys;
	kho_in.scratch_phys = scratch_phys;
	kho_scratch_cnt = scratch_cnt;
}
