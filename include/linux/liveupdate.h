/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#ifndef _LINUX_LIVEUPDATE_H
#define _LINUX_LIVEUPDATE_H

#include <linux/bug.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <uapi/linux/liveupdate.h>

/**
 * enum liveupdate_event - Events that trigger live update callbacks.
 * @LIVEUPDATE_PREPARE: Sent when the live update process is initiated via
 *                      a sysfs by writing '1' into
 *                      ``/sys/kernel/liveupdate/prepare``. This happens
 *                      *before* the blackout window. Subsystems should prepare
 *                      for an upcoming reboot by serializing their states.
 *                      However, it must be considered that user applications,
 *                      e.g. virtual machines are still running during this
 *                      phase.
 * @LIVEUPDATE_REBOOT:  Sent from the reboot() syscall, when the old kernel is
 *                      on its way out. This is the final opportunity for
 *                      subsystems to save any state that must persist across
 *                      the reboot. Callbacks for this event are part of the
 *                      blackout window and must be fast.
 * @LIVEUPDATE_FINISH:  Sent in the newly booted kernel after a successful live
 *                      update and *after* the blackout window. This event is
 *                      initiated by writing '1' into
 *                      ``/sys/kernel/liveupdate/prepare``. Subsystems should
 *                      perform any final cleanup during this phase. This phase
 *                      also provides an opportunity to clean up devices that
 *                      were preserved but never explicitly reclaimed during the
 *                      live update process. State restoration should have
 *                      already occurred before this event. Callbacks for this
 *                      event must not fail. The completion of this call
 *                      transitions the machine from ``updated`` to ``normal``
 *                      state.
 * @LIVEUPDATE_CANCEL:  Sent if the LIVEUPDATE_PREPARE or LIVEUPDATE_REBOOT
 *                      stage fails. Subsystems should revert any actions taken
 *                      during the corresponding prepare phase. Callbacks for
 *                      this event must not fail.
 *
 * These events represent the different stages and actions within the live
 * update process that subsystems (like device drivers and bus drivers)
 * need to be aware of to correctly serialize and restore their state.
 *
 */
enum liveupdate_event {
	LIVEUPDATE_PREPARE,
	LIVEUPDATE_REBOOT,
	LIVEUPDATE_FINISH,
	LIVEUPDATE_CANCEL,
};

/* Forward declaration needed if definition isn't included */
struct file;

/**
 * struct liveupdate_filesystem - Represents a handler for a live-updatable
 * filesystem/file type.
 * @prepare:       Optional. Saves state for a specific file instance (@file,
 *                 @arg) before update, potentially returning value via @data.
 *                 Returns 0 on success, negative errno on failure.
 * @reboot:        Optional. Performs final actions just before kernel
 *                 transition, potentially reading/updating the handle via
 *                 @data.
 *                 Returns 0 on success, negative errno on failure.
 * @cancel:        Optional. Cleans up state/resources if update is aborted
 *                 after prepare/reboot succeeded, using the @data handle (by
 *                 value) from the successful prepare. Returns void.
 * @finish:        Optional. Performs final cleanup in the new kernel using the
 *                 preserved @data handle (by value). Returns void.
 * @retrieve:      Retrieve the preserved file. Must be called before finish.
 * @compatible:    The compatibility string (e.g., "memfd-v1", "vfiofd-v1")
 *                 that uniquely identifies the filesystem or file type this
 *                 handler supports. This is matched against the compatible
 *                 string associated with individual &struct liveupdate_file
 *                 instances.
 * @can_preserve:  callback to determine if @file with associated context (@arg)
 *                 can be preserved by this handler.
 *                 Return bool (true if preservable, false otherwise).
 * @arg:           An opaque pointer to implementation-specific context data
 *                 associated with this filesystem handler registration.
 * @list:          used for linking this handler instance into a global list of
 *                 registered filesystem handlers.
 * @refcnt:        Number of file descriptors are currently preserved.
 *
 * Modules that want to support live update for specific file types should
 * register an instance of this structure. LUO uses this registration to
 * determine if a given file can be preserved and to find the appropriate
 * operations to manage its state across the update.
 */
