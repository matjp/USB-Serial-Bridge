/*
 * xhci_fault.h - XHCI bring-up fault record (the debugging model).
 *
 * The XHCI controller is handed off from UEFI to the bridge in an unknown,
 * partially-configured state. Re-configuring it (reset, rings, device
 * contexts, doorbells) is the most failure-prone part of the design, so a
 * bare "fatal" flag is not debuggable - we need to know WHICH step failed,
 * WHAT the controller reported, and WHAT the bridge was doing.
 *
 * This record lives in the reserved region (published like the mailbox) so
 * the Layer 1 harness on core 0 can read it after the bridge halts and print
 * a human-readable diagnosis. See docs/architecture.md section 9, Layer 1.
 */

#ifndef XHCI_FAULT_H
#define XHCI_FAULT_H

#include <efi.h>

/* Fixed physical address where the bridge publishes the XHCI_FAULT record.
 * Chosen to sit in the reserved region, clear of the mailbox pointer slots
 * (0x10000000 / 0x10000010) and the USB_TOPOLOGY pointer (0x10000008). */
#define XHCI_FAULT_PTR_ADDR  0x10000018ULL

/* Bring-up stages. Each handoff step records its own stage so the fault
 * record says exactly where bring-up stopped. */
enum {
    XHCI_STAGE_NONE      = 0,
    XHCI_STAGE_VERIFY    = 1,  /* XHCI >= 1.0 check (C6) */
    XHCI_STAGE_RESET     = 2,  /* HCRST + wait for HCHalted */
    XHCI_STAGE_RINGS     = 3,  /* command ring + event ring setup */
    XHCI_STAGE_DEVICES   = 4,  /* DCBAA + device contexts */
    XHCI_STAGE_TRANSFER  = 5,  /* transfer ring init */
    XHCI_STAGE_RUN       = 6,  /* USBCMD.RUN */
    XHCI_STAGE_DOORBELL  = 7,  /* endpoint doorbells */
    XHCI_STAGE_POLL      = 8   /* event ring polling */
};

/* Completion-code / error hints for the harness readout. */
enum {
    XHCI_FAULT_HINT_NONE        = 0,
    XHCI_FAULT_HINT_VERIFY      = 1,  /* controller is not XHCI >= 1.0 */
    XHCI_FAULT_HINT_RESET_TIMEOUT = 2, /* HCRST or HCHalted never set */
    XHCI_FAULT_HINT_RING_SETUP  = 3,  /* command/event ring programming failed */
    XHCI_FAULT_HINT_DEV_CTX     = 4,  /* device context / DCBAA setup failed */
    XHCI_FAULT_HINT_TRANSFER    = 5,  /* transfer ring init failed */
    XHCI_FAULT_HINT_RUN         = 6,  /* controller did not start (RUN) */
    XHCI_FAULT_HINT_DOORBELL    = 7,  /* doorbell write failed / no response */
    XHCI_FAULT_HINT_POLL        = 8   /* event ring desync / bad completion */
};

typedef struct {
    UINT32 magic;           /* XHCI_FAULT_MAGIC for sanity check */
    UINT32 stage;           /* XHCI_STAGE_* where bring-up stopped */
    UINT32 hint;            /* XHCI_FAULT_HINT_* for the readout */
    UINT32 usbsts;          /* USBSTS register at failure */
    UINT32 usbcmd;          /* USBCMD register at failure */
    UINT32 crcr;            /* CRCR register at failure */
    UINT32 last_cc;         /* last completion code seen (poll stage) */
    UINT32 last_trb_type;   /* last TRB type seen (poll stage) */
    UINT32 doorbell;        /* last doorbell value written */
    UINT32 reserved[7];
} XHCI_FAULT;

#define XHCI_FAULT_MAGIC 0x58484349u   /* "XHCI" */

/* Publish the XHCI_FAULT record base address at the fixed pointer location.
 * Called by the bridge on the bridge core at bring-up. */
static inline void
xhci_fault_publish(XHCI_FAULT *fault)
{
    volatile UINT64 *slot = (volatile UINT64 *)XHCI_FAULT_PTR_ADDR;
    *slot = (UINT64)(UINTN)fault;
}

/* Read the published XHCI_FAULT record base address (NULL if not published).
 * Called by the Layer 1 harness on core 0 after the bridge halts. */
static inline XHCI_FAULT *
xhci_fault_lookup(void)
{
    volatile UINT64 *slot = (volatile UINT64 *)XHCI_FAULT_PTR_ADDR;
    return (XHCI_FAULT *)(UINTN)*slot;
}

#endif /* XHCI_FAULT_H */
