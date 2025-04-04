// SPDX-License-Identifier: GPL-2.0-only
/*
 * kexec_handover.c - kexec handover metadata processing
 * Copyright (C) 2023 Alexander Graf <graf@amazon.com>
 * Copyright (C) 2025 Microsoft Corporation, Mike Rapoport <rppt@kernel.org>
 * Copyright (C) 2025 Google LLC, Changyuan Lyu <changyuanl@google.com>
 */

#define pr_fmt(fmt) "KHO: " fmt

#include <linux/bitops.h>
#include <linux/count_zeros.h>
#include <linux/libfdt.h>
#include <linux/memblock.h>
#include <linux/kexec_handover.h>
/*
 * KHO is tightly coupled with mm init and needs access to some of mm
 * internal APIs.
 */
#include "../mm/internal.h"

#define PROP_PRESERVED_FOLIO "folio"
#define PROP_PRESERVED_PHYS "addr"
#define PROP_PRESERVED_MEMORY_MAP "preserved-memory-map"

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

static int __kho_record_named_mem(void *fdt, char *name, char *prop, void *val,
				  int len)
{
	int err = 0;

	if (!name)
		return 0;

	err |= fdt_begin_node(fdt, name);
	err |= fdt_property(fdt, prop, val, len);
	err |= fdt_end_node(fdt);

	return err;
}

struct kho_serialization {
	void *fdt;
	struct kho_mem_track track;
};

/**
 * kho_preserve_folio - preserve a folio across KHO.
 * @ser: the `struct kho_serialization *` passed by KHO notifiers.
 * @name: if non NULL, the new kernel can retrieve the folio by @name.
 * @folio: folio to preserve
 *
 * Records that the entire folio is preserved across KHO. The order
 * will be preserved as well.
 *
 * Return: 0 on success, error code on failure
 */
int kho_preserve_folio(struct kho_serialization *ser, char *name,
		       struct folio *folio)
{
	unsigned long pfn = folio_pfn(folio);
	unsigned int end_pfn = folio_pfn(folio) + folio_nr_pages(folio);
	int err = 0;
	u64 phys = (u64)PFN_PHYS(pfn);

	err = __kho_preserve(&ser->track, pfn, end_pfn);
	if (err)
		return err;

	err = __kho_record_named_mem(ser->fdt, name, PROP_PRESERVED_FOLIO,
				     &phys, sizeof(phys));
	if (err)
		__kho_unpreserve(&ser->track, pfn, end_pfn);

	return err;
}
EXPORT_SYMBOL_GPL(kho_preserve_folio);

/**
 * kho_preserve_phys - preserve a physically contiguous range across KHO.
 * @ser: the `struct kho_serialization *` passed by KHO notifiers.
 * @name: if non NULL, the new kernel can retrieve the range by @name.
 * @phys: physical address of the range
 * @size: size of the range
 *
 * Records that the entire range from @phys to @phys + @size is preserved
 * across KHO.
 *
 * Return: 0 on success, error code on failure
 */
int kho_preserve_phys(struct kho_serialization *ser, char *name,
		      phys_addr_t phys, size_t size)
{
	unsigned long pfn = PHYS_PFN(phys);
	unsigned long end_pfn = PHYS_PFN(phys + size);
	int err = 0;
	u64 prop[2] = { phys, size };

	if (!PAGE_ALIGNED(phys) || !PAGE_ALIGNED(size))
		return -EINVAL;

	err = __kho_preserve(&ser->track, pfn, end_pfn);
	if (err)
		return err;

	err = __kho_record_named_mem(ser->fdt, name, PROP_PRESERVED_PHYS,
				     &prop[0], sizeof(prop));
	if (err)
		__kho_unpreserve(&ser->track, pfn, end_pfn);

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
