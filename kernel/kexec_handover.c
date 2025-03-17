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
#define KHO_PRESERVED_FOLIO_MAP_PROP "kho,preserved-folio-map"

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

	/**
	 * Physical address of the first struct khoser_mem_chunk containing
	 * serialized data from struct kho_mem_track.
	 */
	phys_addr_t first_chunk_phys;

	struct kho_fdt root_fdt;
	bool finalized;
};

static struct kho_out kho_out = {
	.chain_head = BLOCKING_NOTIFIER_INIT(kho_out.chain_head),
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
 * kho_get_node - retrieve a node saved in KHO FDT.
 * @parent: the parent node to look up for.
 * @name: the name of the node to look for.
 * @child: if a node named @name is found under @parent, it is stored in @child.
 *
 * If @parent is NULL, this function looks up for @name under KHO root node.
 *
 * Return: 0 on success, and @child is populated, error code on failure.
 */
int kho_retrieve_node(const struct kho_node *parent, const char *name,
		      struct kho_node *child)
{
	const void *parent_fdt = kho_get_fdt();
	const phys_addr_t *recursive_fdt_phys;
	int parent_offset = 0, size;
	struct folio *fdt_folio;

	if (!parent_fdt)
		return -ENOENT;

	if (!child)
		return -EINVAL;

	if (parent) {
		parent_fdt = parent->fdt;
		parent_offset = parent->offset;
	}

	child->offset = fdt_subnode_offset(parent_fdt, parent_offset, name);
	if (child->offset < 0)
		return -ENOENT;

	/* Dereference a recursive FDT if necessary */
	recursive_fdt_phys = fdt_getprop(parent_fdt, child->offset,
					 KHO_RECURSIVE_FDT_PROP, &size);
	if (!recursive_fdt_phys || size != sizeof(phys_addr_t)) {
		/* Not a recursive FDT */
		child->fdt = parent_fdt;
		return 0;
	}

	fdt_folio = kho_restore_folio(*recursive_fdt_phys);
	if (!fdt_folio)
		return -ENOENT;

	child->fdt = page_to_virt(folio_page(fdt_folio, 0));
	child->offset = 0;

	return 0;
}
EXPORT_SYMBOL_GPL(kho_retrieve_node);

/**
 * kho_get_prop - retrieve the property data stored in the KHO tree.
 * @node: the node to look up for.
 * @key: the key of the property.
 * @size: a pointer to store the size of the data in bytes.
 *
 * Return: pointer to the data, and data size is stored in @size, or NULL on
 * failure.
 */
const void *kho_retrieve_prop(const struct kho_node *node, const char *key,
			      u32 *size)
{
	int offset = 0;
	u32 s;
	const void *fdt = node->fdt;

	if (node)
		offset = node->offset;

	if (!size)
		size = &s;

	return fdt_getprop(fdt, offset, key, size);
}
EXPORT_SYMBOL_GPL(kho_retrieve_prop);

/**
 * kho_node_check_compatible - check a node's compatible property.
 * @node: the node to check.
 * @compatible: the compatible stirng.
 *
 * Wrapper of fdt_node_check_compatible().
 *
 * Return: 0 if @compatible is in the node's "compatible" list, or
 * error code on failure.
 */
int kho_node_check_compatible(const struct kho_node *node,
			      const char *compatible)
{
	return fdt_node_check_compatible(node->fdt, node->offset, compatible);
}
EXPORT_SYMBOL_GPL(kho_node_check_compatible);

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

static struct kho_mem_track kho_mem_track;

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
	unsigned long pfn_hi = pfn >> order;

	physxa = xa_load(&tracker->orders, order);
	if (!physxa)
		return;

	bits = xa_load(&physxa->phys_bits, pfn_hi / PRESERVE_BITS);
	if (!bits)
		return;

	clear_bit(pfn_hi % PRESERVE_BITS, bits->preserve);
}

static int __kho_preserve(struct kho_mem_track *tracker, unsigned long pfn,
			  unsigned int order)
{
	struct kho_mem_phys_bits *bits;
	struct kho_mem_phys *physxa;
	unsigned long pfn_hi = pfn >> order;

	might_sleep();

	physxa = xa_load_or_alloc(&tracker->orders, order, sizeof(*physxa));
	if (IS_ERR(physxa))
		return PTR_ERR(physxa);

	bits = xa_load_or_alloc(&physxa->phys_bits, pfn_hi / PRESERVE_BITS,
				sizeof(*bits));
	if (IS_ERR(bits))
		return PTR_ERR(bits);

	set_bit(pfn_hi % PRESERVE_BITS, bits->preserve);

	return 0;
}

/**
 * kho_preserve_folio - preserve a folio across KHO.
 * @folio: folio to preserve
 *
 * Records that the entire folio is preserved across KHO. The order
 * will be preserved as well.
 *
 * Return: 0 on success, error code on failure
 */
int kho_preserve_folio(struct kho_mem_track *tracker, struct folio *folio)
{
	unsigned long pfn = folio_pfn(folio);
	unsigned int order = folio_order(folio);

	return __kho_preserve(tracker, pfn, order);
}
EXPORT_SYMBOL_GPL(kho_preserve_folio);

/**
 * kho_unpreserve_folio - unpreserve a folio
 * @folio: folio to unpreserve
 *
 * Remove the record of a folio previously preserved by kho_preserve_folio().
 *
 * Return: 0 on success, error code on failure
 */
