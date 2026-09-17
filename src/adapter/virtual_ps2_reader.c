/*
 * virtual_ps2_reader.c - O1: consumer-side virtual 8042 port reader.
 *
 * Reads the virtual 8042 port region (see include/virtual_ps2.h) from the
 * OS's existing keyboard/mouse ISR, which the bridge's virtual IRQ (an IPI
 * on the device's vector) triggers. This is the consumer half of the
 * "virtual IRQ + virtual ports" design: the bridge (producer, highest core)
 * writes a byte, sets the status bit, then sends the virtual IRQ; the OS's
 * ISR for that vector calls adapter_read_virtual_ps2() here.
 *
 * Faithful 8042 semantics:
 *   - BOTH keyboard and mouse data arrive through the SINGLE data register,
 *     disambiguated by the status register bits (bit0 = keyboard, bit5 =
 *     mouse).
 *   - The data read is a READ-AND-CLEAR: a pure load from VIRTUAL_PS2_DATA
 *     does NOT clear the status bit (unlike the real 8042 hardware), so the
 *     reader loads the byte and then clears the status bit.
 *   - Keyboard is prioritized over mouse (matches the OS's poll order).
 *
 * Virtual IRQ / APIC EOI:
 *   - The virtual IRQ is delivered as an IPI via the local APIC, so the ISR
 *     must EOI the local APIC (write 0 to the LAPIC EOI register) in
 *     addition to the PIC EOI the OS already does for a real 8042 IRQ. This
 *     is the one OS-side accommodation for the virtual IRQ.
 *
 * O1 is OS-specific only in the final feed step (which OS handler to call).
 * The read-and-clear of the virtual port and the APIC EOI are
 * OS-independent. To keep this repo OS-agnostic, the feed step is a weak
 * hook (adapter_feed_kbd_byte / adapter_feed_mouse_byte) that the OS (or
 * the Layer 2 stub harness) overrides to route the byte into its own input
 * path.
 */

#include <efi.h>
#include <virtual_ps2.h>

#include "adapter.h"

/* ------------------------------------------------------------------ */
/* Weak feed hooks (the only OS-specific part of O1).                  */
/*                                                                     */
/* After reading a byte and clearing its status bit, O1 feeds the byte  */
/* to the OS's existing KBD/mouse handler. Since this repo is           */
/* OS-agnostic, these are weak hooks with default no-op implementations  */
/* here; the OS (or the Layer 2 stub harness) overrides them to route    */
/* the byte into its own input path (e.g. a fake PutKey/KBD buffer).     */
/* ------------------------------------------------------------------ */

/* Feed one keyboard scancode byte to the OS's keyboard handler. */
__attribute__((weak)) void
adapter_feed_kbd_byte(UINT8 byte)
{
    (void)byte;   /* default no-op; overridden by the OS / Layer 2 stub */
}

/* Feed one mouse packet byte to the OS's mouse handler. */
__attribute__((weak)) void
adapter_feed_mouse_byte(UINT8 byte)
{
    (void)byte;   /* default no-op; overridden by the OS / Layer 2 stub */
}

/* ------------------------------------------------------------------ */
/* Weak EOI-write hook.                                                */
/*                                                                     */
/* The local APIC EOI register (0xFEE000B0) is unmapped on the host, so */
/* the actual MMIO store is routed through this weak function. The      */
/* firmware build uses the default volatile store; the Layer 2 stub     */
/* harness overrides it to record the write and assert it was 0.        */
/* ------------------------------------------------------------------ */

/* Write `value` to the local APIC EOI register (MMIO). */
__attribute__((weak)) void
adapter_apic_eoi_write(UINT32 value)
{
    /* Local APIC base 0xFEE00000; EOI register at offset 0x0B0. */
    *(volatile UINT32 *)(0xFEE00000u + 0x0B0u) = value;
}

/* ------------------------------------------------------------------ */
/* O1: read the virtual 8042 port region (read-and-clear) and feed the  */
/* byte to the OS's existing KBD/mouse handler. Called from the OS's    */
/* keyboard/mouse ISR after the bridge sends the virtual IRQ.           */
/* ------------------------------------------------------------------ */

void
adapter_read_virtual_ps2(void)
{
    UINT8 status;
    UINT8 byte;

    /* Read the virtual status register. If no status bit is set, there is
     * no pending byte - return without touching anything. */
    status = virtual_ps2_read_status();
    if (!(status & VIRTUAL_PS2_STAT_ANY))
        return;

    /* Read the data byte, then READ-AND-CLEAR the status bit (a pure load
     * does NOT clear it, unlike the real 8042). Keyboard is prioritized
     * over mouse (matches the OS's poll order). */
    byte = virtual_ps2_read_data();

    if (status & VIRTUAL_PS2_STAT_KBD) {
        virtual_ps2_clear_status(VIRTUAL_PS2_STAT_KBD);
        adapter_feed_kbd_byte(byte);
    } else if (status & VIRTUAL_PS2_STAT_MOUSE) {
        virtual_ps2_clear_status(VIRTUAL_PS2_STAT_MOUSE);
        adapter_feed_mouse_byte(byte);
    }
}

/* ------------------------------------------------------------------ */
/* O1: EOI the local APIC.                                             */
/*                                                                     */
/* The virtual IRQ is delivered as an IPI via the local APIC, so the    */
/* ISR must acknowledge it by writing 0 to the LAPIC EOI register (the  */
/* real 8042 path only EOIs the 8259 PIC). This is the one OS-side      */
/* accommodation for the virtual IRQ. The actual MMIO store is routed   */
/* through the weak adapter_apic_eoi_write() so the host test can       */
/* observe it.                                                          */
/* ------------------------------------------------------------------ */

void
adapter_apic_eoi(void)
{
    adapter_apic_eoi_write(0);
}
