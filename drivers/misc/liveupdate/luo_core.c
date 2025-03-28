// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/**
 * DOC: Live Update Orchestrator (LUO)
 *
 * Live Update is a specialized reboot process where selected devices are
 * kept operational across a kernel transition. For these devices, DMA activity
 * may continue the kernel reboot.
 *
 * The primary use case is in cloud environments, allowing hypervisor updates
 * without disrupting running virtual machines. During a live update, VMs can be
 * suspended (with their state preserved in memory), while the hypervisor kernel
 * reboots. Devices attached to these VMs (e.g., NICs, block devices) are kept
 * operational by the LUO during the hypervisor reboot, allowing the VMs to be
 * quickly resumed on the new kernel.
 *
 * The core of LUO is a state machine that tracks the progress of a live update,
 * along with a callback API that allows other kernel subsystems to participate
 * in the process. Example subsystems that can hook into LUO include: kvm,
 * iommu, interrupts, Documentation/driver-api/liveupdate.rst, participating
 * filesystems, and mm.
 *
 * LUO uses KHO to transfer memory state from Old Kernel to the New Kernel.
 *
 * The LUO state machine ensures that operations are performed in the correct
 * sequence and provides a mechanism to track and recover from potential
 * failures, and select devices and subsystems that should participate in
 * live update sequence.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/err.h>
#include <linux/kexec_handover.h>
#include <linux/kobject.h>
#include <linux/libfdt.h>
#include <linux/liveupdate.h>
#include <linux/rwsem.h>
#include <linux/string.h>
#include "luo_internal.h"

DECLARE_RWSEM(luo_state_rwsem);

enum liveupdate_state luo_state;

static const char *const luo_event_str[] = {
	"PREPARE",
	"REBOOT",
	"FINISH",
	"CANCEL",
};

const char *const luo_state_str[] = {
	"normal",
	"prepared",
	"frozen",
	"updated",
};

bool luo_enabled;

static struct kho_serialization *luo_kho_ser;
static void *luo_fdt_out;
static void *luo_fdt_in;
/* LUO FDT size is 2M, if needed can be expanded */
#define LUO_FDT_SIZE		(2 << 20)
#define LUO_KHO_ENTRY_NAME	"LUO"
#define LUO_STATE		"state"
#define LUO_COMPATIBLE		"luo-v1"

static int __init early_liveupdate_param(char *buf)
{
	return kstrtobool(buf, &luo_enabled);
}
early_param("liveupdate", early_liveupdate_param);

/* Return true if the current state is equal to the provided state */
#define IS_STATE(state) (READ_ONCE(luo_state) == (state))

static void __luo_set_state(enum liveupdate_state state)
{
	void *fdt = luo_fdt_out ? luo_fdt_out : luo_fdt_in;

	if (fdt) {
		u8 *state_ptr = (u8 *)fdt_getprop_w(fdt, 0, LUO_STATE, NULL);

		if (!state_ptr)
			pr_warn("failed to update fdt state property\n");
		else
			*state_ptr = (u8)state;
	}

	WRITE_ONCE(luo_state, state);
}

static inline void luo_set_state(enum liveupdate_state state)
{
	pr_info("Switched from [%s] to [%s] state\n",
		LUO_STATE_STR, luo_state_str[state]);
	__luo_set_state(state);
}

