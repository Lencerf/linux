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
	int err;

	if (!kho_enable)
		return -EINVAL;

	err = kho_out_sysfs_init();
	if (err)
		return err;

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

void __init kho_memory_init(void)
{
	kho_reserve_scratch();
}