struct liveupdate_filesystem {
	int (*prepare)(struct file *file, void *arg, u64 *data);
	int (*reboot)(struct file *file, void *arg, u64 *data);
	void (*cancel)(struct file *file, void *arg, u64 data);
	void (*finish)(struct file *file, void *arg, u64 data, bool reclaimed);
	int (*retrieve)(void *arg, u64 data, struct file **file);
	char *compatible;
	bool (*can_preserve)(struct file *file, void *arg);
	void *arg;
	struct list_head list;
	u64 refcnt;
};

/**
 * struct liveupdate_subsystem - Represents a subsystem participating in LUO
 * @prepare:      Optional. Called during LUO prepare phase. Should perform
 *                preparatory actions and can store a u64 handle/state
 *                via the 'data' pointer for use in later callbacks.
 *                Return 0 on success, negative error code on failure.
 * @reboot:       Optional. Called during LUO reboot event phase (before
 *                actual jump to new kernel). Should perform final state saving
 *                actions and can update the u64 handle/state via the 'data'
 *                pointer. Return 0 on success, negative error code on failure.
 * @cancel:       Optional. Called if the live update process is canceled after
 *                prepare (or reboot) was called. Receives the u64 data
 *                set by prepare/reboot. Used for cleanup.
 * @finish:       Optional. Called after the live update is finished in the new
 *                kernel (or potentially during cancellation after reboot?).
 *                Receives the u64 data set by prepare/reboot. Used for cleanup.
 * @name:         Mandatory. Unique name identifying the subsystem.
 * @arg:          Add this argument to callback functions.
 * @list:         List head used internally by LUO. Should not be modified by
 *                caller after registration.
 * @private_data: For LUO internal use, cached value of data field.
 */
struct liveupdate_subsystem {
	int (*prepare)(void *arg, u64 *data);
	int (*reboot)(void *arg, u64 *data);
	void (*cancel)(void *arg, u64 data);
	void (*finish)(void *arg, u64 data);
	char *name;
	void *arg;
	struct list_head list;
	u64 private_data;
};

#ifdef CONFIG_LIVEUPDATE

/* Return true if live update orchestrator is enabled */
bool liveupdate_enabled(void);

/* Called during reboot to tell participants to complete serialization */
int liveupdate_reboot(void);

/*
 * Return true if machine is in updated state (i.e. live update boot in
 * progress)
 */
bool liveupdate_state_updated(void);

/*
 * Return true if machine is in normal state (i.e. no live update in progress).
 */
bool liveupdate_state_normal(void);

int liveupdate_preserve_folio(struct folio *folio);
int liveupdate_preserve_phys(phys_addr_t phys, size_t size);

int liveupdate_register_subsystem(struct liveupdate_subsystem *h);
int liveupdate_unregister_subsystem(struct liveupdate_subsystem *h);
int liveupdate_get_subsystem_data(struct liveupdate_subsystem *h, u64 *data);

int liveupdate_register_filesystem(struct liveupdate_filesystem *h);
int liveupdate_unregister_filesystem(struct liveupdate_filesystem *h);

#else /* CONFIG_LIVEUPDATE */

static inline int liveupdate_reboot(void)
{
	return 0;
}

static inline int liveupdate_register_subsystem(struct liveupdate_subsystem *h)
{
	return 0;
}

static inline int liveupdate_unregister_subsystem(struct liveupdate_subsystem *h)
{
	return 0;
}

static inline int liveupdate_register_filesystem(struct liveupdate_filesystem *h)
{
	return 0;
}

static inline int liveupdate_unregister_filesystem(struct liveupdate_filesystem *h)
{
	return 0;
}

#endif /* CONFIG_LIVEUPDATE */
#endif /* _LINUX_LIVEUPDATE_H */
