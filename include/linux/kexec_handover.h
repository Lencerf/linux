/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_KEXEC_HANDOVER_H
#define LINUX_KEXEC_HANDOVER_H

#include <linux/types.h>

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

struct kho_serialization;

#ifdef CONFIG_KEXEC_HANDOVER

int kho_preserve_folio(struct kho_serialization *ser, char *name,
		       struct folio *folio);
int kho_preserve_phys(struct kho_serialization *ser, char *name,
		      phys_addr_t phys, size_t size);
struct folio *kho_restore_folio(phys_addr_t phys);
void *kho_restore_phys(phys_addr_t phys, size_t size);

#else

static inline int kho_preserve_folio(struct kho_serialization *ser, char *name,
				     struct folio *folio)
{
	return -EOPNOTSUPP;
}

static inline int kho_preserve_phys(struct kho_serialization *ser, char *name,
				    phys_addr_t phys, size_t size)
{
	return -EOPNOTSUPP;
}

static inline struct folio *kho_restore_folio(phys_addr_t phys)
{
	return NULL;
}

static inline void *kho_restore_phys(phys_addr_t phys, size_t size)
{
	return NULL;
}

#endif /* CONFIG_KEXEC_HANDOVER */

#endif /* LINUX_KEXEC_HANDOVER_H */
