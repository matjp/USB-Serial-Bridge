/*
 * xhci_status.h - XHCI bring-up success record (debug builds only).
 *
 * The fault record (xhci_fault.h) captures state when bring-up FAILS. For
 * downstream debugging and verification it is also useful to capture the
 * ACTUAL XHCI hardware state when bring-up SUCCEEDS - the register snapshot
 * (proving the controller is running) plus the discovered topology. This
 * record is filled and published by B1 only in BRIDGE_DEBUG builds, and
 * printed by the Layer 1 harness on core 0.
 *
 * It lives in the reserved region (published like the topology/fault
 * records) so the harness can read it after the bridge runs. The pointer
 * slot is the next free 8-byte slot in the reserved pointer page, clear of
 * the topology (0x10000008) and fault (0x10000018) pointers.
 */

#ifndef XHCI_STATUS_H
#define XHCI_STATUS_H

#include <efi.h>

/* Fixed physical address where the bridge publishes the XHCI_STATUS record
 * base address (debug builds only). */
#define XHCI_STATUS_PTR_ADDR  0x10000020ULL

typedef struct {
    UINT32 magic;           /* XHCI_STATUS_MAGIC for sanity check */
    UINT32 usbsts;          /* USBSTS after bring-up (controller running) */
    UINT32 usbcmd;          /* USBCMD after bring-up (RUN set) */
    UINT32 crcr;            /* CRCR (command ring control) */
    UINT32 max_slots;       /* HCSPARAMS1: number of slots */
    UINT32 max_eps;         /* HCSPARAMS1: endpoints per slot */
    UINT32 max_scratchpad;  /* HCSPARAMS2: scratchpad buffers */
    UINT32 page_size;       /* PAGESIZE */
    UINT64 xhci_mmio_base;  /* BAR0 (from topology, 64-bit) */
    UINT32 xhci_cap_len;    /* CAPLENGTH (from topology) */
    UINT32 kbd;             /* packed kbd endpoint: addr|ep<<8|int<<16|spd<<24 */
    UINT32 mouse;           /* packed mouse endpoint */
    UINT32 kbd_max_packet;  /* kbd wMaxPacketSize */
    UINT32 mouse_max_packet;/* mouse wMaxPacketSize */
    UINT32 reserved[2];
} XHCI_STATUS;

#define XHCI_STATUS_MAGIC 0x4F4B5843u   /* "OKXC" */

/* Publish the XHCI_STATUS record base address at the fixed pointer location.
 * Called by the bridge on the bridge core after a successful bring-up
 * (debug builds only). */
static inline void
xhci_status_publish(XHCI_STATUS *st)
{
    volatile UINT64 *slot = (volatile UINT64 *)XHCI_STATUS_PTR_ADDR;
    *slot = (UINT64)(UINTN)st;
}

/* Read the published XHCI_STATUS record base address (NULL if not published).
 * Called by the Layer 1 harness on core 0 (debug builds only). */
static inline XHCI_STATUS *
xhci_status_lookup(void)
{
    volatile UINT64 *slot = (volatile UINT64 *)XHCI_STATUS_PTR_ADDR;
    return (XHCI_STATUS *)(UINTN)*slot;
}

#endif /* XHCI_STATUS_H */
