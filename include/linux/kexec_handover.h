/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_KEXEC_HANDOVER_H
#define LINUX_KEXEC_HANDOVER_H

#include <linux/types.h>

struct kho_scratch {
	phys_addr_t addr;
	phys_addr_t size;
};

/* KHO Notifier index */
enum kho_event {
	KEXEC_KHO_FINALIZE = 0,
	KEXEC_KHO_ABORT = 1,
};

struct notifier_block;

struct kho_fdt {
	/* private: internal fields of KHO */
	void *fdt;
};

struct kho_serialization {
	struct kho_fdt *fdt;
};

#ifdef CONFIG_KEXEC_HANDOVER
bool kho_is_enabled(void);

int kho_new_fdt(struct kho_fdt *fdt);
int kho_finish_fdt(struct kho_fdt *fdt);
void kho_free_fdt(struct kho_fdt *fdt);
int kho_reset_fdt(struct kho_fdt *fdt);
int kho_validate_fdt(struct kho_fdt *fdt);

int kho_begin_node(struct kho_fdt *fdt, const char *name);
int kho_end_node(struct kho_fdt *fdt);
int kho_add_prop(struct kho_fdt *fdt, const char *key, const void *val,
		 int size);
int kho_add_string_prop(struct kho_fdt *fdt, const char *key, const char *val);
int kho_link_fdt(struct kho_fdt *parent, const char *name,
		 const struct kho_fdt *child);

int register_kho_notifier(struct notifier_block *nb);
int unregister_kho_notifier(struct notifier_block *nb);

void kho_memory_init(void);
#else
static inline bool kho_is_enabled(void)
{
	return false;
}

static inline int kho_new_fdt(struct kho_fdt *fdt)
{
	return -EOPNOTSUPP;
}

static inline int kho_finish_fdt(struct kho_fdt *fdt)
{
	return -EOPNOTSUPP;
}

static inline void kho_free_fdt(struct kho_fdt *fdt)
{
}

static inline int kho_reset_fdt(struct kho_fdt *fdt)
{
	return -EOPNOTSUPP;
}

static inline int kho_validate_fdt(struct kho_fdt *fdt)
{
	return -EOPNOTSUPP;
}

static inline int kho_begin_node(struct kho_fdt *fdt, const char *name)
{
	return -EOPNOTSUPP;
}

static inline int kho_end_node(struct kho_fdt *fdt)
{
	return -EOPNOTSUPP;
}

static inline int kho_add_prop(struct kho_fdt *fdt, const char *key,
			       const void *val, int size)
{
	return -EOPNOTSUPP;
}

static inline int kho_add_string_prop(struct kho_fdt *fdt, const char *key,
				      const char *val)
{
	return -EOPNOTSUPP;
}

static inline int kho_link_fdt(struct kho_fdt *parent, const char *name,
			       const struct kho_fdt *child)
{
	return -EOPNOTSUPP;
}

static inline int register_kho_notifier(struct notifier_block *nb)
{
	return -EOPNOTSUPP;
}

static inline int unregister_kho_notifier(struct notifier_block *nb)
{
	return -EOPNOTSUPP;
}

static inline void kho_memory_init(void)
{
}
#endif /* CONFIG_KEXEC_HANDOVER */

#endif /* LINUX_KEXEC_HANDOVER_H */
