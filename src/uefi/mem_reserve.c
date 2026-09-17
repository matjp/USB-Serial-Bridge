/*
 * mem_reserve.c - U3: Bridge + virtual port memory reservation.
 *
 * Allocates and reserves the bridge code region and the virtual 8042 port
 * region so the OS does not reuse them. Because an OS that boots via its own
 * BIOS bootloader collects E820 (not the UEFI memory map), an
 * EFI_RESERVED_MEMORY_TYPE entry alone is NOT sufficient - the region must
 * also be carved out of E820 or placed above the OS's physical memory space.
 *
 * NOTE: This is a scaffold. The allocation + reservation logic is filled in
 * by the Firmware Coder (see docs/architecture.md section 7.3, module U3).
 */

#include <efi.h>
#include <efilib.h>

#include <virtual_ps2.h>
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
/* EfiReservedMemoryType alone is NOT sufficient for an E820-collecting
 * OS (one that boots via its own BIOS bootloader and collects the E820
 * map, not the UEFI map). The reserved region must ALSO be placed ABOVE
 * the OS's physical memory space so the OS never allocates over it. We
 * implement the high-placement strategy: walk the EFI memory map, find
 * the highest conventional-memory region, and allocate the reserved pages
 * at the very top of it (via AllocateMaxAddress). This places the
 * bridge/topology above the bulk of the OS's physical memory. */
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
uefi_reserve_memory(void)
{
    EFI_STATUS status;
    EFI_PHYSICAL_ADDRESS bridge_addr = 0;
    EFI_PHYSICAL_ADDRESS topo_addr = 0;
    EFI_PHYSICAL_ADDRESS vp_addr = VIRTUAL_PS2_BASE;
    USB_TOPOLOGY *topo;

    /* 1. Reserve the fixed virtual 8042 port region page. The virtual port
     *    lives at the fixed address VIRTUAL_PS2_BASE (0x10000030) so the OS's
     *    PS/2 driver can reference it directly. Mark the containing page
     *    EFI_RESERVED_MEMORY_TYPE so the OS never allocates over it.
     *
     *    NOTE: BS->AllocatePages with AllocateAddress requires the address to
     *    be page-aligned (4 KiB). VIRTUAL_PS2_BASE (0x10000030) is NOT
     *    page-aligned, so we reserve the whole 4 KiB page that contains it
     *    (0x10000000). The virtual port registers at +0/+1 remain at their
     *    fixed addresses inside that reserved page. */
    vp_addr = VIRTUAL_PS2_BASE & ~(EFI_PHYSICAL_ADDRESS)0xFFF;
    status = uefi_call_wrapper(
        BS->AllocatePages, 4, AllocateAddress, EfiReservedMemoryType, 1, &vp_addr);
    if (EFI_ERROR(status))
        return status;

    /* 2. Allocate + reserve the bridge code region (high-placed). */
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

    return EFI_SUCCESS;
}
