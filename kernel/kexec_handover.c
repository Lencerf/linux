// SPDX-License-Identifier: GPL-2.0-only
/*
 * kexec_handover.c - kexec handover metadata processing
 * Copyright (C) 2023 Alexander Graf <graf@amazon.com>
 * Copyright (C) 2025 Microsoft Corporation, Mike Rapoport <rppt@kernel.org>
 * Copyright (C) 2024 Google LLC
 */

#define pr_fmt(fmt) "KHO: " fmt

#include <linux/cma.h>
#include <linux/kexec.h>
#include <linux/sysfs.h>
#include <linux/libfdt.h>
#include <linux/memblock.h>
#include <linux/notifier.h>
#include <linux/kexec_handover.h>
#include <linux/page-isolation.h>
#include <linux/xxhash.h>

#include "kexec_internal.h"

static bool kho_enable __ro_after_init;

bool kho_is_enabled(void) {
	return kho_enable;
}
EXPORT_SYMBOL_GPL(kho_is_enabled);

static int __init kho_parse_enable(char *p)
{
	return kstrtobool(p, &kho_enable);
}
early_param("kho", kho_parse_enable);

/*
 * With KHO enabled, memory can become fragmented because KHO regions may
 * be anywhere in physical address space. The scratch regions give us a
 * safe zones that we will never see KHO allocations from. This is where we
 * can later safely load our new kexec images into and then use the scratch
 * area for early allocations that happen before page allocator is
 * initialized.
 */
static struct kho_mem *kho_scratch;
static unsigned int kho_scratch_cnt;

#define FDT_MAX SZ_16M

struct kho_out {
	struct blocking_notifier_head chain_head;
	struct kobject *kobj;

	struct mutex m; /* protects root */
	struct kho_node root;

	rwlock_t lock; /* protects fdt */
	void *fdt;
};

static struct kho_out kho_out = {
	.chain_head = BLOCKING_NOTIFIER_INIT(kho_out.chain_head),
	.m = __MUTEX_INITIALIZER(kho_out.m),
	.root = KHO_NODE_INIT,
	.lock = __RW_LOCK_UNLOCKED(kho_out.lock),
};

struct kho_in {
	struct kobject *kobj;
	phys_addr_t kho_scratch_phys;
	phys_addr_t handover_phys;
	u32 handover_len;
};

static struct kho_in kho_in;

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

const void *kho_get_fdt(void)
{
	if (!kho_in.handover_phys)
		return NULL;

	return __va(kho_in.handover_phys);
}
EXPORT_SYMBOL_GPL(kho_get_fdt);

static void kho_return_pfn(ulong pfn)
{
	struct page *page = pfn_to_online_page(pfn);

	if (WARN_ON(!page))
		return;
	__free_page(page);
}

/**
 * kho_return_mem - Notify the kernel that initially reserved memory is no
 * longer needed.
 * @mem: memory range that was preserved during kexec handover
 *
 * When the last consumer of a page returns their memory, kho returns the page
 * to the buddy allocator as free page.
 */
void kho_return_mem(const struct kho_mem *mem)
{
	unsigned long start_pfn, end_pfn, pfn;

	start_pfn = PFN_DOWN(mem->addr);
	end_pfn = PFN_UP(mem->addr + mem->size);

	for (pfn = start_pfn; pfn < end_pfn; pfn++)
		kho_return_pfn(pfn);
}
EXPORT_SYMBOL_GPL(kho_return_mem);

static int kho_claim_pfn(ulong pfn)
{
	struct page *page = pfn_to_online_page(pfn);

	if (!page)
		return -ENOMEM;

	/* almost as free_reserved_page(), just don't free the page */
	ClearPageReserved(page);
	init_page_count(page);
	adjust_managed_page_count(page, 1);

	return 0;
}

/**
 * kho_claim_mem - Notify the kernel that a handed over memory range is now
 * in use
 * @mem: memory range that was preserved during kexec handover
 *
 * A kernel subsystem preserved that range during handover and it is going
 * to reuse this range after kexec. The pages in the range are treated as
 * allocated, but not %PG_reserved.
 *
 * Return: virtual address of the preserved memory range
 */
