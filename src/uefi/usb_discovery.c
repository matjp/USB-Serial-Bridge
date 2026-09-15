/*
 * usb_discovery.c - U1: USB topology discovery + XHCI >= 1.0 verification.
 *
 * Phase 1, before ExitBootServices. Uses the standard UEFI USB stack
 * (EFI_USB2_HC_PROTOCOL / EFI_USB_IO_PROTOCOL) to find the single USB
 * keyboard and mouse and record their interrupt IN endpoints. Done once -
 * no runtime enumeration.
 *
 * NOTE: This is a scaffold. The endpoint-recording logic is filled in by the
 * Firmware Coder (see docs/architecture.md section 7.3, module U1).
 */

#include <efi.h>
#include <efilib.h>

#include "uefi.h"

/* Verify the host controller is XHCI >= 1.0 (C6).
 * Checks the XHCI spec version via the EFI_USB2_HC_PROTOCOL revision, or the
 * XHCI capability registers (HCSPARAMS1/HCCPARAMS) once the controller is
 * located. Aborts cleanly if not XHCI >= 1.0 - no EHCI/UHCI/OHCI fallback. */
EFI_STATUS
uefi_verify_xhci(void)
{
    /* TODO(Firmware Coder): locate the XHCI controller and verify its spec
     * version is >= 1.0. Return EFI_UNSUPPORTED if not. */
    return EFI_SUCCESS;
}

/* Enumerate the single USB keyboard and mouse, record their endpoints. */
EFI_STATUS
uefi_discover_usb(void)
{
    /* TODO(Firmware Coder): walk the USB tree via EFI_USB2_HC_PROTOCOL /
     * EFI_USB_IO_PROTOCOL, find the boot-protocol keyboard and mouse, and
     * record their interrupt IN endpoints for the bridge (B1). */
    return EFI_SUCCESS;
}