/* Called during the prepare phase, to create LUO fdt tree */
static int luo_fdt_setup(struct kho_serialization *ser)
{
	u8 state_val;
	int ret;

	luo_fdt_out = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
					       get_order(LUO_FDT_SIZE));
	if (!luo_fdt_out) {
		pr_err("failed to allocate FDT memory\n");
		return -ENOMEM;
	}

	ret = fdt_create_empty_tree(luo_fdt_out, LUO_FDT_SIZE);
	if (ret)
		goto exit_free;

	ret = fdt_setprop(luo_fdt_out, 0, "compatible", LUO_COMPATIBLE,
			  strlen(LUO_COMPATIBLE) + 1);
	if (ret)
		goto exit_free;

	ret = fdt_setprop(luo_fdt_out, 0, LUO_STATE, &state_val,
			  sizeof(state_val));
	if (ret)
		goto exit_free;

	ret = luo_files_fdt_setup(luo_fdt_out);
	if (ret)
		goto exit_free;

	ret = luo_subsystems_fdt_setup(luo_fdt_out);
	if (ret)
		goto exit_free;

	ret = kho_preserve_phys(ser, __pa(luo_fdt_out), LUO_FDT_SIZE);
	if (ret)
		goto exit_free;

	ret = kho_add_subtree(ser, LUO_KHO_ENTRY_NAME, luo_fdt_out);
	if (ret)
		goto exit_free;

	return 0;

exit_free:
	pr_err("failed to prepare LUO FDT: %d\n", ret);
	free_pages((unsigned long)luo_fdt_out, get_order(LUO_FDT_SIZE));
	luo_fdt_out = NULL;

	return ret;
}

static void luo_fdt_destroy(struct kho_serialization *ser)
{
	free_pages((unsigned long)luo_fdt_out, get_order(LUO_FDT_SIZE));
	luo_fdt_out = NULL;
}

static int luo_do_prepare_calls(void)
{
	int ret;

	ret = luo_do_files_prepare_calls();
	if (ret)
		return ret;

	ret = luo_do_subsystems_prepare_calls();
	if (ret)
		luo_do_files_cancel_calls();

	return ret;
}

static int luo_do_reboot_calls(void)
{
	int ret;

	ret = luo_do_files_reboot_calls();
	if (ret)
		return ret;

	ret = luo_do_subsystems_reboot_calls();
	if (ret)
		luo_do_files_cancel_calls();

	return ret;
}

static void luo_do_finish_calls(void)
{
	luo_do_files_finish_calls();
	luo_do_subsystems_finish_calls();
}

static void luo_do_cancel_calls(void)
{
	luo_do_files_cancel_calls();
	luo_do_subsystems_cancel_calls();
}

static int __luo_prepare(struct kho_serialization *ser)
{
	int ret;

	if (down_write_killable(&luo_state_rwsem)) {
		pr_warn("[prepare] event canceled by user\n");
		return -EAGAIN;
	}

	if (!IS_STATE(LIVEUPDATE_STATE_NORMAL)) {
		pr_warn("Can't switch to [%s] from [%s] state\n",
			luo_state_str[LIVEUPDATE_STATE_PREPARED],
			LUO_STATE_STR);
		ret = -EINVAL;
		goto exit_unlock;
	}

	ret = luo_fdt_setup(ser);
	if (ret)
		goto exit_unlock;

	luo_kho_ser = ser;
	ret = luo_do_prepare_calls();
	luo_kho_ser = NULL;
	if (ret)
		goto exit_unlock;

	luo_set_state(LIVEUPDATE_STATE_PREPARED);

exit_unlock:
	up_write(&luo_state_rwsem);

	return ret;
}

static int __luo_cancel(struct kho_serialization *ser)
{
	if (down_write_killable(&luo_state_rwsem)) {
		pr_warn("[cancel] event canceled by user\n");
		return -EAGAIN;
	}

	if (!IS_STATE(LIVEUPDATE_STATE_PREPARED)) {
		pr_warn("Can't switch to [%s] from [%s] state\n",
			luo_state_str[LIVEUPDATE_STATE_NORMAL],
			LUO_STATE_STR);
		up_write(&luo_state_rwsem);

		return -EINVAL;
	}

	luo_do_cancel_calls();
	luo_fdt_destroy(ser);
	luo_set_state(LIVEUPDATE_STATE_NORMAL);

	up_write(&luo_state_rwsem);

	return 0;
}