void *kho_claim_mem(const struct kho_mem *mem)
{
	unsigned long start_pfn, end_pfn, pfn;
	void *va = __va(mem->addr);

	start_pfn = PFN_DOWN(mem->addr);
	end_pfn = PFN_UP(mem->addr + mem->size);

	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		int err = kho_claim_pfn(pfn);

		if (err)
			return NULL;
	}

	return va;
}
EXPORT_SYMBOL_GPL(kho_claim_mem);

int kho_fill_kimage(struct kimage *image)
{
	ssize_t scratch_size;
	int err = 0;

	if (!kho_enable)
		return 0;

	/* Allocate target memory for KHO FDT */
	struct kexec_buf fdt = {
		.image = image,
		.buffer = NULL,
		.bufsz = 0,
		.mem = KEXEC_BUF_MEM_UNKNOWN,
		.memsz = FDT_MAX,
		.buf_align = SZ_64K, /* Makes it easier to map */
		.buf_max = ULONG_MAX,
		.top_down = true,
	};
	err = kexec_add_buffer(&fdt);
	if (err) {
		pr_err("failed to reserved a segment for KHO FDT: %d\n", err);
		return err;
	}
	image->kho.fdt = &image->segment[image->nr_segments - 1];

	scratch_size = sizeof(*kho_scratch) * kho_scratch_cnt;
	struct kexec_buf scratch = {
		.image = image,
		.buffer = kho_scratch,
		.bufsz = scratch_size,
		.mem = KEXEC_BUF_MEM_UNKNOWN,
		.memsz = scratch_size,
		.buf_align = SZ_64K, /* Makes it easier to map */
		.buf_max = ULONG_MAX,
		.top_down = true,
	};
	err = kexec_add_buffer(&scratch);
	if (err)
		return err;
	image->kho.scratch = &image->segment[image->nr_segments - 1];

	return 0;
}

static int kho_walk_scratch(struct kexec_buf *kbuf,
			    int (*func)(struct resource *, void *))
{
	int ret = 0;
	int i;

	for (i = 0; i < kho_scratch_cnt; i++) {
		struct resource res = {
			.start = kho_scratch[i].addr,
			.end = kho_scratch[i].addr + kho_scratch[i].size - 1,
		};

		/* Try to fit the kimage into our KHO scratch region */
		ret = func(&res, kbuf);
		if (ret)
			break;
	}

	return ret;
}

int kho_locate_mem_hole(struct kexec_buf *kbuf,
			int (*func)(struct resource *, void *))
{
	int ret;

	if (!kho_enable || kbuf->image->type == KEXEC_TYPE_CRASH)
		return 1;

	ret = kho_walk_scratch(kbuf, func);

	return ret == 1 ? 0 : -EADDRNOTAVAIL;
}

/* Helper functions for KHO state tree */

struct kho_prop {
	struct hlist_node hlist;

	const char *key;
	const void *val;
	u32 size;
};

static unsigned long strhash(const char *s)
{
	return xxhash(s, strlen(s), 1120);
}

void kho_init_node(struct kho_node *node)
{
	hash_init(node->props);
	hash_init(node->nodes);
}
EXPORT_SYMBOL_GPL(kho_init_node);

/**
 * kho_add_node - add a child node to a parent node.
 * @parent: parent node to add to.
 * @name: name of the child node.
 * @child: child node.
 *
 * If @parent is NULL, @child is added to KHO state tree root node.
 *
 * @child must be a valid pointer through KHO FDT finalization.
 * @name is duplicated and thus can have a short lifttime.
 *
 * Return: 0 on success, or the following error,
 *  - -ENOENT: @parent is NULL but KHO is not enabled,
 *  - -ENOMEM: failed to duplicate @name,
 *  - -EBUSY: KHO FDT has been finalized,
 *  - -EEXIST: Another node of the same name has been added to the parent.
 */
