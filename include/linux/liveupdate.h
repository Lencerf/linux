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

/**
 * enum liveupdate_state - Defines the possible states of the live update
 * orchestrator.
 * @LIVEUPDATE_STATE_NORMAL:         Default state, no live update in progress.
 * @LIVEUPDATE_STATE_PREPARED:       Live update is prepared for reboot; the
 *                                   LIVEUPDATE_PREPARE callbacks have completed
 *                                   successfully.
 *                                   Devices might operate in a limited state
 *                                   for example the participating devices might
 *                                   not be allowed to unbind, and also the
 *                                   setting up of new DMA mappings might be
 *                                   disabled in this state.
 * @LIVEUPDATE_STATE_FROZEN:         The final reboot event
 *                                   (%LIVEUPDATE_REBOOT) has been sent, and the
 *                                   system is performing its final state saving
 *                                   within the "blackout window". User
 *                                   workloads must be suspended. The actual
 *                                   reboot (kexec) into the new kernel is
 *                                   imminent.
 * @LIVEUPDATE_STATE_UPDATED:        The system has rebooted into a new kernel
 *                                   via live update the system is now running
 *                                   the new kernel, awaiting the finish event.
 *
 * These states track the progress and outcome of a live update operation.
 */
enum liveupdate_state  {
	LIVEUPDATE_STATE_NORMAL = 0,
	LIVEUPDATE_STATE_PREPARED = 1,
	LIVEUPDATE_STATE_FROZEN = 2,
	LIVEUPDATE_STATE_UPDATED = 3,
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

#else /* CONFIG_LIVEUPDATE */

static inline int liveupdate_reboot(void)
{
	return 0;
}

#endif /* CONFIG_LIVEUPDATE */
#endif /* _LINUX_LIVEUPDATE_H */