static int luo_kho_prepare_notifier(struct notifier_block *self,
				    unsigned long cmd, void *v)
{
	int ret;

	switch (cmd) {
	case KEXEC_KHO_FINALIZE:
		ret = __luo_prepare((struct kho_serialization *)v);
		break;
	case KEXEC_KHO_ABORT:
		ret = __luo_cancel((struct kho_serialization *)v);
		break;
	default:
		ret = NOTIFY_BAD;
	}

	return notifier_from_errno(ret);
}

static struct notifier_block luo_kho_prepare_notifier_nb = {
	.notifier_call = luo_kho_prepare_notifier,
};

/**
 * luo_prepare - Initiate the live update preparation phase.
 *
 * This function is called to begin the live update process. It attempts to
 * transition the luo to the ``LIVEUPDATE_STATE_PREPARED`` state.
 *
 * If the calls complete successfully, the orchestrator state is set
 * to ``LIVEUPDATE_STATE_PREPARED``. If any  call fails a
 * ``LIVEUPDATE_CANCEL`` is sent to roll back any actions.
 *
 * @return 0 on success, ``-EAGAIN`` if the state change was cancelled by the
 * user while waiting for the lock, ``-EINVAL`` if the orchestrator is not in
 * the normal state, or a negative error code returned by the calls.
 */
int luo_prepare(void)
{
	return kho_finalize();
}

/**
 * luo_reboot() - Initiate the final reboot notification phase for live update.
 *
 * Attempts to transition the live update orchestrator state from
 * %LIVEUPDATE_STATE_PREPARED to %LIVEUPDATE_STATE_FROZEN. This function is
 * typically called just before the actual reboot system call (e.g., kexec)
 * is invoked, either directly by the orchestration tool or potentially from
 * within the reboot syscall path itself.
 *
 * Based on the outcome of the notification process:
 * - If luo_do_reboot_calls() returns 0 (all callbacks succeeded), the state
 * is set to %LIVEUPDATE_STATE_FROZEN using luo_set_state(), indicating
 * readiness for the imminent kexec.
 * - If luo_do_reboot_calls() returns a negative error code (a callback
 * failed), the state is reverted to %LIVEUPDATE_STATE_NORMAL using
 * luo_set_state() to cancel the live update attempt.
 *
 * @return  0: Success. Negative error otherwise. State is reverted to
 * %LIVEUPDATE_STATE_NORMAL in case of an error.
 */
int luo_reboot(void)
{
	int ret;

	if (down_write_killable(&luo_state_rwsem)) {
		pr_warn("[reboot] event canceled by user\n");
		return -EAGAIN;
	}

	if (!IS_STATE(LIVEUPDATE_STATE_PREPARED)) {
		pr_warn("Can't switch to [%s] from [%s] state\n",
			luo_state_str[LIVEUPDATE_STATE_FROZEN],
			LUO_STATE_STR);
		up_write(&luo_state_rwsem);

		return -EINVAL;
	}

	ret = luo_do_reboot_calls();
	if (!ret)
		luo_set_state(LIVEUPDATE_STATE_FROZEN);
	else
		luo_set_state(LIVEUPDATE_STATE_NORMAL);

	up_write(&luo_state_rwsem);

	return ret;
}

/**
 * luo_finish - Finalize the live update process in the new kernel.
 *
 * This function is called  after a successful live update reboot into a new
 * kernel, once the new kernel is ready to transition to the normal operational
 * state. It signals the completion of the live update sequence to subsystems.
 *
 * It first attempts to acquire the write lock for the orchestrator state.
 *
 * Then, it checks if the system is in the ``LIVEUPDATE_STATE_UPDATED`` state.
 * If not, it logs a warning and returns ``-EINVAL``.
 *
 * If the state is correct, it triggers the ``LIVEUPDATE_FINISH`` notifier
 * chain. Note that the return value of the notifier is intentionally ignored as
 * finish callbacks must not fail. Finally, the orchestrator state is
 * transitioned back to ``LIVEUPDATE_STATE_NORMAL``, indicating the end of the
 * live update process.
 *
 * @return 0 on success, ``-EAGAIN`` if the state change was cancelled by the
 * user while waiting for the lock, or ``-EINVAL`` if the orchestrator is not in
 * the updated state.
 */