int kho_add_node(struct kho_node *parent, const char *name, struct kho_node *child)
{
	unsigned long name_hash;
	int err = 0;
	struct kho_node *node;
	char *n;

	if (!parent) {
		if (kho_enable)
			parent = &kho_out.root;
		else
			return -ENOENT;
	}

	n = kstrdup(name, GFP_KERNEL);
	if (!n)
		return -ENOMEM;

	name_hash = strhash(n);

	read_lock(&kho_out.lock);

	if (kho_out.fdt) {
		err = -EBUSY;
		goto out;
	}

	if (parent == &kho_out.root)
		mutex_lock(&kho_out.m);

	hash_for_each_possible(parent->nodes, node, hlist, name_hash)
		if (!strcmp(node->name, n)) {
			err = -EEXIST;
			break;
		}
	if (err == 0) {
		child->name = n;
		hash_add(parent->nodes, &child->hlist, name_hash);
	}

	if (parent == &kho_out.root)
		mutex_unlock(&kho_out.m);

out:
	read_unlock(&kho_out.lock);

	if (err)
		kfree(n);

	return err;
}
EXPORT_SYMBOL_GPL(kho_add_node);

/**
 * kho_remove_node - remove a child node from a parent node.
 * @parent: parent node to look up for.
 * @name: name of the child node.
 *
 * If @parent is NULL, KHO state tree root node is looked up.
 *
 * Return: the pointer to the child node on success, or the following error pointer,
 *  - -ENOENT: @parent is NULL but KHO is not enabled, or no node named @name is found.
 *  - -EBUSY: KHO FDT has been finalized.
 */
struct kho_node *kho_remove_node(struct kho_node *parent, const char *name)
{
	struct kho_node *child, *ret = ERR_PTR(-ENOENT);
	unsigned long name_hash;

	if (!parent) {
		if (kho_enable)
			parent = &kho_out.root;
		else
			return ERR_PTR(-ENOENT);
	}

	name_hash = strhash(name);

	read_lock(&kho_out.lock);

	if (kho_out.fdt) {
		ret = ERR_PTR(-EBUSY);
		goto out;
	}

	if (parent == &kho_out.root)
		mutex_lock(&kho_out.m);

	hash_for_each_possible(parent->nodes, child, hlist, name_hash)
		if (!strcmp(child->name, name)) {
			ret = child;
			break;
		}

	if (!IS_ERR(ret)) {
		hash_del(&ret->hlist);
		kfree(ret->name);
		ret->name = NULL;
	}

	if (parent == &kho_out.root)
		mutex_unlock(&kho_out.m);

out:
	read_unlock(&kho_out.lock);

	return ret;
}
EXPORT_SYMBOL_GPL(kho_remove_node);

/**
 * kho_add_prop - add a property to a node.
 * @node: KHO node to add the property to.
 * @key: key of the property.
 * @val: pointer to the property value.
 * @size: size of the property value in bytes.
 *
 * @val and @key must be valid pointers through KHO FDT finalization.
 * Generally @key is a string literal with static lifetime.
 *
 * Return: 0 on success, or the following error,
 *  - -ENOMEM: failed to allocate memory,
 *  - -EBUSY: KHO FDT has been finalized,
 *  - -EEXIST: Another property of the same key exists,
 */
int kho_add_prop(struct kho_node *node, const char *key, const void *val, u32 size)
{
	unsigned long key_hash;
	int err = 0;
	struct kho_prop *prop, *p;

	key_hash = strhash(key);
	prop = kmalloc(sizeof(*prop), GFP_KERNEL);
	if (!prop)
		return -ENOMEM;

	prop->key = key;
	prop->val = val;
	prop->size = size;

	read_lock(&kho_out.lock);
	if (kho_out.fdt) {
		err = -EBUSY;
		goto out;
	}

	hash_for_each_possible(node->props, p, hlist, key_hash)
		if (!strcmp(p->key, key)) {
			err = -EEXIST;
			break;
		}
	if (!err)
		hash_add(node->props, &prop->hlist, key_hash);

out:
	read_unlock(&kho_out.lock);
	if (err)
		kfree(prop);
	return err;
}
EXPORT_SYMBOL_GPL(kho_add_prop);

/**
 * kho_add_string_prop - add a string property to a node.
 *
 * See kho_add_prop() for details.
 */
