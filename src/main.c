/*
 * main.c - UEFI setup application entry point (Phase 1).
 *
 * Boot-time setup, before ExitBootServices:
 *   1. Verify the host controller is XHCI >= 1.0 (C6).
 *   2. Enumerate the single USB keyboard and mouse (U1).
 *   3. Allocate + reserve the bridge and mailbox regions (U3).
 *   4. Bring up the highest core via SIPI, loading the bridge code (U2).
 *   5. Hand off to the bootloader / OS on the BSP (core 0).
 *
 * See docs/architecture.md section 5.1.
 */

#include <efi.h>
#include <efilib.h>

#include <mailbox.h>
#include "uefi/uefi.h"

EFI_STATUS
EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
    EFI_STATUS status;
    MAILBOX *kbd_mailbox;
    MAILBOX *mouse_mailbox;

    InitializeLib(image, systab);

    Print(L"USB HID -> Polled Mailbox Bridge\n");

    /* 1. Verify XHCI >= 1.0 (C6). */
    status = uefi_verify_xhci();
    if (EFI_ERROR(status)) {
        Print(L"ERROR: no XHCI >= 1.0 controller found (status %r)\n", status);
        return status;
    }

    /* 2. Enumerate the single USB keyboard and mouse (U1). */
    status = uefi_discover_usb();
    if (EFI_ERROR(status)) {
        Print(L"ERROR: USB keyboard/mouse discovery failed (status %r)\n", status);
        return status;
    }

    /* 3. Allocate + reserve the bridge and mailbox regions (U3). */
    status = uefi_reserve_memory(&kbd_mailbox, &mouse_mailbox);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: memory reservation failed (status %r)\n", status);
        return status;
    }
    mailbox_init(kbd_mailbox);
    mailbox_init(mouse_mailbox);
    mailbox_publish_kbd(kbd_mailbox);
    mailbox_publish_mouse(mouse_mailbox);

    /* 4. Bring up the highest core via SIPI, loading the bridge code (U2). */
    status = uefi_bringup_highest_core();
    if (EFI_ERROR(status)) {
        Print(L"ERROR: highest-core bring-up failed (status %r)\n", status);
        return status;
    }

    /* Layer 1 harness: if the bridge faulted during XHCI bring-up, print a
     * one-screen diagnosis to the console and halt - never boot the OS. */
    uefi_check_bridge_fault();

    Print(L"Bridge setup complete. Handing off to OS on core 0.\n");

    /* 5. Hand off to the bootloader / OS on the BSP (core 0). */
    return EFI_SUCCESS;
}
