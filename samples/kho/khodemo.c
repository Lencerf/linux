// SPDX-License-Identifier: GPL v2
/*
 * Copyright 2024 Google LLC
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/kexec_handover.h>
#include <linux/list.h>
#include <linux/libfdt.h>
#include <linux/mm_types.h>
#include <linux/slab.h>
#include <asm/io.h>
#include <linux/mm.h>

static int small_param;
module_param(small_param, int, 0644);

struct named_page {
	struct list_head list;
	struct kobj_attribute attr;
	char *buf;
	struct kho_node node;
	phys_addr_t buf_phys;
};

static struct list_head named_pages = LIST_HEAD_INIT(named_pages);

static struct kobject *root_kobj;
static struct kobject *named_pages_kobj;

struct kho_node mod_node = KHO_NODE_INIT;
struct kho_node named_pages_node = KHO_NODE_INIT;

static ssize_t __used store_buf(struct kobject *kobj,
				struct kobj_attribute *attr, const char *buf,
				size_t count)
{
	size_t size = count;
	char *target_buf = NULL;

	struct list_head *pos;
	list_for_each(pos, &named_pages) {
		struct named_page *p = container_of(pos, struct named_page, list);

		if (attr == &p->attr) {
			target_buf = p->buf;
			break;
		}
	}

	if (!target_buf)
		return -ENOENT;

	if (size > PAGE_SIZE - 1)
		size = PAGE_SIZE - 1;

	memcpy(target_buf, buf, size);
	target_buf[size + 1] = '\0';

	return size;
}

static ssize_t __used show_buf(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	char *target_buf = NULL;
	struct list_head *pos;

	list_for_each(pos, &named_pages) {
		struct named_page *p = container_of(pos, struct named_page, list);

		if (attr == &p->attr) {
			target_buf = p->buf;
			break;
		}
	}
	if (!target_buf)
		return -ENOENT;

	memcpy(buf, target_buf, PAGE_SIZE);
	return PAGE_SIZE - 1;
}

static int add_named_page(const char *name, char *buf)
{
	int err = 0;
	struct named_page *p;

	p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (!p) {
		err = -ENOMEM;
		goto err1;
	}
	kho_init_node(&p->node);
	p->attr = (struct kobj_attribute) {
		.attr = {
			.name = name,
			.mode = 0644,
		},
		.show = show_buf,
		.store = store_buf,
	};
	p->buf = buf;
	list_add_tail(&p->list, &named_pages);
	pr_err("added named buf %p\n", buf);

	err = sysfs_add_file_to_group(named_pages_kobj, &p->attr.attr, NULL);
	if (err)
		goto err2;

	p->buf_phys = virt_to_phys(buf);

	kho_preserve_phys(p->buf_phys, PAGE_SIZE);
	kho_add_prop(&p->node, "address", &p->buf_phys, sizeof(p->buf_phys));
	kho_add_node(&named_pages_node, name, &p->node);

	pr_info("added named page %s\n", name);
	return 0;

err2:
	list_del(&p->list);
	kfree(p);
err1:
	return err;
}

static ssize_t __used store_add_page(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	ssize_t err = 0;
	char *name;
	char *page_buf;

	size_t l = strnlen(buf, count - 1);

	name = kmalloc(l + 1, GFP_KERNEL);
	if (!name) {
		err = -ENOMEM;
		goto err1;
	}
	name[l] = '\0';
	memcpy(name, buf, l);

	page_buf = (char *)get_zeroed_page(GFP_KERNEL);
	if (!page_buf) {
		err = -ENOMEM;
		goto err2;
	}
	err = add_named_page(name, page_buf);
	if (err)
		goto err3;

	return l + 1;

err3:
	free_page((unsigned long)page_buf);
err2:
	kfree(name);
err1:
	return err;
}

static struct kobj_attribute add_page_attr =
	__ATTR(add_page, 0200, NULL, store_add_page);



static ssize_t __used store_remove_page(struct kobject *kobj,
	struct kobj_attribute *attr,
	const char *buf, size_t count)
{
	struct named_page *p;
	size_t l = strnlen(buf, count - 1);

	list_for_each_entry(p, &named_pages, list)
		if (!strcmp(p->attr.attr.name, buf))
			break;

	if (&p->list == &named_pages) {
		pr_err("cannot find %s\n", buf);
		return -ENOENT;
	}

	sysfs_remove_file(named_pages_kobj, &p->attr.attr);

	kho_remove_prop(&p->node, "address", NULL);
	kho_remove_node(&named_pages_node, buf);

	list_del_init(&p->list);
	free_page((unsigned long)(p->buf));
	kfree(p->attr.attr.name);
	kfree(p);

	return l + 1;
}

static struct kobj_attribute remove_page_attr =
	__ATTR(remove_page, 0200, NULL, store_remove_page);


/* put attribute to attribute group */
static struct attribute *register_attrs[] = {
	&add_page_attr.attr,
	&remove_page_attr.attr,
	NULL, /* NULL terminate the list*/
};