int kho_add_string_prop(struct kho_node *node, const char *key, const char *val)
{
	return kho_add_prop(node, key, val, strlen(val) + 1);
}
EXPORT_SYMBOL_GPL(kho_add_string_prop);

/**
 * kho_remove_prop - add a property from a node.
 * @node: KHO node to remove the property from.
 * @key: key of the property.
 * @size: if non-NULL, the property size is stored in it on success.
 *
 * Return: the pointer to the property value, or the following error,
 *  - -EBUSY: KHO FDT has been finalized,
 *  - -ENOENT: No property with @key is found.
 */
void *kho_remove_prop(struct kho_node *node, const char *key, u32 *size)
{
	struct kho_prop *p, *prop = NULL;
	unsigned long key_hash;
	void *ret = ERR_PTR(-ENOENT);

	key_hash = strhash(key);

	read_lock(&kho_out.lock);

	if (kho_out.fdt) {
		ret = ERR_PTR(-EBUSY);
		goto out;
	}

	hash_for_each_possible(node->props, p, hlist, key_hash)
		if (!strcmp(p->key, key)) {
			prop = p;
			break;
		}

	if (prop) {
		ret = (void *)prop->val;
		if (size)
			*size = prop->size;
		hash_del(&prop->hlist);
		kfree(prop);
	}

out:
	read_unlock(&kho_out.lock);
	return ret;
}
EXPORT_SYMBOL_GPL(kho_remove_prop);

static ssize_t fdt_read(struct file *file, struct kobject *kobj,
			const struct bin_attribute *attr, char *buf,
			loff_t pos, size_t count)
{
	read_lock(&kho_out.lock);
	memcpy(buf, attr->private + pos, count);
	read_unlock(&kho_out.lock);

	return count;
}

static struct bin_attribute bin_attr_fdt_out = __BIN_ATTR_ADMIN_RO(fdt, 0);

static int kho_out_update_sysfs_fdt(void)
{
	int err = 0;

	if (kho_out.fdt) {
		bin_attr_fdt_out.private = kho_out.fdt;
		bin_attr_fdt_out.size = fdt_totalsize(kho_out.fdt);
		err = sysfs_create_bin_file(kho_out.kobj, &bin_attr_fdt_out);
	} else {
		sysfs_remove_bin_file(kho_out.kobj, &bin_attr_fdt_out);
	}

	return err;
}

static int kho_unfreeze(void)
{
	int err;
	void *fdt;

	write_lock(&kho_out.lock);
	fdt = kho_out.fdt;
	kho_out.fdt = NULL;
	write_unlock(&kho_out.lock);

	if (fdt)
		kvfree(fdt);

	err = blocking_notifier_call_chain(&kho_out.chain_head, KEXEC_KHO_UNFREEZE, NULL);
	err = notifier_to_errno(err);

	return notifier_to_errno(err);
}

static int kho_flatten_tree(void *fdt)
{
	int iter, err = 0;
	struct kho_node *node, *sub_node;
	struct list_head *ele;
	struct kho_prop *prop;
	LIST_HEAD(stack);

	kho_out.root.visited = false;
	list_add(&kho_out.root.list, &stack);

	for (ele = stack.next; !list_is_head(ele, &stack); ele = stack.next) {
		node = list_entry(ele, struct kho_node, list);

		if (node->visited) {
			err = fdt_end_node(fdt);
			if (err)
				return err;
			list_del_init(ele);
			continue;
		}

		err = fdt_begin_node(fdt, node->name);
		if (err)
			return err;

		hash_for_each(node->props, iter, prop, hlist) {
			err = fdt_property(fdt, prop->key, prop->val, prop->size);
			if (err)
				return err;
		}

		hash_for_each(node->nodes, iter, sub_node, hlist) {
			sub_node->visited = false;
			list_add(&sub_node->list, &stack);
		}

		node->visited = true;
	}

	return 0;
}

