/*
 * core_bringup.c - U2: Highest-core bring-up via EFI_MP_SERVICES_PROTOCOL.
 *
 * Determines the core count, picks the highest-numbered AP as the bridge
 * core, allocates its stack in the reserved region, and starts it via the
 * UEFI MP Services protocol (StartupThisAP).
 *
 * Because this application runs INSIDE the UEFI environment (before
 * ExitBootServices), we MUST NOT issue manual INIT-SIPI-SIPI sequences.
 * The firmware's EFI_MP_SERVICES_PROTOCOL abstracts the underlying
 * architecture completely: it wakes the AP in its native environment
 * (already in long mode, with UEFI's page tables, GDT, and stack set up)
 * and executes a plain C function of our choice on it. This eliminates the
 * entire class of boot-loop bugs that plague hand-rolled trampolines
 * (real-mode trampoline, identity-mapped page tables, flat GDT, APIC
 * INIT-SIPI-SIPI timing, APIC ID targeting).
 *
 * The AP procedure (bridge_ap_entry) switches to the reserved-region bridge
 * stack and calls bridge_entry() (B5). The bridge runs in place from the
 * UEFI image; the copy-to-reserved-region step is deferred to the production
 * handoff.
 *
 * See docs/architecture.md section 7.3, module U2.
 */

#include <efi.h>
#include <efilib.h>

#include "uefi.h"
#include "../bridge/bridge.h"

/* ------------------------------------------------------------------ */
/* Bridge core state.                                                  */
/* ------------------------------------------------------------------ */

/* Bridge core stack size (bytes), allocated in the reserved region. */
#define BRIDGE_STACK_SIZE      0x4000   /* 16 KiB */

/* Bridge stack top (set before the AP is started; read by the AP procedure
 * to switch to the reserved-region bridge stack). Global (not static) so the
 * AP procedure's reference resolves to a normal data-section relocation. */
UINT64 g_bridge_stack_top;

/* Boot-status flag: set to 1 by the AP procedure as its very first action,
 * so the BSP can verify the AP actually started and reached the bridge
 * entry. Global (not static) so the AP procedure's reference resolves to a
 * normal data-section relocation. */
volatile UINT8 g_ap_booted = 0;

/* ------------------------------------------------------------------ */
/* AP procedure.                                                       */
/*                                                                     */
/* Runs on the bridge core in its native environment (long mode, UEFI   */
/* page tables/GDT/stack already set up by the firmware). Switches to   */
/* the reserved-region bridge stack, signals the BSP, and calls         */
/* bridge_entry() (B5).                                                 */
/* ------------------------------------------------------------------ */
static VOID EFIAPI
bridge_ap_entry(VOID *procedure_argument)
{
    /* Signal the BSP: the AP is running. */
    g_ap_booted = 1;

    /* Switch to the reserved-region bridge stack. The firmware-provided
     * stack may be reclaimed after ExitBootServices, but the bridge runs
     * persistently, so it needs its own stack in the reserved region. */
    if (g_bridge_stack_top != 0)
        __asm__ __volatile__("movq %0, %%rsp" : : "r"(g_bridge_stack_top)
                             : "memory");

    /* Run the bridge. This never returns (bridge_entry loops forever). */
    bridge_entry();

    /* Not reached. */
    (VOID)procedure_argument;
}

