/*
 * xhci_observer.h - Read-only xHCI ring observer (pre-ExitBootServices).
 *
 * Before ExitBootServices, the UEFI firmware is single-threaded and runs
 * entirely on the BSP. The XhciDxe driver on the BSP owns the xHCI state
 * machine. If the bridge AP writes to the xHCI MMIO operational registers
 * or the PCI configuration space while the BSP is executing firmware
 * routines, it triggers severe race conditions: the controller falls out of
 * sync with UEFI's internal memory structures, and the firmware on the BSP
 * halts, throws a #GP, or fails to process ExitBootServices.
 *
 * The safe pre-EBS design is therefore a completely PASSIVE data observer:
 *
 *   1. On the BSP, parse the UEFI USB structures to extract the physical
 *      memory addresses of the xHCI Event Ring and the Transfer Rings for
 *      the keyboard and mouse slots. These are stored in the reserved page
 *      (XHCI_OBSERVER_ADDR) and passed to the AP via the StartupThisAP
 *      procedure argument.
 *
 *   2. On the AP, run a tight polling loop that continuously inspects the
 *      Cycle Bit of the Event Ring memory blocks. Memory reads are
 *      thread-safe and do not affect the xHCI controller's state, so the AP
 *      can silently read new packets as they arrive.
 *
 *   3. The AP NEVER advances the enqueue/dequeue pointers and NEVER writes
 *      back to the xHCI registers (no ERDP update, no doorbell, no rearm).
 *      UEFI on the BSP remains the sole owner of the controller. The AP
 *      simply duplicates the packet data into the VIRTUAL_PS2_BASE virtual
 *      buffer.
 *
 * After ExitBootServices, the UEFI firmware vanishes and the AP becomes the
 * sole owner of the xHCI memory rings; only then may it begin updating the
 * xHCI register pointers (see xhci.c's full bring-up path).
 */

#ifndef XHCI_OBSERVER_H
#define XHCI_OBSERVER_H

#include <efi.h>

/* Fixed physical address where the XHCI_OBSERVER is stored in the reserved
 * page (0x10000000..0x10000FFF). Chosen clear of the mailbox/topology/
 * fault/status pointer slots (0x00..0x28) and VIRTUAL_PS2_BASE (0x30). The
 * reserved page is identity-mapped in the bridge AP's page tables, so the
 * AP can read the observer after it detaches from UEFI. */
#define XHCI_OBSERVER_ADDR  0x10000040ULL

/* The UEFI xHCI rings the bridge AP observes read-only before
 * ExitBootServices. Extracted on the BSP from the xHCI MMIO registers
 * (ERSTBA -> event ring; DCBAAP -> device contexts -> endpoint context
 * TR Dequeue Pointer -> transfer rings) and passed to the AP via the
 * StartupThisAP procedure argument. */
typedef struct {
    UINT64 event_ring_addr;   /* UEFI's xHCI event ring base (physical) */
    UINT32 event_ring_size;   /* number of TRBs in the event ring */
    UINT32 reserved0;
    UINT64 kbd_tr_addr;       /* UEFI's keyboard transfer ring base */
    UINT64 mouse_tr_addr;     /* UEFI's mouse transfer ring base */
    UINT32 kbd_slot;          /* keyboard device slot number */
    UINT32 mouse_slot;        /* mouse device slot number */
} XHCI_OBSERVER;

#endif /* XHCI_OBSERVER_H */