static int kho_convert_tree(void *buffer, int size)
{
	void *fdt = buffer;
	int err = 0;

	err = fdt_create(fdt, size);
	if (err)
		goto out;

	err = fdt_finish_reservemap(fdt);
	if (err)
		goto out;

	err = kho_flatten_tree(fdt);
	if (err)
		goto out;

	err = fdt_finish(fdt);
	if (err)
		goto out;

	err = fdt_check_header(fdt);
	if (err)
		goto out;

out:
	if (err) {
		pr_err("failed to flatten state tree: %d\n", err);
		return -EINVAL;
	}
	return 0;
}

static int kho_finalize(void)
{
	int err = 0;
	void *fdt;

	fdt = kvmalloc(FDT_MAX, GFP_KERNEL);
	if (!fdt)
		return -ENOMEM;

	kho_out.root.name = "";
	err = kho_add_string_prop(&kho_out.root, "compatible", "kho-v1");
	if (err && err != -EEXIST) {
		kvfree(fdt);
		return err;
	}

	err = blocking_notifier_call_chain(&kho_out.chain_head, KEXEC_KHO_FINALIZE, NULL);
	err = notifier_to_errno(err);
	if (err)
		goto unfreeze;

	write_lock(&kho_out.lock);
	kho_out.fdt = fdt;
	write_unlock(&kho_out.lock);

	err = kho_convert_tree(fdt, FDT_MAX);

unfreeze:
	if (err) {
		int abort_err;

		pr_err("Failed to convert KHO state tree: %d\n", err);

		abort_err = kho_unfreeze();
		if (abort_err)
			pr_err("Failed to abort KHO state tree: %d\n", abort_err);
	}

	return err;
}

int kho_copy_fdt(struct kimage *image)
{
	int err = 0;
	void *fdt;

	if (!kho_enable || !image->file_mode)
		return 0;

	if (!kho_out.fdt) {
		err = kho_finalize();
		kho_out_update_sysfs_fdt();
		if (err)
			return err;
	}

	fdt = kimage_map_segment(image, image->kho.fdt->mem, PAGE_ALIGN(FDT_MAX));
	if (!fdt) {
		pr_err("failed to vmap fdt ksegment in kimage\n");
		return -ENOMEM;
	}

	memcpy(fdt, kho_out.fdt, fdt_totalsize(kho_out.fdt));

	kimage_unmap_segment(fdt);

	return 0;
}

/* Handling for /sys/kernel/kho */

#define KHO_ATTR_RO(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_RO_MODE(_name, 0400)
#define KHO_ATTR_RW(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_RW_MODE(_name, 0600)

static ssize_t finalize_store(struct kobject *dev, struct kobj_attribute *attr,
			      const char *buf, size_t size)
{
	bool val = false;
	int ret = 0;

	if (!kho_enable)
		return -EOPNOTSUPP;

	if (!kho_scratch_cnt)
		return -ENOMEM;

	if (kstrtobool(buf, &val) < 0)
		return -EINVAL;

	if (!kexec_trylock())
		return -EBUSY;

	if (val == !!kho_out.fdt) {
		if (kho_out.fdt)
			ret = -EEXIST;
		else
			ret = -ENOENT;
		goto unlock;
	}

	if (val)
		ret = kho_finalize();
	else
		ret = kho_unfreeze();

	if (ret)
		goto unlock;

	ret = kho_out_update_sysfs_fdt();

unlock:
	kexec_unlock();
	return ret == 0 ? size : ret;
}

static ssize_t finalize_show(struct kobject *dev, struct kobj_attribute *attr,
			     char *buf)
{
	ssize_t ret;
	unsigned int finalized = 0;

	read_lock(&kho_out.lock);
	finalized = !!kho_out.fdt;
	read_unlock(&kho_out.lock);

	ret = sysfs_emit(buf, "%d\n", finalized);
	return ret;
}
KHO_ATTR_RW(finalize);

static ssize_t scratch_len_show(struct kobject *dev, struct kobj_attribute *attr,
				char *buf)
{
	ssize_t count = 0;

	for (int i = 0; i < kho_scratch_cnt; i++)
		count += sysfs_emit_at(buf, count, "0x%llx\n", kho_scratch[i].size);

	return count;
}
KHO_ATTR_RO(scratch_len);

