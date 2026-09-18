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
#include "uefi/exception_handler.h"
#include "uefi/log_file.h"

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

    /* Open bridge-debug.log on the boot volume (robust edition). This only
     * activates when the boot device is verified to be a removable USB drive
     * AND it is the same drive the app was booted from; otherwise it is a
     * no-op and the app continues with console-only output. */
    uefi_log_init(image);

    /* Install the CPU exception trap (debug aid) so a fault inside the app
     * prints a register dump + stack trace and halts, instead of dying in
     * the firmware's default handler. No-op if the platform lacks
     * EFI_CPU_ARCH_PROTOCOL. */
    uefi_install_exception_handler(image);

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

    /* Flush the debug log to disk before handing off. */
    uefi_log_flush();

    /* 5. Hand off to the bootloader / OS on the BSP (core 0).
     *
     * We do NOT return EFI_SUCCESS here. Returning would make the firmware's
     * boot manager continue to the next boot option; with no OS on the drive
     * (as in the OVMF CI test) OVMF would reboot and re-run this app in a
     * loop, flooding the log with repeated boots. Instead we halt the BSP so
     * the app runs exactly once. The bridge core (AP) keeps running its own
     * poll loop independently. When a real OS/bootloader is added later, this
     * halt is replaced by the actual handoff (e.g. ExitBootServices + jump).
     *
     * The stall is deliberately LONG (60 s per iteration) so the final
     * diagnostic lines stay on screen long enough to read before the display
     * powers off / the screen blanks. The messages scroll by too fast to
     * catch during normal execution, so this pause is what lets the user see
     * the log flush status and any [LOG] write/flush failure markers. */
    for (;;) {
        uefi_call_wrapper(BS->Stall, 1, 60000000);   /* 60 s per iteration */
    }
}
