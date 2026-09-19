/*
 * bridge_debug.h - Shared-memory capture of the bridge's ACTUAL serial
 * output (the PS/2 byte stream it produces), for pre-ExitBootServices
 * verification.
 *
 * The bridge AP (B4, virtual_ps2_writer.c) appends every byte it writes to
 * the virtual 8042 data register into this ring buffer. The BSP reads the
 * buffer back and logs it (main.c) BEFORE ExitBootServices, so the CI log
 * shows exactly what the bridge is producing - proving the read-only
 * observer -> parse -> translate -> write pipeline is actually delivering
 * HID-derived PS/2 bytes.
 *
 * This is a DEBUG/VERIFICATION aid only. It is not part of the OS-facing
 * ABI (the OS consumes VIRTUAL_PS2_DATA/STATUS, not this buffer). It lives
 * in the reserved region, clear of the mailbox/topology/fault/status/
 * observer slots and VIRTUAL_PS2_BASE.
 *
 * Layout (fixed physical addresses in the reserved page 0x10000000..):
 *   0x10000000 mailbox ptr
 *   0x10000008 USB_TOPOLOGY_PTR_ADDR
 *   0x10000018 XHCI_FAULT_PTR_ADDR
 *   0x10000020 XHCI_STATUS_PTR_ADDR
 *   0x10000030 VIRTUAL_PS2_BASE (status +0, data +1)
 *   0x10000040 XHCI_OBSERVER_ADDR (XHCI_OBSERVER, 0x40..0x68)
 *   0x10000080 BRIDGE_DEBUG_ADDR   <-- this buffer (clear of the observer)
 */

#ifndef BRIDGE_DEBUG_H
#define BRIDGE_DEBUG_H

#include <efi.h>

/* Fixed physical address of the bridge debug capture region. Placed clear
 * of the XHCI_OBSERVER (0x10000040..0x10000068) and all other reserved
 * slots. NOTE: this MUST NOT overlap the observer - the AP reads the
 * observer every poll, so writing the debug capture over it would corrupt
 * the transfer-ring addresses and break the observer's matching. */
#define BRIDGE_DEBUG_ADDR   0x10000080ULL

/* Capacity of the captured serial-output ring buffer (bytes). */
#define BRIDGE_DEBUG_CAP    256

/* Producer/consumer ring-buffer state + counters. All fields are volatile
 * because they are written by the AP and read by the BSP (cache-coherent
 * x86, MESI). The producer writes data[] then mfence() then advances head,
 * so the consumer never sees a torn byte. */
typedef struct {
    volatile UINT32 head;        /* next write index (AP producer)        */
    volatile UINT32 tail;        /* next read index (BSP consumer)        */
    volatile UINT32 wrap;        /* number of times the buffer wrapped    */
    volatile UINT32 kbd_bytes;   /* total keyboard bytes produced         */
    volatile UINT32 mouse_bytes; /* total mouse bytes produced            */
    volatile UINT8  data[BRIDGE_DEBUG_CAP];

    /* --- Observer diagnostics (written by B1 bridge_observer_poll) ---
     * These let the BSP see how far the passive observer got: how many
     * times it polled, how many valid event-ring TRBs it saw, how many
     * matched the kbd/mouse transfer rings, and its current dequeue
     * index + cycle bit. If kbd_bytes==0, these tell us whether the
     * observer is running at all, whether it is stuck on a cycle-bit
     * mismatch, or whether it is seeing events that never match. */
    volatile UINT32 obs_polls;       /* times bridge_observer_poll ran    */
    volatile UINT32 obs_events;      /* valid (cycle-matching) TRBs seen  */
    volatile UINT32 obs_kbd;         /* transfer events matched to kbd    */
    volatile UINT32 obs_mouse;       /* transfer events matched to mouse  */
    volatile UINT32 obs_deq;         /* observer's current dequeue index  */
    volatile UINT32 obs_cycle;       /* observer's current cycle bit      */
    volatile UINT32 obs_last_type;   /* last event TRB type (field3>>6)   */
    volatile UINT32 obs_last_cc;     /* last event completion code        */
    volatile UINT32 obs_last_trb;    /* last event TRB address (low 32)   */
} BRIDGE_DEBUG_REC;

/* Producer-side: append one serial byte to the capture buffer. Called by
 * B4 (bridge_write_virtual_ps2) for every byte it writes to the virtual
 * 8042 data register.
 *
 * Declared WEAK so the host tests can override it with a mock (the fixed
 * address 0x10000080 is unmapped on the host). The default implementation
 * (in virtual_ps2_writer.c) writes to the shared-memory ring buffer. */
__attribute__((weak)) void bridge_debug_capture(UINT8 byte, BOOLEAN is_kbd);

#endif /* BRIDGE_DEBUG_H */