static ssize_t scratch_phys_show(struct kobject *dev, struct kobj_attribute *attr,
				 char *buf)
{
	ssize_t count = 0;

	for (int i = 0; i < kho_scratch_cnt; i++)
		count += sysfs_emit_at(buf, count, "0x%llx\n", kho_scratch[i].addr);

	return count;
}
KHO_ATTR_RO(scratch_phys);

static const struct attribute *kho_out_attrs[] = {
	&finalize_attr.attr,
	&scratch_phys_attr.attr,
	&scratch_len_attr.attr,
	NULL,
};

/* Handling for /sys/firmware/kho */
static struct bin_attribute bin_attr_fdt_in = __BIN_ATTR(fdt, 0400, sysfs_bin_attr_simple_read,
							 NULL, 0);

static __init int kho_in_sysfs_init(const void *fdt)
{
	int err;

	kho_in.kobj = kobject_create_and_add("kho", firmware_kobj);
	if (!kho_in.kobj)
		return -ENOMEM;

	bin_attr_fdt_in.size = fdt_totalsize(fdt);
	bin_attr_fdt_in.private = (void *)fdt;
	err = sysfs_create_bin_file(kho_in.kobj, &bin_attr_fdt_in);
	if (err)
		goto err_put_kobj;

	return 0;

err_put_kobj:
	kobject_put(kho_in.kobj);
	return err;
}

static __init int kho_out_sysfs_init(void)
{
	int err;

	kho_out.kobj = kobject_create_and_add("kho", kernel_kobj);
	if (!kho_out.kobj)
		return -ENOMEM;

	err = sysfs_create_files(kho_out.kobj, kho_out_attrs);
	if (err)
		goto err_put_kobj;

	return 0;

err_put_kobj:
	kobject_put(kho_out.kobj);
	return err;
}

