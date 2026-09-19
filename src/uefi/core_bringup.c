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
#include "../bridge/usb_topology.h"

/* ------------------------------------------------------------------ */
/* EFI_MP_SERVICES_PROTOCOL (portability shim).                        */
/*                                                                     */
/* The MP Services protocol is defined in GNU-EFI's efimp.h, which is   */
/* included by efi.h only in GNU-EFI >= 4.0. Older GNU-EFI versions     */
/* (e.g. the gnu-efi package on Ubuntu's apt repos, used by the GitHub  */
/* Actions CI) do NOT include efimp.h, so EFI_MP_SERVICES_PROTOCOL,     */
/* EFI_PROCESSOR_INFORMATION, PROCESSOR_AS_BSP_BIT, and the GUID are    */
/* undefined there. To keep the firmware buildable across GNU-EFI       */
/* versions, we provide self-contained definitions guarded by the       */
/* efimp.h include guard (_EFI_MP_H): if efimp.h was already included,  */
/* these are skipped; otherwise they supply the types we need.          */
/* ------------------------------------------------------------------ */
#ifndef _EFI_MP_H

#define EFI_MP_SERVICES_PROTOCOL_GUID \
    { 0x3fdda605, 0xa76e, 0x4f46, {0xad, 0x29, 0x12, 0xf4, 0x53, 0x1b, 0x3d, 0x08} }

#define PROCESSOR_AS_BSP_BIT        (1 << 0)
#define PROCESSOR_ENABLED_BIT       (1 << 1)
#define PROCESSOR_HEALTH_STATUS_BIT (1 << 2)

/* Forward-declare the protocol struct before the member function typedefs
 * reference it (mirrors efimp.h's INTERFACE_DECL). */
struct _EFI_MP_SERVICES_PROTOCOL;

typedef
struct {
    UINT32  Package;
    UINT32  Core;
    UINT32  Thread;
} EFI_CPU_PHYSICAL_LOCATION;

typedef
struct {
    UINT32  Package;
    UINT32  Module;
    UINT32  Tile;
    UINT32  Die;
    UINT32  Core;
    UINT32  Thread;
} EFI_CPU_PHYSICAL_LOCATION2;

typedef
union {
    EFI_CPU_PHYSICAL_LOCATION2  Location2;
} EXTENDED_PROCESSOR_INFORMATION;

typedef
struct {
    UINT64                          ProcessorId;
    UINT32                          StatusFlag;
    EFI_CPU_PHYSICAL_LOCATION       Location;
    EXTENDED_PROCESSOR_INFORMATION ExtendedInformation;
} EFI_PROCESSOR_INFORMATION;

typedef
VOID
(EFIAPI *EFI_AP_PROCEDURE) (
    IN VOID *ProcedureArgument
);

typedef
EFI_STATUS
(EFIAPI *EFI_MP_SERVICES_GET_NUMBER_OF_PROCESSORS) (
    IN struct _EFI_MP_SERVICES_PROTOCOL *This,
    OUT UINTN                           *NumberOfProcessors,
    OUT UINTN                           *NumberOfEnabledProcessors
);

typedef
EFI_STATUS
(EFIAPI *EFI_MP_SERVICES_GET_PROCESSOR_INFO) (
    IN struct _EFI_MP_SERVICES_PROTOCOL *This,
    IN UINTN                            ProcessorNumber,
    OUT EFI_PROCESSOR_INFORMATION       *ProcessorInfoBuffer
);

typedef
EFI_STATUS
(EFIAPI *EFI_MP_SERVICES_STARTUP_ALL_APS) (
    IN struct _EFI_MP_SERVICES_PROTOCOL *This,
    IN EFI_AP_PROCEDURE                 Procedure,
    IN BOOLEAN                          SingleThread,
    IN EFI_EVENT                        WaitEvent               OPTIONAL,
    IN UINTN                            TimeoutInMicroseconds,
    IN VOID                             *ProcedureArgument      OPTIONAL,
    OUT UINTN                           **FailedCpuList         OPTIONAL
);

