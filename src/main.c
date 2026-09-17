/*
 * main.c - UEFI setup application entry point (Phase 1).
 *
 * Boot-time setup, before ExitBootServices:
 *   1. Verify the host controller is XHCI >= 1.0 (C6).
 *   2. Enumerate the single USB keyboard and mouse (U1).
 *   3. Allocate + reserve the bridge and virtual port regions (U3).
 *   4. Bring up the highest core via SIPI, loading the bridge code (U2).
 *   5. Hand off to the bootloader / OS on the BSP (core 0).
 *
 * See docs/architecture.md section 5.1.
 */

#include <efi.h>
#include <efilib.h>

#include "uefi/uefi.h"

EFI_STATUS
EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
    EFI_STATUS status;

    /* GNU-EFI requirement: InitializeLib MUST be the very first statement.
     * It sets up the global ST/BS/RT pointers. Using Print() or accessing
     * the SystemTable globals before this dereferences null/garbage and
     * crashes (#UD). */
    InitializeLib(image, systab);

    Print(L"BRIDGE-DBG: efi_main entered, InitializeLib done\n");
    Print(L"USB HID -> Virtual 8042 Port Bridge\n");

    /* 1. Verify XHCI >= 1.0 (C6). */
    Print(L"BRIDGE-DBG: calling uefi_verify_xhci\n");
    status = uefi_verify_xhci();
    Print(L"BRIDGE-DBG: uefi_verify_xhci returned %r\n", status);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: no XHCI >= 1.0 controller found (status %r)\n", status);
        return status;
    }

    /* 2. Enumerate the single USB keyboard and mouse (U1). */
    Print(L"BRIDGE-DBG: calling uefi_discover_usb\n");
    status = uefi_discover_usb();
    Print(L"BRIDGE-DBG: uefi_discover_usb returned %r\n", status);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: USB keyboard/mouse discovery failed (status %r)\n", status);
        return status;
    }

    /* 3. Allocate + reserve the bridge and virtual port regions (U3). */
    Print(L"BRIDGE-DBG: calling uefi_reserve_memory\n");
    status = uefi_reserve_memory();
    Print(L"BRIDGE-DBG: uefi_reserve_memory returned %r\n", status);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: memory reservation failed (status %r)\n", status);
        return status;
    }

    /* 4. Bring up the highest core via SIPI, loading the bridge code (U2). */
    Print(L"BRIDGE-DBG: calling uefi_bringup_highest_core\n");
    status = uefi_bringup_highest_core();
    Print(L"BRIDGE-DBG: uefi_bringup_highest_core returned %r\n", status);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: highest-core bring-up failed (status %r)\n", status);
        return status;
    }

    /* Layer 1 harness: if the bridge faulted during XHCI bring-up, print a
     * one-screen diagnosis to the console and halt - never boot the OS. */
    Print(L"BRIDGE-DBG: calling uefi_check_bridge_fault\n");
    uefi_check_bridge_fault();
    Print(L"BRIDGE-DBG: uefi_check_bridge_fault returned\n");

    Print(L"Bridge setup complete. Handing off to OS on core 0.\n");

    /* 5. Hand off to the bootloader / OS on the BSP (core 0). */
    return EFI_SUCCESS;
}
