/*
 * virtual_ps2.h - Virtual 8042 port region (the OS-agnostic "virtual PS/2
 * hardware" interface). See docs/architecture.md section 6.
 *
 * This is the alternative to the mailbox ABI: instead of a ring buffer that
 * a per-OS adapter drains, the bridge presents a small shared-memory region
 * that MIRRORS the real 8042 controller's two I/O registers:
 *
 *   - VIRTUAL_PS2_STATUS  mirrors the 8042 status register (I/O port 0x64)
 *   - VIRTUAL_PS2_DATA    mirrors the 8042 data register   (I/O port 0x60)
 *
 * The OS's existing PS/2 driver reads these two registers. To use the
 * virtual port, the OS replaces its `in 0x60` / `in 0x64` instructions with
 * loads from these fixed addresses (the absolute-minimum OS change). The OS
 * reuses its existing Set 1 decoder and 3-byte mouse-packet parser unchanged.
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
 * The bridge (producer, highest core) writes bytes and sets status bits; the
 * OS (consumer, core 0) reads status and data. x86 is cache-coherent (MESI);
 * the producer uses mfence() before setting the status bit so the data byte
 * is visible before the "ready" flag.
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
/* Weak accessors.                                                     */
/*                                                                     */
/* The bridge writes to the fixed addresses above. On the host these are */
/* unmapped, so the host test overrides these weak functions with a mock  */
/* register file (same pattern as xhci_read32/xhci_write32 in xhci.c).    */
/* ------------------------------------------------------------------ */

/* Read the virtual status register. */
__attribute__((weak)) UINT8 virtual_ps2_read_status(void);

/* Write one byte to the virtual data register. */
__attribute__((weak)) void virtual_ps2_write_data(UINT8 byte);

/* Set the given status bits (OR into the status register). */
__attribute__((weak)) void virtual_ps2_set_status(UINT8 bits);

#endif /* VIRTUAL_PS2_H */