typedef
EFI_STATUS
(EFIAPI *EFI_MP_SERVICES_STARTUP_THIS_AP) (
    IN struct _EFI_MP_SERVICES_PROTOCOL *This,
    IN EFI_AP_PROCEDURE                 Procedure,
    IN UINTN                            ProcessorNumber,
    IN EFI_EVENT                        WaitEvent               OPTIONAL,
    IN UINTN                            TimeoutInMicroseconds,
    IN VOID                             *ProcedureArgument      OPTIONAL,
    OUT BOOLEAN                         *Finished               OPTIONAL
);

typedef
EFI_STATUS
(EFIAPI *EFI_MP_SERVICES_SWITCH_BSP) (
    IN struct _EFI_MP_SERVICES_PROTOCOL *This,
    IN UINTN                            ProcessorNumber,
    IN BOOLEAN                          EnableOldBSP
);

typedef
EFI_STATUS
(EFIAPI *EFI_MP_SERVICES_ENABLEDISABLEAP) (
    IN struct _EFI_MP_SERVICES_PROTOCOL *This,
    IN UINTN                            ProcessorNumber,
    IN BOOLEAN                          EnableAP,
    IN UINT32                           *HealthFlag     OPTIONAL
);

typedef
EFI_STATUS
(EFIAPI *EFI_MP_SERVICES_WHOAMI) (
    IN struct _EFI_MP_SERVICES_PROTOCOL *This,
    OUT UINTN                           *ProcessorNumber
);

typedef
struct _EFI_MP_SERVICES_PROTOCOL {
    EFI_MP_SERVICES_GET_NUMBER_OF_PROCESSORS    GetNumberOfProcessors;
    EFI_MP_SERVICES_GET_PROCESSOR_INFO          GetProcessorInfo;
    EFI_MP_SERVICES_STARTUP_ALL_APS             StartupAllAPs;
    EFI_MP_SERVICES_STARTUP_THIS_AP             StartupThisAP;
    EFI_MP_SERVICES_SWITCH_BSP                  SwitchBSP;
    EFI_MP_SERVICES_ENABLEDISABLEAP             EnableDisableAP;
    EFI_MP_SERVICES_WHOAMI                      WhoAmI;
} EFI_MP_SERVICES_PROTOCOL;

#endif   /* !_EFI_MP_H */

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
/* No-op event notification function.                                   */
/*                                                                     */
/* EDK2's CoreCreateEventInternal rejects a NULL NotifyFunction for    */
/* EVT_NOTIFY_WAIT / EVT_NOTIFY_SIGNAL events with EFI_INVALID_PARAMETER*/
/* (MdeModulePkg/Core/Dxe/Event/Event.c). We create an EVT_NOTIFY_WAIT  */
/* event only to obtain a valid EFI_EVENT handle to pass as the         */
/* WaitEvent argument to StartupThisAP (a non-NULL WaitEvent makes it   */
/* return immediately after dispatching the AP). We never actually wait */
/* on the event, so the notification function is never invoked; a no-op */
/* satisfies the non-NULL requirement. This mirrors EDK2's own          */
/* EfiEventEmptyFunction().                                             */
/* ------------------------------------------------------------------ */
static VOID EFIAPI
bridge_ap_done_notify(EFI_EVENT event, VOID *context)
{
    (VOID)event;
    (VOID)context;
}

/* ------------------------------------------------------------------ */
/* AP detach from UEFI (bare-metal page tables + CR3 switch + CLI).    */
/*                                                                     */
/* The AP procedure launched via StartupThisAP() is structurally bound  */
/* to the UEFI boot environment's lifecycle. When the BSP calls         */
/* ExitBootServices(), the firmware forcibly aborts the AP driver,      */
/* clears its processor state, and parks the core. To survive, the AP   */
/* must detach from UEFI before ExitBootServices:                       */
/*   1. Disable interrupts (CLI).                                       */
/*   2. Build independent page tables mapping the regions the bridge    */
/*      needs (the reserved region incl. VIRTUAL_PS2_BASE, the bridge   */
/*      image/stack, and the XHCI MMIO).                                */
/*   3. Switch CR3 to the new page tables.                              */
/*   4. Enter an infinite loop (bridge_entry never returns).            */
/*                                                                     */
/* After this returns, the AP no longer uses UEFI's page tables, GDT,   */
/* or interrupt services; it runs in its own bare-metal environment.    */
/* ------------------------------------------------------------------ */

