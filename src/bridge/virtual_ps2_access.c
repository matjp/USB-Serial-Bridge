/*
 * virtual_ps2_access.c - Strong definitions of the virtual 8042 accessors.
 *
 * The virtual_ps2_* accessors are declared __attribute__((weak)) in
 * include/virtual_ps2.h so that host tests can override them with a mock
 * register file (the fixed VIRTUAL_PS2_BASE addresses are unmapped on the
 * host). However, the FIRMWARE must provide strong definitions of these
 * symbols: without them they are UNDEFINED WEAK in the image, the bridge's
 * calls route through the PLT, and GNU-EFI's _relocate() does not resolve
 * PLT/GOT entries (it only handles R_X86_64_RELATIVE), leaving the PLT
 * resolver NULL -> indirect jump to a bad address -> #UD.
 *
 * These strong definitions write to / read from the fixed virtual 8042
 * port region (VIRTUAL_PS2_BASE, see include/virtual_ps2.h). They are the
 * firmware's real implementation of the "virtual PS/2 hardware" interface.
 *
 * Producer side (bridge, highest core):
 *   - virtual_ps2_read_status()  - read the status register
 *   - virtual_ps2_write_data()   - write one data byte
 *   - virtual_ps2_set_status()   - set status bits (OR)
 *   - virtual_ps2_send_irq()     - deliver the virtual IRQ (IPI)
 *
 * Consumer side (OS adapter O1, core 0):
 *   - virtual_ps2_read_data()    - read the data byte
 *   - virtual_ps2_clear_status() - clear status bits (AND-NOT)
 */

#include <efi.h>
#include <virtual_ps2.h>

/* ------------------------------------------------------------------ */
/* Producer-side accessors (used by the bridge).                       */
/* ------------------------------------------------------------------ */

/* Read the virtual status register (mirrors 8042 I/O port 0x64). */
UINT8
virtual_ps2_read_status(void)
{
    volatile UINT8 *status = (volatile UINT8 *)(UINTN)VIRTUAL_PS2_STATUS;
    return *status;
}

/* Write one byte to the virtual data register (mirrors 8042 I/O port 0x60). */
void
virtual_ps2_write_data(UINT8 byte)
{
    volatile UINT8 *data = (volatile UINT8 *)(UINTN)VIRTUAL_PS2_DATA;
    *data = byte;
}

/* Set the given status bits (OR into the status register). */
void
virtual_ps2_set_status(UINT8 bits)
{
    volatile UINT8 *status = (volatile UINT8 *)(UINTN)VIRTUAL_PS2_STATUS;
    *status = (UINT8)(*status | bits);
}

/* Send a virtual interrupt (IPI) to the OS core on the given vector.
 *
 * The virtual IRQ is delivered as an inter-processor interrupt via the
 * local APIC ICR to the OS core (core 0), on the same vector the OS already
 * uses for that device (0x21 keyboard, 0x2C mouse), so the OS's existing
 * ISR fires and reads the virtual data port.
 *
 * NOTE: This is a scaffold-level implementation. The local APIC ICR write
 * (ICR_HIGH = OS core APIC ID << 24, ICR_LOW = 0x4000 | vector) is filled
 * in by the Firmware Coder (see docs/architecture.md section 7.2, module
 * O1 / the virtual IRQ mechanism). The OS core APIC ID is determined at
 * bring-up (U2). For now the function is a no-op placeholder so the symbol
 * is strongly defined and the bridge's call binds directly.
 */
void
virtual_ps2_send_irq(UINT8 vector)
{
    /* TODO(Firmware Coder): deliver the virtual IRQ via the local APIC ICR
     * to the OS core on `vector`. Placeholder no-op for now. */
    (void)vector;
}

/* ------------------------------------------------------------------ */
/* Consumer-side accessors (used by the OS adapter, O1).               */
/* ------------------------------------------------------------------ */

/* Read the data byte from the virtual data register (consumer side).
 * A pure load does NOT clear the status bit (unlike the real 8042), so the
 * reader must also call virtual_ps2_clear_status() (read-and-clear). */
UINT8
virtual_ps2_read_data(void)
{
    volatile UINT8 *data = (volatile UINT8 *)(UINTN)VIRTUAL_PS2_DATA;
    return *data;
}

/* Clear the given status bits (AND-NOT into the status register). */
void
virtual_ps2_clear_status(UINT8 bits)
{
    volatile UINT8 *status = (volatile UINT8 *)(UINTN)VIRTUAL_PS2_STATUS;
    *status = (UINT8)(*status & (UINT8)~bits);
}
