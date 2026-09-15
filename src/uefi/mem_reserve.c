/*
 * mem_reserve.c - U3: Bridge + mailbox memory reservation.
 *
 * Allocates and reserves the bridge code region and the mailbox region so
 * the OS does not reuse them. Because TempleOS boots via its own BIOS
 * bootloader collecting E820 (not the UEFI memory map), an
 * EFI_RESERVED_MEMORY_TYPE entry alone is NOT sufficient - the region must
 * also be carved out of E820 or placed above mem_physical_space.
 *
 * NOTE: This is a scaffold. The allocation + reservation logic is filled in
 * by the Firmware Coder (see docs/architecture.md section 7.3, module U3).
 */

#include <efi.h>
#include <efilib.h>

#include <mailbox.h>
#include "uefi.h"

EFI_STATUS
uefi_reserve_memory(MAILBOX **out_mailbox)
{
    /* TODO(Firmware Coder):
     *   1. Allocate a page for the MAILBOX (EFI_RESERVED_MEMORY_TYPE).
     *   2. Allocate + reserve the bridge code region.
     *   3. Ensure the regions are also excluded from E820 / placed above
     *      mem_physical_space so TempleOS (and any E820-collecting OS) does
     *      not reuse them.
     *   4. Return the mailbox pointer via *out_mailbox. */
    *out_mailbox = NULL;
    return EFI_SUCCESS;
}