static __init int kho_init(void)
{
	const void *fdt = kho_get_fdt();
	int err;

	if (!kho_enable)
		return -EINVAL;

	err = kho_out_sysfs_init();
	if (err)
		return err;

	if (fdt) {
		err = kho_in_sysfs_init(fdt);
		/*
		 * Failure to create /sys/firmware/kho/fdt does not prevent
		 * reviving state from KHO and setting up KHO for the next
		 * kexec.
		 */
		if (err)
			pr_err("failed exposing handover FDT in sysfs: %d\n", err);

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
}
late_initcall(kho_init);

/*
 * The scratch areas are scaled by default as percent of memory allocated from
 * memblock. A user can override the scale with command line parameter:
 *
 * kho_scratch=N%
 *
 * It is also possible to explicitly define size for a global and per-node
 * scratch areas:
 *
 * kho_scratch=n[KMG],m[KMG]
 *
 * The explicit size definition takes precedence over scale definition.
 */
static unsigned int scratch_scale __initdata = 200;
static phys_addr_t scratch_size_global __initdata;
static phys_addr_t scratch_size_pernode __initdata;

static int __init kho_parse_scratch_size(char *p)
{
	unsigned long size, size_pernode;
	char *endptr, *oldp = p;

	if (!p)
		return -EINVAL;

	size = simple_strtoul(p, &endptr, 0);
	if (*endptr == '%') {
		scratch_scale = size;
		pr_notice("scratch scale is %d percent\n", scratch_scale);
	} else {
		size = memparse(p, &p);
		if (!size || p == oldp)
			return -EINVAL;

		if (*p != ',')
			return -EINVAL;

		size_pernode = memparse(p + 1, &p);
		if (!size_pernode)
			return -EINVAL;

		scratch_size_global = size;
		scratch_size_pernode = size_pernode;
		scratch_scale = 0;

		pr_notice("scratch areas: global: %lluMB pernode: %lldMB\n",
			  (u64)(scratch_size_global >> 20),
			  (u64)(scratch_size_pernode >> 20));
	}

	return 0;
}
early_param("kho_scratch", kho_parse_scratch_size);

static phys_addr_t __init scratch_size(int nid)
{
	phys_addr_t size;

	if (scratch_scale) {
		size = memblock_reserved_kern_size(nid) * scratch_scale / 100;
	} else {
		if (numa_valid_node(nid))
			size = scratch_size_pernode;
		else
			size = scratch_size_global;
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
	int nid, i = 1;

	if (!kho_enable)
		return;

	/* FIXME: deal with node hot-plug/remove */
	kho_scratch_cnt = num_online_nodes() + 1;
	size = kho_scratch_cnt * sizeof(*kho_scratch);
	kho_scratch = memblock_alloc(size, PAGE_SIZE);
	if (!kho_scratch)
		goto err_disable_kho;

	/* reserve large contiguous area for allocations without nid */
	size = scratch_size(NUMA_NO_NODE);
	addr = memblock_phys_alloc(size, CMA_MIN_ALIGNMENT_BYTES);
	if (!addr)
		goto err_free_scratch_desc;

	kho_scratch[0].addr = addr;
	kho_scratch[0].size = size;

	for_each_online_node(nid) {
		size = scratch_size(nid);
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

/*
 * Scan the FDT for any memory ranges and make sure they are reserved in
 * memblock, otherwise they will end up in a weird state on free lists.
 */
static void __init kho_init_reserved_pages(void)
{
	const void *fdt = kho_get_fdt();
	int offset = 0, depth = 0, initial_depth = 0, len;

	if (!fdt)
		return;

	/* Go through the mem list and add 1 for each reference */
	for (offset = 0;
	     offset >= 0 && depth >= initial_depth;
	     offset = fdt_next_node(fdt, offset, &depth)) {
		const void *mems;
		u32 i;

		mems = fdt_getprop(fdt, offset, "mem", &len);
		if (!mems || len & (sizeof(*mems) - 1))
			continue;

		for (i = 0; i < len; i += sizeof(struct kho_mem)) {
			const struct kho_mem *mem = mems + i;

			memblock_reserve(mem->addr, mem->size);
		}
	}
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
			set_pageblock_migratetype(pfn_to_page(pfn), MIGRATE_CMA);
	}
}

void __init kho_memory_init(void)
{
	if (!kho_get_fdt()) {
		kho_reserve_scratch();
	} else {
		kho_init_reserved_pages();
		kho_release_scratch();
	}
}

void __init kho_populate(phys_addr_t handover_dt_phys, phys_addr_t scratch_phys,
			 u64 scratch_len)
{
	void *handover_dt;
	struct kho_mem *scratch;

	/* Determine the real size of the FDT */
	handover_dt = early_memremap(handover_dt_phys, sizeof(struct fdt_header));
	if (!handover_dt) {
		pr_warn("setup: failed to memremap kexec FDT (0x%llx)\n", handover_dt_phys);
		return;
	}

	if (fdt_check_header(handover_dt)) {
		pr_warn("setup: kexec handover FDT is invalid (0x%llx)\n", handover_dt_phys);
		early_memunmap(handover_dt, sizeof(struct fdt_header));
		return;
	}

	kho_in.handover_len = fdt_totalsize(handover_dt);
	kho_in.handover_phys = handover_dt_phys;

	early_memunmap(handover_dt, sizeof(struct fdt_header));

	/* Reserve the FDT so we can still access it in late boot */
	memblock_reserve(kho_in.handover_phys, kho_in.handover_len);

	kho_in.kho_scratch_phys = scratch_phys;
	kho_scratch_cnt = scratch_len / sizeof(*kho_scratch);
	scratch = early_memremap(scratch_phys, scratch_len);
	if (!scratch) {
		pr_warn("setup: failed to memremap kexec scratch (0x%llx)\n", scratch_phys);
		return;
	}

	/*
	 * We pass a safe contiguous blocks of memory to use for early boot
	 * purporses from the previous kernel so that we can resize the
	 * memblock array as needed.
	 */
	for (int i = 0; i < kho_scratch_cnt; i++) {
		struct kho_mem *area = &scratch[i];
		u64 size = area->size;

		memblock_add(area->addr, size);

		if (WARN_ON(memblock_mark_kho_scratch(area->addr, size))) {
			pr_err("Kexec failed to mark the scratch region. Disabling KHO revival.");
			kho_in.handover_len = 0;
			kho_in.handover_phys = 0;
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
