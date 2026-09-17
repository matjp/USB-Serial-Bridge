/*
 * uefi.h - Internal interface for the UEFI setup application (Phase 1).
 *
 * These functions implement the boot-time setup steps U1-U3 and the entry
 * orchestration in main.c. See docs/architecture.md section 7.3.
 */

#ifndef UEFI_H
#define UEFI_H

#include <efi.h>

/*
 * Screen output gating.
 *
 * The release build is SILENT: it performs the bridge setup and hands off to
 * the OS with no console output at all. All Print() calls in the UEFI setup
 * path (main.c, l1_harness.c, usb_discovery.c) are compiled to no-ops unless
 * BRIDGE_DEBUG is defined (the debug build).
 *
 * This header is included AFTER <efilib.h> in every file that prints, so the
 * Print() function declaration is already visible and this macro cleanly
 * overrides all subsequent call sites. The debug build is unchanged.
 */
#ifdef BRIDGE_DEBUG
/* Debug build: Print() works normally (declared in <efilib.h>). */
#else
/* Release build: Print() is a no-op - no screen output. */
#define Print(...) ((void)0)
#endif

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