/* x86-64 page-table entry bits. */
#define AP_PT_PRESENT   (1ULL << 0)
#define AP_PT_WRITABLE  (1ULL << 1)
#define AP_PT_PS        (1ULL << 7)   /* 2 MiB large page */

#define AP_PT_ENTRIES   512
#define AP_PT_2MB       0x200000ULL   /* 2 MiB */

/* Number of 2 MiB pages to map for the XHCI MMIO BAR (4 MiB total). */
#define AP_XHCI_MAP_2MB 2

/* Pre-allocated page-table pages (8 x 4 KiB, page-aligned via AllocatePages).
 * Layout matches the x86-64 paging hierarchy:
 *   pml4[0] -> pdpt_low ; pml4[3] -> pdpt_xhci (for the XHCI MMIO at
 *   0xC000000000, PML4 index 3). */
typedef struct {
    UINT64 pml4[AP_PT_ENTRIES];      /* page 0 */
    UINT64 pdpt_low[AP_PT_ENTRIES];  /* page 1 */
    UINT64 pd_low[4][AP_PT_ENTRIES]; /* pages 2-5 (low 4 GB) */
    UINT64 pdpt_xhci[AP_PT_ENTRIES]; /* page 6 */
    UINT64 pd_xhci[AP_PT_ENTRIES];   /* page 7 */
} AP_PAGE_TABLES;

/* Map a 2 MiB-aligned physical address in the pre-allocated page tables. */
static void
ap_map_2mb(AP_PAGE_TABLES *pt, UINT64 phys)
{
    UINT64 pml4_idx = (phys >> 39) & 0x1FF;
    UINT64 pdpt_idx = (phys >> 30) & 0x1FF;
    UINT64 pd_idx   = (phys >> 21) & 0x1FF;
    UINT64 *pdpt;
    UINT64 *pd;

    if (pml4_idx == 0) {
        /* Low 4 GB: use the pre-allocated pdpt_low / pd_low pages. */
        pdpt = pt->pdpt_low;
        pd   = pt->pd_low[pdpt_idx];
    } else {
        /* Other regions (e.g. XHCI MMIO at 0xC000000000): use the
         * pre-allocated pdpt_xhci / pd_xhci pages. */
        pdpt = pt->pdpt_xhci;
        pd   = pt->pd_xhci;
    }

    pt->pml4[pml4_idx] = (UINT64)(UINTN)pdpt | AP_PT_PRESENT | AP_PT_WRITABLE;
    pdpt[pdpt_idx]     = (UINT64)(UINTN)pd   | AP_PT_PRESENT | AP_PT_WRITABLE;
    pd[pd_idx]         = phys | AP_PT_PRESENT | AP_PT_WRITABLE | AP_PT_PS;
}

/* Detach the AP from UEFI: disable interrupts, build independent page
 * tables, and switch CR3. Runs on the AP before bridge_entry(). */
