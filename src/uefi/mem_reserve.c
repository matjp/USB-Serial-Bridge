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
#include "../bridge/usb_topology.h"

/* The discovered topology, filled by U1 (usb_discovery.c). U3 copies it
 * into the reserved region and publishes its address for B1. */
extern USB_TOPOLOGY g_usb_topology;

/* Size of the bridge code region to reserve (in pages). The bridge code
 * (B1-B5) is linked into the same UEFI image; U2 copies it here. We reserve
 * a generous region to hold the bridge code + its data + stacks. */
#define BRIDGE_REGION_PAGES  64   /* 256 KiB */

/* Size of the USB_TOPOLOGY region (in pages). One page is plenty. */
#define TOPOLOGY_REGION_PAGES 1

/* Size of the bridge core stack (in bytes), allocated in the reserved
 * region by U2. */
#define BRIDGE_STACK_SIZE  0x4000   /* 16 KiB */

/* ------------------------------------------------------------------ */
/* Highest-address allocation helper.                                  */
/*                                                                     */
/* EfiReservedMemoryType alone is NOT sufficient for an E820-collecting  */
/* OS (e.g. TempleOS boots via its own BIOS bootloader and collects the */
/* E820 map, not the UEFI map). The reserved region must ALSO be placed  */
/* ABOVE the OS's physical memory space so the OS never allocates over   */
/* it. We implement the high-placement strategy: walk the EFI memory     */
/* map, find the highest conventional-memory region, and allocate the    */
/* reserved pages at the very top of it (via AllocateMaxAddress). This    */
/* places the bridge/mailbox/topology above the bulk of the OS's         */
/* physical memory.                                                      */
/* ------------------------------------------------------------------ */
static EFI_STATUS
allocate_reserved_pages_high(UINTN pages, EFI_PHYSICAL_ADDRESS *out)
{
    EFI_STATUS status;
    UINTN map_size = 0;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    EFI_MEMORY_DESCRIPTOR *desc;
    UINTN count, i;
    EFI_PHYSICAL_ADDRESS highest_end = 0;

    /* First call returns the required buffer size. */
    status = uefi_call_wrapper(
        BS->GetMemoryMap, 5, &map_size, NULL, &map_key, &desc_size, &desc_version);
    if (status != EFI_BUFFER_TOO_SMALL)
        return status;

    /* Allocate a scratch buffer for the map. */
    map_size += desc_size * 4;   /* room for a few new descriptors */
    status = uefi_call_wrapper(BS->AllocatePool, 2, EfiBootServicesData, map_size,
                               (VOID **)&map);
    if (EFI_ERROR(status))
        return status;

    status = uefi_call_wrapper(
        BS->GetMemoryMap, 5, &map_size, map, &map_key, &desc_size, &desc_version);
    if (EFI_ERROR(status)) {
        FreePool(map);
        return status;
    }

    /* Find the highest end of any conventional-memory region. */
    count = map_size / desc_size;
    for (i = 0; i < count; i++) {
        desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * desc_size);
        if (desc->Type == EfiConventionalMemory) {
            EFI_PHYSICAL_ADDRESS end = desc->PhysicalStart +
                                       (desc->NumberOfPages << 12);
            if (end > highest_end)
                highest_end = end;
        }
    }

    FreePool(map);

    if (highest_end == 0)
        return EFI_OUT_OF_RESOURCES;

    /* Allocate the reserved pages at the highest address below the top of
     * conventional memory. This places them above the OS's physical memory
     * space. */
    *out = highest_end;
    status = uefi_call_wrapper(
        BS->AllocatePages, 4, AllocateMaxAddress, EfiReservedMemoryType,
        pages, out);
    return status;
}

EFI_STATUS
uefi_reserve_memory(MAILBOX **out_mailbox)
{
    EFI_STATUS status;
    EFI_PHYSICAL_ADDRESS mailbox_addr = 0;
    EFI_PHYSICAL_ADDRESS bridge_addr = 0;
    EFI_PHYSICAL_ADDRESS topo_addr = 0;
    MAILBOX *mb;
    USB_TOPOLOGY *topo;

    *out_mailbox = NULL;

    /* 1. Allocate a page for the MAILBOX (EFI_RESERVED_MEMORY_TYPE), placed
     *    at the highest address so an E820-collecting OS never allocates
     *    over it. */
    status = allocate_reserved_pages_high(1, &mailbox_addr);
    if (EFI_ERROR(status))
        return status;
    mb = (MAILBOX *)(UINTN)mailbox_addr;

    /* Initialize and publish the mailbox. (main.c also calls these after
     * this function; both are idempotent.) */
    mailbox_init(mb);
    mailbox_publish(mb);

    /* 2. Allocate + reserve the bridge code region (also high-placed). */
    status = allocate_reserved_pages_high(BRIDGE_REGION_PAGES, &bridge_addr);
    if (EFI_ERROR(status))
        return status;

    /* 3. Allocate + reserve the USB_TOPOLOGY region (also high-placed). */
    status = allocate_reserved_pages_high(TOPOLOGY_REGION_PAGES, &topo_addr);
    if (EFI_ERROR(status))
        return status;

    /* Copy the discovered topology (filled by U1) into the reserved region
     * and publish its address so B1 can find it on the bridge core. */
    topo = (USB_TOPOLOGY *)(UINTN)topo_addr;
    *topo = g_usb_topology;
    usb_topology_publish(topo);

    /* 4. Return the mailbox pointer. */
    *out_mailbox = mb;

    return EFI_SUCCESS;
}