int luo_finish(void)
{
	if (down_write_killable(&luo_state_rwsem)) {
		pr_warn("[finish] event canceled by user\n");
		return -EAGAIN;
	}

	if (!IS_STATE(LIVEUPDATE_STATE_UPDATED)) {
		pr_warn("Can't switch to [%s] from [%s] state\n",
			luo_state_str[LIVEUPDATE_STATE_NORMAL],
			LUO_STATE_STR);
		up_write(&luo_state_rwsem);

		return -EINVAL;
	}

	luo_do_finish_calls();
	luo_set_state(LIVEUPDATE_STATE_NORMAL);

	up_write(&luo_state_rwsem);

	return 0;
}

/**
 * luo_cancel - Cancel the ongoing live update preparation or reboot states.
 *
 * This function is called to abort a live update that is currently in the
 * ``LIVEUPDATE_STATE_PREPARED`` state.
 *
 * If the state is correct, it triggers the ``LIVEUPDATE_CANCEL`` notifier chain
 * to allow subsystems to undo any actions performed during the prepare or
 * reboot phase. Finally, the orchestrator state is transitioned back to
 * ``LIVEUPDATE_STATE_NORMAL``.
 *
 * @return 0 on success, or ``-EAGAIN`` if the state change was cancelled by the
 * user while waiting for the lock.
 */
int luo_cancel(void)
{
	return kho_abort();
}

static int __init luo_startup(void)
{
	phys_addr_t fdt_phys;
	u8 *state_ptr;
	int prop_len;
	int ret;

	if (!kho_is_enabled()) {
		if (luo_enabled)
			pr_warn("disabling because KHO is disabled\n");
		luo_enabled = false;
		return 0;
	}

	ret = register_kho_notifier(&luo_kho_prepare_notifier_nb);
	if (ret) {
		luo_enabled = false;
		pr_warn("Faile to register with KHO [%d]\n", ret);
	}

	/*
	 * Retrieve LUO subtree, and verify its format.  Panic in case of
	 * exceptions, since machine devices and memory is in unpredictable
	 * state.
	 */
	ret = kho_retrieve_subtree(LUO_KHO_ENTRY_NAME, &fdt_phys);
	if (ret) {
		if (ret != -ENOENT) {
			panic("failed to retrieve FDT '%s' from KHO: %d\n",
			      LUO_KHO_ENTRY_NAME, ret);
		}
		__luo_set_state(LIVEUPDATE_STATE_NORMAL);

		return 0;
	}

	luo_fdt_in = __va(fdt_phys);
	ret = fdt_node_check_compatible(luo_fdt_in, 0, LUO_COMPATIBLE);
	if (ret) {
		panic("FDT '%s' is incompatible with '%s' [%d]\n",
		      LUO_KHO_ENTRY_NAME, LUO_COMPATIBLE, ret);
	}

	state_ptr = (u8 *)fdt_getprop(luo_fdt_in, 0, LUO_STATE, &prop_len);
	if (!state_ptr || prop_len != sizeof(u8))
		panic("Could not find 'state' property\n");

	if (*state_ptr != LIVEUPDATE_STATE_FROZEN)
		panic("Unexpected state value[%d]\n", *state_ptr);

	__luo_set_state(LIVEUPDATE_STATE_UPDATED);
	luo_files_startup(luo_fdt_in);
	luo_subsystems_startup(luo_fdt_in);

	return 0;
}
early_initcall(luo_startup);

/* Public Functions */