static struct attribute_group reg_attr_group = { .attrs = register_attrs };


// static int demo_kho_notifier_finalize(void) {
// 	size_t pages_count = 0;
// 	size_t names_total_size = 0;
// 	struct list_head *pos;
// 	phys_addr_t *addresses;
// 	char *names_buffer;
// 	int iter = 0;
// 	int name_offset = 0;

// 	/* Calculate the size of the temp buffer for page addresses and names. */
// 	list_for_each(pos, &named_pages) {
// 		struct named_page *p = container_of(pos, struct named_page, list);

// 		pages_count += 1;
// 		names_total_size += strlen(p->attr.attr.name) + 1;
// 	}
// 	/* Device tree string list property is ended by an extra \0. */
// 	names_total_size += 1;
// 	/* String list property size must be 4-byte aligned. */
// 	names_total_size = (names_total_size + 3) & ~3;
// 	addresses = kvmalloc_array(pages_count, sizeof(phys_addr_t), GFP_KERNEL);
// 	names_buffer = kzalloc(names_total_size, GFP_KERNEL);
// 	if (!(addresses && names_buffer)) {
// 		pr_err("failed to allocate temp buffer");
// 		return NOTIFY_BAD;
// 	}
// 	/* Now serialize the addresses and names of the page list. */
// 	list_for_each(pos, &named_pages) {
// 		struct named_page *p = container_of(pos, struct named_page, list);

// 		pr_err("save %llx\n", virt_to_phys(p->buf));
// 		kho_preserve_folio(virt_to_folio(p->buf));
// 		addresses[iter] = __pa(p->buf);
// 		size_t name_len = strlen(p->attr.attr.name);

// 		memcpy(names_buffer + name_offset, p->attr.attr.name, name_len);

// 		name_offset += name_len + 1;
// 		iter += 1;
// 	}

// 	kho_add_prop(&named_pages_node, "addresses", addresses, sizeof(phys_addr_t) * pages_count);
// 	kho_add_prop(&named_pages_node, "names", names_buffer, names_total_size);

// 	return NOTIFY_DONE;
// }

// static int demo_kho_notifier_unfreeze(void) {
// 	void* buffer;

// 	buffer = kho_remove_prop(&named_pages_node, "addresses", NULL);
// 	if (IS_ERR(buffer))
// 		pr_err("remove addresses %ld\n", PTR_ERR(buffer));
// 	else
// 		kvfree(buffer);

// 	buffer = kho_remove_prop(&named_pages_node, "names", NULL);
// 	if (IS_ERR(buffer))
// 		pr_err("remove mem %ld\n", PTR_ERR(buffer));
// 	else
// 		kvfree(buffer);

// 	return NOTIFY_DONE;
// }

// static int demo_kho_notifier(struct notifier_block *self, unsigned long cmd, void *data)
// {
// 	switch (cmd) {
// 	case KEXEC_KHO_UNFREEZE:
// 		return demo_kho_notifier_unfreeze();
// 	case KEXEC_KHO_FINALIZE:
// 		return demo_kho_notifier_finalize();
// 	default:
// 		return NOTIFY_BAD;
// 	}
// }

static int add_saved_named_page(const char *name, const struct kho_in_node *node, void *data) {
	u32 size = 0;
	phys_addr_t *addr;
	struct folio *f ;
	char *buf;

	addr = (phys_addr_t *)kho_get_prop(node, "address", &size);
	if(!addr || size != sizeof(*addr)) {
		pr_err("expect address, find addr=%p, size=%d\n", addr, size);
		return -EINVAL;
	}

	f = kho_restore_folio(*addr);
	buf = page_to_virt(folio_page(f, 0));
	return add_named_page(name, buf);
}

static int print_nodes(const char *name, const struct kho_in_node *node, void *data) {
	pr_info("node name = %s\n", name);
	return 0;
}

