/*
 * tdm.h - Time-Division Multiplexing context switch (bridge <-> OS task).
 *
 * The bridge and the OS background task share the highest core via
 * time-division multiplexing. A timer ISR on that core fires at each slot
 * boundary and alternates between the bridge context and the OS task
 * context. Each switch saves/restores the callee-saved registers and the
 * stack pointer - a minimal cooperative context switch, ring 0, same
 * address space (no memory protection).
 *
 * Slot length: 2 ms (see TDM_SLOT_US). This gives the bridge enough time to
 * poll the two USB interrupt endpoints and write into the virtual port
 * region each slot, while leaving the OS background task the majority of the
 * core. See docs/implementation-spec.md section 5.
 */

#ifndef TDM_H
#define TDM_H

#include <efi.h>

/* TDM slot length in microseconds. 2 ms is comfortably above the USB
 * interrupt polling interval (keyboards/mice poll at 1-10 ms) while keeping
 * the OS background task responsive. */
#define TDM_SLOT_US  2000

/* Save the current (bridge) context and switch to the OS task context.
 * Called from the timer ISR on the highest core at slot boundaries. */
void tdm_switch_to_os(void);

/* Save the OS task context and switch back to the bridge context. */
void tdm_switch_to_bridge(void);

/* Install the timer ISR that drives the TDM round-robin on the highest core. */
void tdm_install_timer(void);

#endif /* TDM_H */
