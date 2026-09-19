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
        goto done;
    }

    /* 2. Enumerate the single USB keyboard and mouse (U1). */
    Print(L"BRIDGE-DBG: calling uefi_discover_usb\n");
    status = uefi_discover_usb();
    Print(L"BRIDGE-DBG: uefi_discover_usb returned %r\n", status);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: USB keyboard/mouse discovery failed (status %r)\n", status);
        goto done;
    }

    /* 3. Allocate + reserve the bridge and virtual port regions (U3). */
    Print(L"BRIDGE-DBG: calling uefi_reserve_memory\n");
    status = uefi_reserve_memory();
    Print(L"BRIDGE-DBG: uefi_reserve_memory returned %r\n", status);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: memory reservation failed (status %r)\n", status);
        goto done;
    }

    /* 4. Bring up the highest core via SIPI, loading the bridge code (U2). */
    Print(L"BRIDGE-DBG: calling uefi_bringup_highest_core\n");
    status = uefi_bringup_highest_core();
    Print(L"BRIDGE-DBG: uefi_bringup_highest_core returned %r\n", status);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: highest-core bring-up failed (status %r)\n", status);
        goto done;
    }

    /* 4a. Retry the XHCI ring extraction. The first attempt (inside
     *     uefi_discover_usb) runs too early: the firmware's XhciDxe driver
     *     has not yet programmed ERSTBA, so the event ring reads as 0 and
     *     the observer extraction fails. By now XhciDxe has had time to
     *     initialize the controller, so retry with a bounded delay until
     *     the event ring + kbd/mouse transfer rings are visible. The bridge
     *     AP reads the observer from the shared reserved page on every poll,
     *     so it picks up the populated data automatically. */
    Print(L"BRIDGE-DBG: calling uefi_extract_observer (retry)\n");
    uefi_extract_observer();
    Print(L"BRIDGE-DBG: uefi_extract_observer returned\n");

    /* 4a. Register the ExitBootServices notification. When the firmware
     *     tears down boot services, the notification runs on the BSP and
     *     waits for the bridge AP to finish detaching (independent page
     *     tables in CR3, Local APIC masked, interrupts off) so the AP
     *     survives the handoff (rule 1). */
    Print(L"BRIDGE-DBG: calling uefi_register_exit_boot_services_hook\n");
    status = uefi_register_exit_boot_services_hook();
    Print(L"BRIDGE-DBG: uefi_register_exit_boot_services_hook returned %r\n",
          status);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: ExitBootServices hook registration failed (status %r)\n",
              status);
        goto done;
    }

    /* 4b. Mark the bridge AP as DISABLED in the ACPI MADT so the OS believes
     *     the core is missing/dead and never tries to bring it up (rule 4).
     *     Best-effort: if ACPI is unavailable, the OS may still see the core,
     *     but the bridge AP is already detached and self-sustaining. */
    Print(L"BRIDGE-DBG: calling uefi_disable_bridge_ap_in_madt\n");
    status = uefi_disable_bridge_ap_in_madt();
    Print(L"BRIDGE-DBG: uefi_disable_bridge_ap_in_madt returned %r\n", status);

    /* Layer 1 harness: if the bridge faulted during XHCI bring-up, print a
     * one-screen diagnosis to the console and halt - never boot the OS. */
    Print(L"BRIDGE-DBG: calling uefi_check_bridge_fault\n");
    uefi_check_bridge_fault();
    Print(L"BRIDGE-DBG: uefi_check_bridge_fault returned\n");

    Print(L"Bridge setup complete. Handing off to OS on core 0.\n");

done:
    /* Flush the debug log to disk. This runs on BOTH success and every
     * error path, so the diagnostics are always captured. */
    uefi_log_flush();

    /* Close the log file and flush the volume. This forces the FAT driver
     * to commit all buffered data to the physical disk - EFI_FILE->Flush
     * alone may only flush to the volume's cache on some drivers, which is
     * why the file stayed empty on the Toshiba despite writes reporting
     * Success. */
    uefi_log_close();

    /* 5. Hand off to the bootloader / OS on the BSP (core 0).
     *
     * We do NOT return EFI_SUCCESS here. Returning would make the firmware's
     * boot manager continue to the next boot option; with no OS on the drive
     * (as in the OVMF CI test) OVMF would reboot and re-run this app in a
     * loop, flooding the log with repeated boots. Instead we halt the BSP so
     * the app runs exactly once. The bridge core (AP) keeps running its own
     * poll loop independently. When a real OS/bootloader is added later, this
     * halt is replaced by the actual handoff (e.g. ExitBootServices + jump). */
    for (;;) {
        uefi_call_wrapper(BS->Stall, 1, 10000000);   /* 10 s per iteration */
    }
}