static void
bridge_ap_detach(void)
{
    AP_PAGE_TABLES *pt;
    EFI_STATUS status;
    EFI_PHYSICAL_ADDRESS pt_addr = 0;
    USB_TOPOLOGY *topo;
    UINT64 xhci_base = 0;
    UINT64 map_base;
    UINT64 i;

    /* 1. Disable interrupts. The bridge runs with interrupts off; it never
     *    needs UEFI's timer/IRQ services after detaching, and disabling
     *    interrupts prevents the AP from generating IRQs that could
     *    interfere with the BSP's UEFI environment. */
    __asm__ __volatile__("cli" : : : "memory");

    /* 2. Allocate page-aligned memory for the independent page tables
     *    (8 pages = 32 KiB). EfiReservedMemoryType so the OS never reclaims
     *    them after ExitBootServices. */
    status = uefi_call_wrapper(
        BS->AllocatePages, 4, AllocateAnyPages, EfiReservedMemoryType,
        8, &pt_addr);
    if (EFI_ERROR(status)) {
        return;   /* Cannot detach; bridge_entry runs under UEFI's tables. */
    }
    pt = (AP_PAGE_TABLES *)(UINTN)pt_addr;

    /* 3. Zero the page tables. */
    for (i = 0; i < (sizeof(*pt) / sizeof(UINT64)); i++)
        ((UINT64 *)pt)[i] = 0;

    /* 4. Identity-map the low 4 GB using 2 MiB large pages. This covers the
     *    reserved region (0x10000000..0x10000040, incl. VIRTUAL_PS2_BASE),
     *    the bridge image, and the bridge stack. */
    for (i = 0; i < (4ULL * 1024 * 1024 * 1024 / AP_PT_2MB); i++)
        ap_map_2mb(pt, i * AP_PT_2MB);

    /* 5. Map the XHCI MMIO region (from the recorded topology). */
    topo = usb_topology_lookup();
    if (topo != NULL)
        xhci_base = topo->xhci_mmio_base;
    if (xhci_base != 0) {
        map_base = xhci_base & ~(AP_PT_2MB - 1);
        for (i = 0; i < AP_XHCI_MAP_2MB; i++)
            ap_map_2mb(pt, map_base + i * AP_PT_2MB);
    }

    /* 6. Switch CR3 to the new page tables. This flushes the TLB; the next
     *    instruction fetch and data access use the independent tables. */
    __asm__ __volatile__("movq %0, %%cr3" : : "r"((UINT64)(UINTN)pt)
                         : "memory");
}

/* AP procedure.                                                       */
/*                                                                     */
/* Runs on the bridge core in its native environment (long mode, UEFI   */
/* page tables/GDT/stack already set up by the firmware). Switches to   */
/* the reserved-region bridge stack, signals the BSP, detaches from     */
/* UEFI (independent page tables, CR3 switch, CLI), and calls           */
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

    /* Detach from UEFI: disable interrupts, build independent page tables,
     * and switch CR3. The bridge then runs in its own bare-metal
     * environment and survives ExitBootServices. */
    bridge_ap_detach();

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
    EFI_EVENT ap_done_event = NULL;
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

    /* 3. Identify the BSP and pick the highest-numbered ENABLED AP as the
     *    bridge core. APIC IDs are not guaranteed contiguous, so we use the
     *    processor NUMBER (index into the MP Services enumeration), not a
     *    raw APIC ID.
     *
     *    CRITICAL: StartupThisAP returns EFI_INVALID_PARAMETER if the
     *    requested processor is the BSP or a DISABLED AP. We must therefore
     *    skip any AP whose StatusFlag lacks PROCESSOR_ENABLED_BIT, and only
     *    hand an enabled AP to StartupThisAP. */
    for (i = 0; i < num_processors; i++) {
        EFI_PROCESSOR_INFORMATION info;
        status = uefi_call_wrapper(
            mp->GetProcessorInfo, 3, mp, i, &info);
        if (EFI_ERROR(status))
            continue;

#ifdef BRIDGE_DEBUG
        Print(L"BRIDGE-DBG:   processor %d: StatusFlag=0x%x (BSP=%d, "
              L"enabled=%d)\n",
              i, info.StatusFlag,
              (info.StatusFlag & PROCESSOR_AS_BSP_BIT) ? 1 : 0,
              (info.StatusFlag & PROCESSOR_ENABLED_BIT) ? 1 : 0);
#endif

        if (info.StatusFlag & PROCESSOR_AS_BSP_BIT) {
#ifdef BRIDGE_DEBUG
            bsp_number = i;
#endif
            continue;
        }

        /* Skip disabled APs: StartupThisAP rejects them with
         * EFI_INVALID_PARAMETER. */
        if (!(info.StatusFlag & PROCESSOR_ENABLED_BIT))
            continue;

        /* Track the highest-numbered enabled AP. */
        if (!found || i > highest_ap) {
            highest_ap = i;
            found = TRUE;
        }
    }

    if (!found) {
#ifdef BRIDGE_DEBUG
        Print(L"BRIDGE-DBG: no ENABLED AP found (num_processors=%d, "
              L"num_enabled=%d)\n",
              num_processors, num_enabled);
#endif
        return EFI_UNSUPPORTED;
    }

