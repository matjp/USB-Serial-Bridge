/*
 * adapter.h - Internal interface for the per-OS input read (Phase 2).
 *
 * The OS's PS/2 driver reads the virtual 8042 port region (O1) from its
 * existing keyboard/mouse ISRs, which are triggered by the virtual interrupt
 * the bridge sends (see include/virtual_ps2.h). The OS reuses its existing
 * Set 1 decoder and 3-byte mouse-packet parser unchanged. See
 * docs/architecture.md section 7.2.
 *
 * O1 is the consumer side of the "virtual IRQ + virtual ports" design:
 *   - The bridge writes a byte, sets the status bit, then sends a virtual
 *     interrupt (IPI) on the device's vector.
 *   - The OS's ISR for that vector fires, reads the virtual data port
 *     (read-and-clear), feeds the byte to the OS's existing handler, and
 *     EOIs the local APIC (the virtual IRQ is APIC-sourced, not PIC-sourced).
 */

#ifndef ADAPTER_H
#define ADAPTER_H

#include <efi.h>
#include <virtual_ps2.h>

/* O1: Read the virtual 8042 port region (status/data, read-and-clear) and
 * feed the byte to the OS's existing KBD/mouse handler. Called from the
 * OS's keyboard/mouse ISR after the bridge sends the virtual IRQ. */
void adapter_read_virtual_ps2(void);

/* O1: EOI the local APIC. The virtual IRQ is delivered as an IPI via the
 * local APIC, so the ISR must acknowledge it by writing 0 to the LAPIC EOI
 * register (the real 8042 path only EOIs the 8259 PIC). This is the one
 * OS-side accommodation for the virtual IRQ. */
void adapter_apic_eoi(void);

#endif /* ADAPTER_H */
