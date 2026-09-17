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

/* ------------------------------------------------------------------ */
/* Weak hooks (the only OS-specific part of O1).                       */
/*                                                                     */
/* After reading a byte and clearing its status bit, O1 feeds the byte  */
/* to the OS's existing KBD/mouse handler. Since this repo is           */
/* OS-agnostic, these are weak hooks with default no-op implementations  */
/* in virtual_ps2_reader.c; the OS (or the Layer 2 stub harness)         */
/* overrides them to route the byte into its own input path.            */
/*                                                                     */
/* NOTE: the weak attribute lives on the DEFINITIONS in                 */
/* virtual_ps2_reader.c, not on these declarations. If the declarations  */
/* were marked weak, a strong override in a test/OS would inherit weak  */
/* (GCC propagates weak from a prior declaration to the definition),    */
/* leaving two weak definitions that the linker resolves arbitrarily.   */
/* Marking only the definitions weak lets a strong override win.        */
/* ------------------------------------------------------------------ */

/* Feed one keyboard scancode byte to the OS's keyboard handler. */
void adapter_feed_kbd_byte(UINT8 byte);

/* Feed one mouse packet byte to the OS's mouse handler. */
void adapter_feed_mouse_byte(UINT8 byte);

/* Write `value` to the local APIC EOI register (MMIO). The firmware build
 * uses the default volatile store; the Layer 2 stub harness overrides it
 * to record the write and assert it was 0 (0xFEE000B0 is unmapped on the
 * host). */
void adapter_apic_eoi_write(UINT32 value);

#endif /* ADAPTER_H */