#ifdef BRIDGE_DEBUG
    Print(L"BRIDGE-DBG: MP Services: %d processors (%d enabled), BSP=%d, "
          L"bridge AP=%d\n",
          num_processors, num_enabled, bsp_number, highest_ap);
#endif

    /* 4. Allocate a stack for the bridge core in the reserved region. */
    status = uefi_call_wrapper(
        BS->AllocatePages, 4, AllocateAnyPages, EfiReservedMemoryType,
        BRIDGE_STACK_SIZE / 4096, &stack_addr);
    if (EFI_ERROR(status))
        return status;
    g_bridge_stack_top = stack_addr + BRIDGE_STACK_SIZE;

    /* 5. Start the highest AP. StartupThisAP wakes the AP in its native
     *    environment (long mode) and runs bridge_ap_entry on it.
     *
     *    CRITICAL: WaitEvent MUST be non-NULL. If WaitEvent is NULL,
     *    StartupThisAP BLOCKS until the AP procedure FINISHES. But
     *    bridge_ap_entry never returns (it calls bridge_entry(), which loops
     *    forever), so a blocking StartupThisAP would hang the BSP forever.
     *    Passing a valid event makes StartupThisAP return immediately after
     *    dispatching the AP; the event is signaled only when the AP finishes
     *    (which never happens here, and we never wait on it). We then verify
     *    the AP actually started via the g_ap_booted handshake. */
    status = uefi_call_wrapper(
        BS->CreateEvent, 5, EVT_NOTIFY_WAIT, TPL_NOTIFY,
        bridge_ap_done_notify,  /* NotifyFunction: MUST be non-NULL for
                                 * EVT_NOTIFY_WAIT, else EDK2 returns
                                 * EFI_INVALID_PARAMETER. Never invoked
                                 * (we never wait on the event). */
        NULL,                   /* NotifyContext */
        &ap_done_event);
    if (EFI_ERROR(status))
        return status;

    /* 5a. Ensure the target AP is enabled and idle before starting it.
     *
     *     Some OVMF/EDK2 builds report PROCESSOR_ENABLED_BIT set from
     *     GetProcessorInfo yet keep the AP's internal state at
     *     CpuStateDisabled (the two are derived from different sources in
     *     older MpInitLib revisions). StartupThisAPWorker rejects a
     *     CpuStateDisabled AP with EFI_INVALID_PARAMETER regardless of the
     *     reported StatusFlag. Calling EnableDisableAP(EnableAP=TRUE) is
     *     idempotent: it resets the AP to CpuStateIdle (via
     *     ResetProcessorToIdleState) if it was disabled, and is a no-op if
     *     it was already enabled. This closes the StatusFlag/state gap so
     *     StartupThisAP accepts the processor. */
    status = uefi_call_wrapper(
        mp->EnableDisableAP, 4, mp, highest_ap, TRUE, NULL);
#ifdef BRIDGE_DEBUG
    Print(L"BRIDGE-DBG: EnableDisableAP(bridge AP=%d, enable=1) status=%r\n",
          highest_ap, status);
#endif
    if (EFI_ERROR(status)) {
        /* Not fatal: the AP may already be enabled. StartupThisAP will
         * report the definitive result. */
        status = EFI_SUCCESS;
    }

    g_ap_booted = 0;
    status = uefi_call_wrapper(
        mp->StartupThisAP, 7, mp, bridge_ap_entry, highest_ap,
        ap_done_event,    /* WaitEvent: non-NULL = non-blocking (returns
                           * immediately after dispatching the AP) */
        1000000,          /* TimeoutInMicroseconds: 1 s */
        NULL,             /* ProcedureArgument */
        NULL);            /* Finished */

    /* The event is never signaled (the AP never finishes), so we do not wait
     * on it. Close it to avoid leaking the handle. */
    uefi_call_wrapper(BS->CloseEvent, 1, ap_done_event);

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
