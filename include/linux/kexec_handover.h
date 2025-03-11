/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_KEXEC_HANDOVER_H
#define LINUX_KEXEC_HANDOVER_H

#include <linux/types.h>
#include <linux/hashtable.h>
#include <linux/notifier.h>
#include <linux/mm_types.h>

struct kho_mem {
	phys_addr_t addr;
	phys_addr_t size;
};

/* KHO Notifier index */
enum kho_event {
	KEXEC_KHO_FINALIZE = 0,
	KEXEC_KHO_UNFREEZE = 1,
};

#define KHO_HASHTABLE_BITS 4
#define KHO_NODE_INIT { \
	.props = HASHTABLE_INIT(KHO_HASHTABLE_BITS), \
	.nodes = HASHTABLE_INIT(KHO_HASHTABLE_BITS), \
}

struct kho_node {
	struct hlist_node hlist;

	struct list_head list;
	bool visited;

	const char *name;
	DECLARE_HASHTABLE(props, KHO_HASHTABLE_BITS);
	DECLARE_HASHTABLE(nodes, KHO_HASHTABLE_BITS);
};

#ifdef CONFIG_KEXEC_HANDOVER
bool kho_is_enabled(void);
void kho_init_node(struct kho_node *node);
int kho_add_node(struct kho_node *parent, const char *name, struct kho_node *child);
struct kho_node *kho_remove_node(struct kho_node *parent, const char *name);
int kho_add_prop(struct kho_node *node, const char *key, const void *val, u32 size);
void *kho_remove_prop(struct kho_node *node, const char *key, u32 *size);
int kho_add_string_prop(struct kho_node *node, const char *key, const char *val);

int register_kho_notifier(struct notifier_block *nb);
int unregister_kho_notifier(struct notifier_block *nb);

int kho_preserve_folio(struct folio *folio);
int kho_preserve_phys(phys_addr_t phys, size_t size);
struct folio *kho_restore_folio(phys_addr_t phys);
void *kho_restore_phys(phys_addr_t phys, size_t size);

void kho_memory_init(void);

void kho_populate(phys_addr_t dt_phys, phys_addr_t scratch_phys,
		  u64 scratch_len);
const void *kho_get_fdt(void);
void kho_return_mem(const struct kho_mem *mem);
void *kho_claim_mem(const struct kho_mem *mem);
#else
static inline bool kho_is_enabled(void) { return false; }
static inline void kho_init_node(struct kho_node *node) { }
static inline int kho_add_node(struct kho_node *parent, const char *name,
			       struct kho_node *child) { return 0; }
static inline struct kho_node *kho_remove_node(struct kho_node *parent,
					       const char *name) { return NULL; }
static inline int kho_add_prop(struct kho_node *node, const char *key,
			       const void *val, u32 size) { return 0; }
static inline void *kho_remove_prop(struct kho_node *node, const char *key,
				    u32 *size) { return NULL; }
static inline int kho_add_string_prop(struct kho_node *node, const char *key,
				      const char *val) { return 0; }

static inline int register_kho_notifier(struct notifier_block *nb) { return 0; }
static inline int unregister_kho_notifier(struct notifier_block *nb) { return 0; }

static inline int kho_preserve_folio(struct folio *folio) { return 0; }
static inline int kho_preserve_phys(phys_addr_t phys, size_t size) { return 0; }
static inline struct folio *kho_restore_folio(phys_addr_t phys) { return NULL; }
static inline void *kho_restore_phys(phys_addr_t phys, size_t size) { return NULL; }

static inline void kho_memory_init(void) {}

static inline void kho_populate(phys_addr_t dt_phys, phys_addr_t scratch_phys,
				u64 scratch_len) {}
static inline void *kho_get_fdt(void) { return NULL; }
static inline void kho_return_mem(const struct kho_mem *mem) { }
static inline void *kho_claim_mem(const struct kho_mem *mem) { return NULL; }
#endif /* CONFIG_KEXEC_HANDOVER */

#endif /* LINUX_KEXEC_HANDOVER_H */
