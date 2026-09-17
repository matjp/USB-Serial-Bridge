/*
 * virtual_ps2.h - Virtual 8042 port region + virtual IRQ (the OS-agnostic
 * "virtual PS/2 hardware" interface). See docs/architecture.md section 6.
 *
 * This is the alternative to the mailbox ABI: instead of a ring buffer that
 * a per-OS adapter drains, the bridge presents a small shared-memory region
 * that MIRRORS the real 8042 controller's two I/O registers, PLUS a virtual
 * interrupt that triggers the OS's existing ISR-driven input path.
 *
 *   - VIRTUAL_PS2_STATUS  mirrors the 8042 status register (I/O port 0x64)
 *   - VIRTUAL_PS2_DATA    mirrors the 8042 data register   (I/O port 0x60)
 *   - VIRTUAL_PS2_IRQ_*   the vectors on which the bridge delivers a virtual
 *                         interrupt (an IPI) so the OS's existing keyboard /
 *                         mouse ISRs fire and read the virtual data port.
 *
 * The OS's existing PS/2 driver reads these two registers. To use the
 * virtual port, the OS replaces its `in 0x60` / `in 0x64` instructions with
 * loads from these fixed addresses, and its ISRs EOI the local APIC (see
 * below). The OS reuses its existing Set 1 decoder and 3-byte mouse-packet
 * parser unchanged.
 *
 * Faithful 8042 semantics (verified against the reference OS source):
 *   - BOTH keyboard and mouse data arrive through the SINGLE data register
 *     (0x60), disambiguated by the status register bits:
 *         bit0 = keyboard output buffer full
 *         bit5 = mouse output buffer full
 *   - Only ONE byte is in flight at a time (the 8042 has a single output
 *     buffer). The bridge writes a byte, sets the status bit, and does not
 *     write the next byte until the OS has consumed it (status bit clear).
 *   - On the real 8042, reading the data register clears the output-buffer-
 *     full bit. A pure load from VIRTUAL_PS2_DATA does NOT clear it, so the
 *     OS's data read must be a READ-AND-CLEAR (load the byte, then clear the
 *     status bit). This is the honest minimal OS change (2 lines per data
 *     read site, not 1).
 *
 * Virtual IRQ (the ISR-driven path):
 *   - The reference OS (TempleOS) drives keyboard/mouse input PRIMARILY from
 *     ISRs: IRQKbd (vector 0x21 / IRQ1) and IRQMsHard (vector 0x2C / IRQ12)
 *     read the data port. The polled path (KbdMsHndlr) is only a fallback
 *     gated on `!irqs_working`.
 *   - To use that ISR path with a USB-only machine, the bridge delivers a
 *     VIRTUAL interrupt: after writing a byte and setting the status bit, it
 *     sends an inter-processor interrupt (IPI) via the local APIC ICR to the
 *     OS core (core 0) on the SAME vector the OS already uses for that device
 *     (0x21 keyboard, 0x2C mouse). The OS's existing ISR fires and reads the
 *     virtual data port.
 *   - The one OS-side accommodation: a real 8042 IRQ is PIC-sourced, so the
 *     OS's ISRs EOI the 8259 PIC (OutU8(0x20,0x20)). A virtual IRQ is
 *     APIC-sourced (an IPI), so the ISR must ALSO EOI the local APIC
 *     (write 0 to the LAPIC EOI register). This is a one-line addition per
 *     ISR — smaller than suppressing the IRQ path and using the polled
 *     fallback.
 *
 * The bridge (producer, highest core) writes bytes, sets status bits, and
 * sends the virtual IRQ; the OS (consumer, core 0) reads status/data in its
 * ISR and EOIs the APIC. x86 is cache-coherent (MESI); the producer uses
 * mfence() before setting the status bit so the data byte is visible before
 * the "ready" flag, and sends the IPI only after the status bit is set.
 */

#ifndef VIRTUAL_PS2_H
#define VIRTUAL_PS2_H

#include <efi.h>

/* Fixed physical address of the virtual 8042 port region. Placed in the
 * reserved region, clear of the topology/fault/status pointer slots
 * (0x10000000..0x10000028). The OS must map this address (identity-mapped
 * OSes see it directly; others carve it out of their memory map). */
#define VIRTUAL_PS2_BASE    0x10000030u

/* Register offsets within the region (mirror the 8042 I/O ports). */
#define VIRTUAL_PS2_STATUS  (VIRTUAL_PS2_BASE + 0)   /* U8, mirrors 0x64 */
#define VIRTUAL_PS2_DATA    (VIRTUAL_PS2_BASE + 1)   /* U8, mirrors 0x60 */

/* Status-register bits (mirror the 8042 status register). */
#define VIRTUAL_PS2_STAT_KBD    0x01u   /* bit0: keyboard output buffer full */
#define VIRTUAL_PS2_STAT_MOUSE  0x20u   /* bit5: mouse output buffer full */

/* All status bits (used to test whether ANY byte is pending). */
#define VIRTUAL_PS2_STAT_ANY    (VIRTUAL_PS2_STAT_KBD | VIRTUAL_PS2_STAT_MOUSE)

/* ------------------------------------------------------------------ */
/* Virtual IRQ vectors.                                                */
/*                                                                     */
/* The bridge delivers a virtual interrupt (an IPI via the local APIC   */
/* ICR) to the OS core on the SAME vector the OS already uses for that  */
/* device, so the OS's existing ISR fires and reads the virtual data    */
/* port. These match the reference OS (TempleOS) vectors: IRQ1 = 0x21   */
/* (keyboard), IRQ12 = 0x2C (mouse).                                    */
/* ------------------------------------------------------------------ */
#define VIRTUAL_PS2_IRQ_KBD    0x21u   /* vector for keyboard ISR (IRQ1)  */
#define VIRTUAL_PS2_IRQ_MOUSE  0x2Cu   /* vector for mouse ISR (IRQ12)   */

/* ------------------------------------------------------------------ */
/* Weak accessors.                                                     */
/*                                                                     */
/* The bridge writes to the fixed addresses above and sends the virtual */
/* IRQ. On the host these are unmapped, so the host test overrides these */
/* weak functions with a mock register file (same pattern as            */
/* xhci_read32/xhci_write32 in xhci.c).                                 */
/* ------------------------------------------------------------------ */

/* Read the virtual status register. */
__attribute__((weak)) UINT8 virtual_ps2_read_status(void);

/* Write one byte to the virtual data register. */
__attribute__((weak)) void virtual_ps2_write_data(UINT8 byte);

/* Set the given status bits (OR into the status register). */
__attribute__((weak)) void virtual_ps2_set_status(UINT8 bits);

/* Send a virtual interrupt (IPI) to the OS core on the given vector.
 * Called by the bridge AFTER the data byte is written and the status bit
 * is set, so the OS's ISR for that vector fires and reads the data. */
__attribute__((weak)) void virtual_ps2_send_irq(UINT8 vector);

#endif /* VIRTUAL_PS2_H */
