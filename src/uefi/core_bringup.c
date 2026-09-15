/*
 * core_bringup.c - U2: Highest-core bring-up via SIPI.
 *
 * Determines the core count (CPUID leaf 0xB / ACPI MADT), picks the
 * highest-numbered core as the bridge core, sets up its GDT, stack, and page
 * tables, loads the bridge code, and starts it via SIPI.
 *
 * NOTE: This is a scaffold. The SIPI sequence, GDT/stack/page-table setup,
 * and bridge-code loading are filled in by the Firmware Coder
 * (see docs/architecture.md section 7.3, module U2).
 */

#include <efi.h>
#include <efilib.h>

#include "uefi.h"

EFI_STATUS
uefi_bringup_highest_core(void)
{
    /* TODO(Firmware Coder):
     *   1. Determine core count (CPUID leaf 0xB / ACPI MADT).
     *   2. Pick the highest-numbered core as the bridge core.
     *   3. Set up its GDT, stack, and page tables.
     *   4. Load the bridge code (B1-B5) into the reserved region.
     *   5. Start the core via SIPI (init IPI 0xC4500, startup IPI
     *      0xC4600+MPN_VECT, per TempleOS MultiProc.HC conventions). */
    return EFI_SUCCESS;
}