/**
 * liveupdate_reboot() - Kernel reboot notifier for live update final
 * serialization.
 *
 * This function is invoked directly from the reboot() syscall pathway if a
 * reboot is initiated while the live update state is %LIVEUPDATE_STATE_PREPARED
 * (i.e., if the user did not explicitly trigger the frozen state). It handles
 * the implicit transition into the final frozen state.
 *
 * It triggers the %LIVEUPDATE_REBOOT event callbacks for participating
 * subsystems. These callbacks must perform final state saving very quickly as
 * they execute during the blackout period just before kexec.
 *
 * If any %LIVEUPDATE_REBOOT callback fails, this function triggers the
 * %LIVEUPDATE_CANCEL event for all participants to revert their state, aborts
 * the live update, and returns an error.
 */
int liveupdate_reboot(void)
{
	if (!IS_STATE(LIVEUPDATE_STATE_PREPARED))
		return 0;

	return luo_reboot();
}
EXPORT_SYMBOL_GPL(liveupdate_reboot);

/**
 * liveupdate_state_updated - Check if the system is in the live update
 * 'updated' state.
 *
 * This function checks if the live update orchestrator is in the
 * ``LIVEUPDATE_STATE_UPDATED`` state. This state indicates that the system has
 * successfully rebooted into a new kernel as part of a live update, and the
 * preserved devices are expected to be in the process of being reclaimed.
 *
 * This is typically used by subsystems during early boot of the new kernel
 * to determine if they need to attempt to restore state from a previous
 * live update.
 *
 * @return true if the system is in the ``LIVEUPDATE_STATE_UPDATED`` state,
 * false otherwise.
 */
bool liveupdate_state_updated(void)
{
	return IS_STATE(LIVEUPDATE_STATE_UPDATED);
}
EXPORT_SYMBOL_GPL(liveupdate_state_updated);

/**
 * liveupdate_state_normal - Check if the system is in the live update 'normal'
 * state.
 *
 * This function checks if the live update orchestrator is in the
 * ``LIVEUPDATE_STATE_NORMAL`` state. This state indicates that no live update
 * is in progress. It represents the default operational state of the system.
 *
 * This can be used to gate actions that should only be performed when no
 * live update activity is occurring.
 *
 * @return true if the system is in the ``LIVEUPDATE_STATE_NORMAL`` state,
 * false otherwise.
 */
bool liveupdate_state_normal(void)
{
	return IS_STATE(LIVEUPDATE_STATE_NORMAL);
}
EXPORT_SYMBOL_GPL(liveupdate_state_normal);

/**
 * liveupdate_enabled - Check if the live update feature is enabled.
 *
 * This function returns the state of the live update feature flag, which
 * can be controlled via the ``liveupdate`` kernel command-line parameter.
 *
 * @return true if live update is enabled, false otherwise.
 */
bool liveupdate_enabled(void)
{
	return luo_enabled;
}
EXPORT_SYMBOL_GPL(liveupdate_enabled);

/**
 * liveupdate_preserve_folio - Preserve folio during prepare phase
 * @folio: Memory to be preserve.
 *
 * Preserve a folio during LUO prepare phase.
 *
 * @return 0 on success, or negative error code
 */
int liveupdate_preserve_folio(struct folio *folio)
{
	if (!luo_kho_ser) {
		pr_err("Memory preservation is allowed only during prepare phase\n");
		return -EINVAL;
	}

	return kho_preserve_folio(luo_kho_ser, folio);
}
EXPORT_SYMBOL_GPL(liveupdate_preserve_folio);

/**
 * liveupdate_preserve_phys - Preserve physical address during prepare phase
 * @phys: Start address of the physical region.
 * @size: Length of the physical region.
 *
 * Preserve a physical range from @phys to @size during prepare phase.
 *
 * @return 0 on success, or negative error code
 */
int liveupdate_preserve_phys(phys_addr_t phys, size_t size)
{
	if (!luo_kho_ser) {
		pr_err("Memory preservation is allowed only during prepare phase\n");
		return -EINVAL;
	}

	return kho_preserve_phys(luo_kho_ser, phys, size);
}
EXPORT_SYMBOL_GPL(liveupdate_preserve_phys);