static int restore_from_kho(void)
{
	/* Find our node `khodemo` in the device tree. */
	// const void *fdt = kho_get_fdt();
	// int offset, len, err, i = 0;
	int err = 0;
	const void *p_small_param = NULL;
	int prop_size = 0;
	struct kho_in_node node_demo;
	struct kho_in_node node_pages;

	pr_err("restoring from KHO");

	kho_get_nodes(NULL, print_nodes, NULL);

	err = kho_get_node(NULL, "khodemo", &node_demo);

	if (err < 0) {
		pr_err("cannot find khodemo in KHO FDT\n");
		return -ENOENT;
	}

	p_small_param = kho_get_prop(&node_demo, "small_param", &prop_size);
	if (!p_small_param ||prop_size != sizeof(small_param)) {
		pr_err("Looking for /khodemo.small_param, found p=%p, len=%d\n",
			p_small_param, prop_size);
		err = -ENOENT;
		goto out;
	}
	small_param = *(int *)p_small_param;

	err = kho_get_node(&node_demo, "named_pages", &node_pages);
	if (err < 0) {
		pr_err("cannot find named_pages in KHO dt\n");
		err = -ENOENT;
		goto reset_small_param;
	}

	kho_get_nodes(&node_pages, add_saved_named_page, NULL);

	// addresses = kho_get_prop(&node_pages, "addresses", &addresses_size);
	// names = kho_get_prop(&node_pages, "names", NULL);
	// // addresses = (phys_addr_t *)fdt_getprop(fdt, offset, "addresses", &pages_size);
	// // names = (char *)fdt_getprop(fdt, offset, "names", &names_size);
	// if (!(addresses && names)) {
	// 	pr_err("cannot find both `addresses` and `names` in /khodemo/named_pages");
	// 	err = -ENOENT;
	// 	goto reset_small_param;
	// }
	// for (p_name = names; i < addresses_size / sizeof(phys_addr_t); i += 1) {
	// 	// struct kho_mem mem = {
	// 	// 	.addr = addresses[i],
	// 	// 	.size = PAGE_SIZE,
	// 	// };
	// 	struct folio *f = kho_restore_folio(addresses[i]);
	// 	char *buf = page_to_virt(folio_page(f, 0));
	// 	// char *buf =  phys_to_virt(addresses[i]);
	// 	size_t name_len = strlen(p_name);
	// 	char *name = kmalloc(name_len + 1, GFP_KERNEL);

	// 	if (!name) {
	// 		err = -ENOMEM;
	// 		goto reset_pages;
	// 	}

	// 	pr_err("restroing %llx\n", addresses[i]);

	// 	name[name_len] = '\0';
	// 	memcpy(name, p_name, name_len);
	// 	err = add_named_page(name, buf);
	// 	if (err)
	// 		goto reset_pages;

	// 	p_name += name_len + 1;
	// }
	return 0;

// reset_pages:
	pr_err("TODO: reset the page list");
reset_small_param:
	small_param = 0;
out:
	return err;
}


// static struct notifier_block demo_kho_nb = {
// 	.notifier_call = demo_kho_notifier,
// };

static int __init demo_init(void)
{
	int err = 0;

	root_kobj = kobject_create_and_add("khodemo", kernel_kobj);
	if (!root_kobj) {
		err = -ENOMEM;
		goto out;
	}

	err = sysfs_create_group(root_kobj, &reg_attr_group);
	if (err)
		goto put_root;

	named_pages_kobj = kobject_create_and_add("named_pages", root_kobj);
	if (!named_pages_kobj) {
		err = -ENOMEM;
		goto remove_attr;
	}

	err = restore_from_kho();
	if (err) {
		pr_warn("%s: %d\n", __func__, err);
	}

	// err = register_kho_notifier(&demo_kho_nb);
	// if (err)
	// 	goto put_named_pages;

	kho_add_node(NULL, "khodemo", &mod_node);
	kho_add_node(&mod_node, "named_pages", &named_pages_node);
	kho_add_prop(&mod_node, "small_param", &small_param, sizeof(small_param));

	return 0;

// put_named_pages:
	kobject_put(named_pages_kobj);
remove_attr:
	sysfs_remove_group(root_kobj, &reg_attr_group);
put_root:
	kobject_put(root_kobj);
out:
	return err;
}

static void __exit demo_exit(void)
{
	struct list_head *pos;
	struct list_head *n;

	// unregister_kho_notifier(&demo_kho_nb);

	list_for_each_safe(pos, n, &named_pages) {
		list_del(pos);
		struct named_page *p = container_of(pos, struct named_page, list);

		sysfs_remove_file_from_group(named_pages_kobj, &p->attr.attr,
					     NULL);
		free_page((unsigned long)p->buf);
		kfree(p->attr.attr.name);
		kfree(p);
	}

	kobject_put(named_pages_kobj);
	sysfs_remove_group(root_kobj, &reg_attr_group);
	kobject_put(root_kobj);

	pr_info("KHO demo exit");
}

module_init(demo_init);
module_exit(demo_exit);

MODULE_AUTHOR("Changyuan Lyu <changyuanl@google.com>");
MODULE_DESCRIPTION("Demo driver using KHO");
MODULE_LICENSE("GPL");
