// SPDX-License-Identifier: GPL v2

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/kexec.h>
#include <linux/kexec_handover.h>
#include <linux/list.h>
#include <linux/libfdt.h>

static int small_param;
module_param(small_param, int, 0644);

struct named_page {
	struct list_head list;
	struct kobj_attribute attr;
	char *buf;
};

static struct list_head named_pages;

static struct kobject *root_kobj;
static struct kobject *named_pages_kobj;

struct kho_node mod_node = KHO_NODE_INIT(mod_node);
struct kho_node named_pages_node = KHO_NODE_INIT(named_pages_node);

static char *demo_buf;

static struct kho_mem demo_buf_mem = {
	.size = PAGE_SIZE,
};

static ssize_t __used store_buf(struct kobject *kobj,
				struct kobj_attribute *attr, const char *buf,
				size_t count)
{
	size_t size = count;
	char *target_buf = NULL;

	if (kobj == root_kobj) {
		target_buf = demo_buf;
	} else {
		struct list_head *pos;

		list_for_each(pos, &named_pages) {
			struct named_page *p = container_of(pos, struct named_page, list);

			if (attr == &p->attr) {
				target_buf = p->buf;
				break;
			}
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

	if (kobj == root_kobj) {
		target_buf = demo_buf;
	} else {
		struct list_head *pos;

		list_for_each(pos, &named_pages) {
			struct named_page *p = container_of(pos, struct named_page, list);

			if (attr == &p->attr) {
				target_buf = p->buf;
				break;
			}
		}
	}
	if (!target_buf)
		return -ENOENT;

	memcpy(buf, target_buf, PAGE_SIZE);
	return PAGE_SIZE - 1;
}

static struct kobj_attribute store_val_attribute =
	__ATTR(demo_buf, 0644, show_buf, store_buf);

int add_named_page(const char *name, char *buf)
{
	int err = 0;
	struct named_page *p;

	p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (!p) {
		err = -ENOMEM;
		goto err1;
	}
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

	err = sysfs_add_file_to_group(named_pages_kobj, &p->attr.attr, NULL);
	if (err)
		goto err2;

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

/* put attribute to attribute group */
static struct attribute *register_attrs[] = {
	&store_val_attribute.attr, &add_page_attr.attr,
	NULL, /* NULL terminate the list*/
};

static struct attribute_group reg_attr_group = { .attrs = register_attrs };

static int demo_kho_notifier(struct notifier_block *self, unsigned long cmd,
			     void *fdt)
{
	int err = 0;
	size_t pages_count = 0;
	size_t names_total_size = 0;
	struct list_head *pos;
	struct kho_mem *mems;
	char *names_buffer;
	int iter = 0;
	int name_offset = 0;

	switch (cmd) {
	case KEXEC_KHO_ABORT:
		pr_err("TODO: KEXEC_KHO_ABORT");

		return NOTIFY_DONE;
	case KEXEC_KHO_DUMP:
		/* handled below */
		break;
	default:
		return NOTIFY_BAD;
	}

	/* Calculate the size of the temp buffer for page addresses and names. */
	list_for_each(pos, &named_pages) {
		struct named_page *p = container_of(pos, struct named_page, list);

		pages_count += 1;
		names_total_size += strlen(p->attr.attr.name) + 1;
	}
	/* Device tree string list property is ended by an extra \0. */
	names_total_size += 1;
	/* String list property size must be 4-byte aligned. */
	names_total_size = (names_total_size + 3) & ~3;
	mems = kmalloc_array(pages_count, sizeof(struct kho_mem), GFP_KERNEL);
	names_buffer = kzalloc(names_total_size, GFP_KERNEL);
	if (!(mems && names_buffer)) {
		pr_err("failed to allocate temp buffer");
		fdt_end_node(fdt);
		return NOTIFY_BAD;
	}
	/* Now serialize the addresses and names of the page list. */
	list_for_each(pos, &named_pages) {
		struct named_page *p = container_of(pos, struct named_page, list);

		mems[iter] = (struct kho_mem){
			.addr = __pa(p->buf),
			.size = PAGE_SIZE,
		};
		size_t name_len = strlen(p->attr.attr.name);

		memcpy(names_buffer + name_offset, p->attr.attr.name, name_len);

		name_offset += name_len + 1;
		iter += 1;
	}
	kho_add_prop(&named_pages_node, "mem", mems, sizeof(struct kho_mem) * pages_count);
	kho_add_prop(&named_pages_node, "names", names_buffer, names_total_size);

	if (err)
		pr_err("%s: err=%d\n", __func__, err);

	return err ? NOTIFY_BAD : NOTIFY_DONE;
}

static int restore_text_from_dt(void)
{
	/* Find our node `khodemo` in the device tree. */
	const void *fdt = kho_get_fdt();
	int offset, len, err, i = 0;
	const void *p;
	int pages_size = 0;
	int names_size = 0;
	const struct kho_mem *mems;
	const char *names;
	const char *p_name;

	if (!fdt) {
		pr_err("cannot find kho dt\n");

		return -ENOENT;
	}

	offset = fdt_path_offset(fdt, "/khodemo");
	if (offset < 0) {
		pr_err("cannot find /khodemo in KHO dt\n");

		return -ENOENT;
	}

	p = fdt_getprop(fdt, offset, "small_param", &len);
	if (!p || len != sizeof(small_param)) {
		pr_err("Looking for /khodemo.small_param, found p=%p, len=%d\n",
		       p, len);
		err = -ENOENT;
		goto err1;
	}
	small_param = *(int *)p;

	p = fdt_getprop(fdt, offset, "mem", &len);
	if (!p || len != sizeof(struct kho_mem)) {
		pr_err("Looing for /khodemo.mem, found p=%p, len=%d\n", p, len);
		err = -ENOENT;
		goto err2;
	}

	demo_buf = kho_claim_mem(p);

	offset = fdt_path_offset(fdt, "/khodemo/named_pages");
	if (offset < 0) {
		pr_err("cannot find named_pages in KHO dt\n");
		err = -ENOENT;
		goto err3;
	}

	mems = (struct kho_mem *)fdt_getprop(fdt, offset, "mem", &pages_size);
	names = (char *)fdt_getprop(fdt, offset, "names", &names_size);
	if (!(mems && names)) {
		pr_err("cannot find both `mem` and `names` in /khodemo/named_pages");
		err = -ENOENT;
		goto err3;
	}
	p_name = names;
	for (; i < pages_size / sizeof(struct kho_mem); i += 1) {
		char *buf = kho_claim_mem(&mems[i]);
		size_t name_len = strlen(p_name);
		char *name = kmalloc(name_len + 1, GFP_KERNEL);

		if (!name)
			goto err4;

		name[name_len] = '\0';
		memcpy(name, p_name, name_len);
		err = add_named_page(name, buf);
		if (err)
			goto err4;

		p_name += name_len + 1;
	}
	return 0;

err4:
	pr_err("TODO: reset the page list");
err3:
	free_page((unsigned long)demo_buf);
err2:
	small_param = 0;
err1:
	return err;
}

static struct notifier_block demo_kho_nb = {
	.notifier_call = demo_kho_notifier,
};

static int __init demo_init(void)
{
	int err = 0;

	pr_info("KHO demo init");
	INIT_LIST_HEAD(&named_pages);

	root_kobj = kobject_create_and_add("khodemo", kernel_kobj);
	if (!root_kobj) {
		err = -ENOMEM;
		goto err1;
	}
	err = sysfs_create_group(root_kobj, &reg_attr_group);
	if (err)
		goto err2;

	named_pages_kobj = kobject_create_and_add("named_pages", root_kobj);
	if (!named_pages_kobj) {
		err = -ENOMEM;
		goto err3;
	}

	err = restore_text_from_dt();
	if (err) {
		pr_warn("%s: %d\n", __func__, err);
		demo_buf = (char *)get_zeroed_page(GFP_KERNEL);

		if (!demo_buf) {
			err = -ENOMEM;
			goto err4;
		}
	}

	register_kho_notifier(&demo_kho_nb);
	kho_add_node(NULL, KBUILD_MODNAME, &mod_node);
	kho_add_node(&mod_node, "named_pages", &named_pages_node);
	kho_add_prop(&mod_node, "small_param", &small_param, sizeof(small_param));
	kho_add_prop(&mod_node, "mem", &demo_buf_mem, sizeof(demo_buf_mem));

	return 0;

err4:
	kobject_put(named_pages_kobj);
err3:
	sysfs_remove_group(root_kobj, &reg_attr_group);
err2:
	kobject_put(root_kobj);
err1:
	return err;
}

static void __exit demo_exit(void)
{
	struct list_head *pos;
	struct list_head *n;

	unregister_kho_notifier(&demo_kho_nb);

	if (demo_buf)
		free_page((unsigned long)demo_buf);

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
