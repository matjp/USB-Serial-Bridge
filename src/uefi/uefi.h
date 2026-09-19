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

/* U1: Retry the XHCI ring extraction (event ring + kbd/mouse transfer
 * rings) until the firmware's XhciDxe driver has programmed ERSTBA.
 * The first attempt inside uefi_discover_usb runs too early (ERSTBA is
 * still 0); this is called again after the AP bring-up, when XhciDxe has
 * had time to initialize. Populates the XHCI_OBSERVER at XHCI_OBSERVER_ADDR
 * that the bridge AP reads on every poll. Returns TRUE on success. */
BOOLEAN uefi_extract_observer(void);

/* U3: Allocate + reserve the bridge, virtual port, and topology regions. */
EFI_STATUS uefi_reserve_memory(void);

/* U2: Bring up the highest core via EFI_MP_SERVICES_PROTOCOL, loading the
 * bridge code. */
EFI_STATUS uefi_bringup_highest_core(void);

/* U2: Register the EFI_EVENT_GROUP_EXIT_BOOT_SERVICES notification that
 * ensures the bridge AP is fully detached (independent page tables in CR3,
 * Local APIC masked, interrupts off) before the firmware tears down. */
EFI_STATUS uefi_register_exit_boot_services_hook(void);

/* U2: Mark the bridge AP as DISABLED in the ACPI MADT so the OS believes the
 * core is missing/dead and never tries to bring it up (rule 4). */
EFI_STATUS uefi_disable_bridge_ap_in_madt(void);

/* Layer 1 harness: after bridge bring-up, check the XHCI fault record. If the
 * bridge faulted, print a one-screen diagnosis to ConOut and halt (never boot
 * the OS). Returns normally if the bridge is healthy. */
void uefi_check_bridge_fault(void);

#endif /* UEFI_H */