void kho_unpreserve_folio(struct kho_mem_track *tracker, struct folio *folio)
{
	unsigned long pfn = folio_pfn(folio);
	unsigned int order = folio_order(folio);

	return __kho_unpreserve(tracker, pfn, order);
}
EXPORT_SYMBOL_GPL(kho_unpreserve_folio);

int kho_preserve_fdt(struct kho_mem_track *tracker, struct kho_fdt *fdt)
{
	return kho_preserve_folio(tracker, virt_to_folio(fdt->fdt));
}

void kho_unpreserve_fdt(struct kho_mem_track *tracker, struct kho_fdt *fdt)
{
	kho_unpreserve_folio(tracker, virt_to_folio(fdt->fdt));
}

/**
 * kho_preserve_phys - preserve a physically contiguous range across KHO.
 * @phys: physical address of the range
 * @size: size of the range
 *
 * Records that the entire range from @phys to @phys + @size is preserved
 * across KHO.
 *
 * Return: 0 on success, error code on failure
 */
int kho_preserve_phys(struct kho_mem_track *tracker, phys_addr_t phys,
		      size_t size)
{
	unsigned long pfn = PHYS_PFN(phys), end_pfn = PHYS_PFN(phys + size);
	unsigned int order = ilog2(end_pfn - pfn);
	unsigned long failed_pfn;
	int err = 0;

	for (; pfn < end_pfn;
	     pfn += (1 << order), order = ilog2(end_pfn - pfn)) {
		err = __kho_preserve(tracker, pfn, order);
		if (err) {
			failed_pfn = pfn;
			break;
		}
	}

	if (err)
		for (pfn = PHYS_PFN(phys); pfn < failed_pfn;
		     pfn += (1 << order), order = ilog2(end_pfn - pfn))
			__kho_unpreserve(tracker, pfn, order);

	return err;
}
EXPORT_SYMBOL_GPL(kho_preserve_phys);

/**
 * kho_unpreserve_phys - unpreserve a physically contiguous range
 * @phys: physical address of the range
 * @size: size of the range
 *
 * Remove the record of a range previously preserved by kho_preserve_phys().
 *
 * Return: 0 on success, error code on failure
 */
int kho_unpreserve_phys(struct kho_mem_track *tracker, phys_addr_t phys,
			size_t size)
{
	unsigned long pfn = PHYS_PFN(phys), end_pfn = PHYS_PFN(phys + size);
	unsigned int order = ilog2(end_pfn - pfn);
	int err = 0;

	for (; pfn < end_pfn; pfn += (1 << order), order = ilog2(end_pfn - pfn))
		__kho_unpreserve(tracker, pfn, order);

	return err;
}
EXPORT_SYMBOL_GPL(kho_unpreserve_phys);

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

	chunk = (struct khoser_mem_chunk *)get_zeroed_page(GFP_KERNEL);
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
		unsigned long chunk_page = (unsigned long)chunk;

		chunk = KHOSER_LOAD_PTR(chunk->hdr.next);
		free_page(chunk_page);
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
	struct kho_mem_track *tracker = &kho_mem_track;
	struct khoser_mem_chunk *first_chunk = NULL;
	struct khoser_mem_chunk *chunk = NULL;
	struct kho_mem_phys *physxa;
	unsigned long order;

	xa_for_each(&tracker->orders, order, physxa) {
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

	mem = fdt_getprop(fdt, 0, KHO_PRESERVED_FOLIO_MAP_PROP, &len);

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

	if (kho_out.first_chunk_phys) {
		struct khoser_mem_chunk *first_chunk =
			phys_to_virt(kho_out.first_chunk_phys);

		kho_mem_ser_free(first_chunk);
		kho_out.first_chunk_phys = 0;
	}

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

	/* Reserve the kho,preserved-folio-map property in the root FDT */
	err = fdt_property_placeholder(root->fdt, KHO_PRESERVED_FOLIO_MAP_PROP,
				       sizeof(phys_addr_t),
				       (void **)&first_chunk_phys);
	if (err)
		goto unfreeze;

	struct kho_serialization ser = {
		.fdt = &kho_out.root_fdt,
		.tracker = &kho_mem_track,
	};
	err = blocking_notifier_call_chain(&kho_out.chain_head,
					   KEXEC_KHO_FINALIZE, &ser);
	err = notifier_to_errno(err);
	if (err)
		goto unfreeze;

	first_chunk = kho_mem_serialize();
	if (IS_ERR(first_chunk)) {
		err = PTR_ERR(first_chunk);
		goto unfreeze;
	}
	*first_chunk_phys = first_chunk ? virt_to_phys(first_chunk) : 0;
	kho_out.first_chunk_phys = *first_chunk_phys;

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
	int err;
	const void *fdt = kho_get_fdt();

	if (!kho_enable)
		return 0;

	err = kho_new_fdt(&kho_out.root_fdt);
	if (err) {
		pr_err("failed to init KHO root FDT: %d\n", err);
		goto err_free_scratch;
	}
	err = kho_preserve_fdt(&kho_mem_track, &kho_out.root_fdt);
	if (err) {
		pr_err("failed to preserve KHO root FDT: %d\n", err);
		goto err_free_fdt;
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

	pr_notice(
		"scratch areas: lowmem: %lluMiB global: %lluMiB pernode: %lldMiB\n",
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
