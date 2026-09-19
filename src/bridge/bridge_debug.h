/*
 * bridge_debug.h - Persistent virtual debug serial for the bridge core.
 *
 * The BSP harness and the UEFI console are only available during the boot
 * phase; once the bridge is handed off (or the BSP harness is gone), the
 * bridge has no output channel. To debug the bridge after handoff, the
 * bridge implements its own VIRTUAL DEBUG SERIAL port in shared memory - a
 * fixed ring buffer in the reserved region that the bridge writes diagnostic
 * text to (its "serial out") and that the OS (or the BSP harness, before
 * handoff) reads (its "serial in"). Same producer/consumer pattern as the
 * virtual 8042 port, but for debug output instead of input.
 *
 * Why this design (see docs/architecture.md Layer 1):
 *   - Persistent: lives in the reserved region, survives handoff.
 *   - OS-independent: the bridge just writes bytes; any OS can read them.
 *   - No console-driver dependency: plain memory stores to a fixed address,
 *     avoiding the page-table / firmware-driver concerns of calling
 *     ConOut->OutputString from the AP (whose page tables only identity-map
 *     the low 4 GB, and whose console driver code may live above 4 GB).
 *   - Reuses the established fixed-address shared-memory mechanism.
 *
 * GATING: compiled in ONLY under BRIDGE_DEBUG (the debug build). The normal
 * bridge.efi build is unchanged - no debug-serial writes, no reserved-region
 * debug buffer. Keeps the release image silent and minimal.
 *
 * Layout (fixed physical addresses in the reserved region, clear of the
 * existing slots 0x10000000 mailbox, 0x10000008 topology, 0x10000018 fault,
 * 0x10000020 status, 0x10000030 virtual 8042):
 *
 *   BRIDGE_DEBUG_BUF_ADDR  0x10000040   ring buffer of NUL-terminated lines
 *
 * The buffer is a ring of fixed-size line slots. The bridge writes complete
 * diagnostic lines via bridge_debug_puts(); a reader (BSP harness now, OS
 * later) drains the buffer. A monotonically increasing write sequence number
 * lets a reader detect wrap and ordering.
 */

#ifndef BRIDGE_DEBUG_H
#define BRIDGE_DEBUG_H

#include <efi.h>

#ifdef BRIDGE_DEBUG

/* Fixed physical address of the virtual debug serial ring buffer. Placed in
 * the reserved region, clear of the mailbox/topology/fault/status/virtual-8042
 * slots (0x10000000..0x10000030). */
#define BRIDGE_DEBUG_BUF_ADDR  0x10000040ULL

/* Ring buffer geometry. */
#define BRIDGE_DEBUG_LINE_LEN   128   /* bytes per line slot (incl NUL) */
#define BRIDGE_DEBUG_LINE_COUNT  64   /* number of line slots */
#define BRIDGE_DEBUG_BUF_SIZE   (BRIDGE_DEBUG_LINE_LEN * BRIDGE_DEBUG_LINE_COUNT)

/* Header at the start of the buffer. */
typedef struct {
    UINT32 magic;        /* BRIDGE_DEBUG_MAGIC for sanity check */
    UINT32 write_seq;    /* monotonically increasing write sequence */
    UINT32 next_slot;    /* index of the next slot to write (0..COUNT-1) */
    UINT32 reserved;
} BRIDGE_DEBUG_HDR;

#define BRIDGE_DEBUG_MAGIC  0x44534242u   /* "BBSD" */

/* Initialize the virtual debug serial ring buffer (set magic, zero slots).
 * Called once by the UEFI app after the reserved page is zeroed, before the
 * bridge is started. Debug builds only. */
void bridge_debug_init(void);

/* Write a NUL-terminated diagnostic line to the virtual debug serial.
 * No-op if the buffer is not present. Debug builds only. */
void bridge_debug_puts(const CHAR8 *line);

/* Format a line into a small stack buffer and write it. Debug builds only.
 * Supports a minimal subset of format specifiers: %s (CHAR8*), %d/%u/%x
 * (UINT32), %p (pointer). */
void bridge_debug_printf(const CHAR8 *fmt, ...);

#endif /* BRIDGE_DEBUG */

#endif /* BRIDGE_DEBUG_H */
