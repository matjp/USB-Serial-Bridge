/*
 * uefi.h - Internal interface for the UEFI setup application (Phase 1).
 *
 * These functions implement the boot-time setup steps U1-U3 and the entry
 * orchestration in main.c. See docs/architecture.md section 7.3.
 */

#ifndef UEFI_H
#define UEFI_H

#include <efi.h>

/* U1: Verify the host controller is XHCI >= 1.0 (C6). */
EFI_STATUS uefi_verify_xhci(void);

/* U1: Enumerate the single USB keyboard and mouse, record endpoints. */
EFI_STATUS uefi_discover_usb(void);

/* U3: Allocate + reserve the bridge, virtual port, and topology regions. */
EFI_STATUS uefi_reserve_memory(void);

/* U2: Bring up the highest core via SIPI, loading the bridge code. */
EFI_STATUS uefi_bringup_highest_core(void);

/* Layer 1 harness: after bridge bring-up, check the XHCI fault record. If the
 * bridge faulted, print a one-screen diagnosis to ConOut and halt (never boot
 * the OS). Returns normally if the bridge is healthy. */
void uefi_check_bridge_fault(void);

#endif /* UEFI_H */
