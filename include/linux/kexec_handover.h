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
struct folio;

#define DECLARE_KHOSER_PTR(name, type) \
	union {                        \
		phys_addr_t phys;      \
		type ptr;              \
	} name
#define KHOSER_STORE_PTR(dest, val)               \
	({                                        \
		typeof(val) v = val;              \
		typecheck(typeof((dest).ptr), v); \
		(dest).phys = virt_to_phys(v);    \
	})
#define KHOSER_LOAD_PTR(src)                                                 \
	({                                                                   \
		typeof(src) s = src;                                         \
		(typeof((s).ptr))((s).phys ? phys_to_virt((s).phys) : NULL); \
	})

struct kho_fdt {
	/* private: internal fields of KHO */
	void *fdt;
};

struct kho_mem_track;

struct kho_serialization {
	struct kho_fdt *fdt;
	struct kho_mem_track *tracker;
};

struct kho_node {
	/* private: internal fields of KHO */
	const void *fdt;
	int offset;
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

int kho_preserve_folio(struct kho_mem_track *tracker, struct folio *folio);
void kho_unpreserve_folio(struct kho_mem_track *tracker, struct folio *folio);
int kho_preserve_phys(struct kho_mem_track *tracker, phys_addr_t phys,
		      size_t size);
int kho_unpreserve_phys(struct kho_mem_track *tracker, phys_addr_t phys,
			size_t size);
int kho_preserve_fdt(struct kho_mem_track *tracker, struct kho_fdt *fdt);
void kho_unpreserve_fdt(struct kho_mem_track *tracker, struct kho_fdt *fdt);
struct folio *kho_restore_folio(phys_addr_t phys);
void *kho_restore_phys(phys_addr_t phys, size_t size);

void kho_memory_init(void);

void kho_populate(phys_addr_t handover_fdt_phys, phys_addr_t scratch_phys,
		  u64 scratch_len);

int kho_retrieve_node(const struct kho_node *parent, const char *name,
		      struct kho_node *child);
const void *kho_retrieve_prop(const struct kho_node *node, const char *key,
			      u32 *size);
int kho_node_check_compatible(const struct kho_node *node,
			      const char *compatible);
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

static inline int kho_preserve_folio(struct folio *folio)
{
	return -EOPNOTSUPP;
}

static inline int kho_unpreserve_folio(struct folio *folio)
{
	return -EOPNOTSUPP;
}

static inline int kho_preserve_phys(phys_addr_t phys, size_t size)
{
	return -EOPNOTSUPP;
}

static inline int kho_unpreserve_phys(phys_addr_t phys, size_t size)
{
	return -EOPNOTSUPP;
}

static inline int kho_preserve_fdt(struct kho_mem_track *tracker,
				   struct kho_fdt *fdt)
{
	return -EOPNOTSUPP;
}

static inline void kho_unpreserve_fdt(struct kho_mem_track *tracker,
				      struct kho_fdt *fdt)
{
}

static inline struct folio *kho_restore_folio(phys_addr_t phys)
{
	return NULL;
}

static inline void *kho_restore_phys(phys_addr_t phys, size_t size)
{
	return NULL;
}

static inline void kho_memory_init(void)
{
}

static inline void kho_populate(phys_addr_t handover_fdt_phys,
				phys_addr_t scratch_phys, u64 scratch_len)
{
}

static inline int kho_retrieve_node(const struct kho_node *parent,
				    const char *name, struct kho_node *child)
{
	return -EOPNOTSUPP;
}

static inline const void *kho_retrieve_prop(const struct kho_node *node,
					    const char *key, u32 *size)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline int kho_node_check_compatible(const struct kho_node *node,
					    const char *compatible)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_KEXEC_HANDOVER */

#endif /* LINUX_KEXEC_HANDOVER_H */