/* ------------------------------------------------------------------ */
/* uefi_bringup_highest_core(): determine core count, allocate the      */
/* bridge stack, and start the highest AP via EFI_MP_SERVICES_PROTOCOL. */
/* ------------------------------------------------------------------ */
EFI_STATUS
uefi_bringup_highest_core(void)
{
    EFI_MP_SERVICES_PROTOCOL *mp = NULL;
    EFI_GUID mp_guid = EFI_MP_SERVICES_PROTOCOL_GUID;
    EFI_STATUS status;
    UINTN num_processors = 0;
    UINTN num_enabled = 0;
#ifdef BRIDGE_DEBUG
    UINTN bsp_number = 0;
#endif
    UINTN highest_ap = 0;
    UINTN i;
    EFI_PHYSICAL_ADDRESS stack_addr = 0;
    BOOLEAN found = FALSE;

    /* 1. Locate the MP Services protocol. This is the firmware-sanctioned
     *    way to start APs while still inside the UEFI environment. */
    status = uefi_call_wrapper(
        BS->LocateProtocol, 3, &mp_guid, NULL, (VOID **)&mp);
    if (EFI_ERROR(status) || mp == NULL) {
#ifdef BRIDGE_DEBUG
        Print(L"BRIDGE-DBG: EFI_MP_SERVICES_PROTOCOL not found (status=%r)\n",
              status);
#endif
        return EFI_UNSUPPORTED;
    }

    /* 2. Get the number of processors. */
    status = uefi_call_wrapper(
        mp->GetNumberOfProcessors, 3, mp, &num_processors, &num_enabled);
    if (EFI_ERROR(status))
        return status;

    if (num_processors < 2) {
        /* Single-core system: no AP to bring up. */
#ifdef BRIDGE_DEBUG
        Print(L"BRIDGE-DBG: only %d processor(s); no AP to start\n",
              num_processors);
#endif
        return EFI_SUCCESS;
    }

    /* 3. Identify the BSP and pick the highest-numbered AP as the bridge
     *    core. APIC IDs are not guaranteed contiguous, so we use the
     *    processor NUMBER (index into the MP Services enumeration), not a
     *    raw APIC ID. */
    for (i = 0; i < num_processors; i++) {
        EFI_PROCESSOR_INFORMATION info;
        status = uefi_call_wrapper(
            mp->GetProcessorInfo, 3, mp, i, &info);
        if (EFI_ERROR(status))
            continue;

        if (info.StatusFlag & PROCESSOR_AS_BSP_BIT) {
#ifdef BRIDGE_DEBUG
            bsp_number = i;
#endif
            continue;
        }

        /* Track the highest-numbered AP. */
        if (!found || i > highest_ap) {
            highest_ap = i;
            found = TRUE;
        }
    }

    if (!found) {
#ifdef BRIDGE_DEBUG
        Print(L"BRIDGE-DBG: no AP found (num_processors=%d)\n",
              num_processors);
#endif
        return EFI_UNSUPPORTED;
    }

#ifdef BRIDGE_DEBUG
    Print(L"BRIDGE-DBG: MP Services: %d processors, BSP=%d, bridge AP=%d\n",
          num_processors, bsp_number, highest_ap);
#endif

    /* 4. Allocate a stack for the bridge core in the reserved region. */
    status = uefi_call_wrapper(
        BS->AllocatePages, 4, AllocateAnyPages, EfiReservedMemoryType,
        BRIDGE_STACK_SIZE / 4096, &stack_addr);
    if (EFI_ERROR(status))
        return status;
    g_bridge_stack_top = stack_addr + BRIDGE_STACK_SIZE;

    /* 5. Start the highest AP. StartupThisAP wakes the AP in its native
     *    environment (long mode) and runs bridge_ap_entry on it. The call
     *    blocks until the AP finishes (or the timeout elapses); bridge_ap_entry
     *    never returns, so this call effectively never returns either. We use
     *    a finite timeout so the BSP can detect a failed bring-up. */
    g_ap_booted = 0;
    status = uefi_call_wrapper(
        mp->StartupThisAP, 7, mp, bridge_ap_entry, highest_ap,
        NULL,            /* WaitEvent: NULL = blocking */
        1000000,         /* TimeoutInMicroseconds: 1 s */
        NULL,            /* ProcedureArgument */
        NULL);           /* Finished */

#ifdef BRIDGE_DEBUG
    /* Report whether the AP actually reached the bridge entry. g_ap_booted
     * is set by bridge_ap_entry as its very first action, so a value of 1
     * proves the AP started and reached the bridge. A value of 0 means the
     * AP failed to start (or the timeout elapsed). */
    Print(L"BRIDGE-DBG: AP boot status: g_ap_booted=%d (bridge AP=%d, "
          L"StartupThisAP status=%r)\n",
          g_ap_booted, highest_ap, status);
#endif

    if (EFI_ERROR(status) || !g_ap_booted)
        return EFI_DEVICE_ERROR;

    /* 6. Load the input adapter into the reserved region on core 0 (for
     *    O1/O2). The adapter code is also linked into the image; in a full
     *    implementation it is copied to a reserved region on core 0. This
     *    is deferred to the OS-integration phase. */

    return EFI_SUCCESS;
}
