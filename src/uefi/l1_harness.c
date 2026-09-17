/*
 * l1_harness.c - Layer 1 harness: surface the XHCI bring-up fault report.
 *
 * The bridge core (B1) records a structured XHCI_FAULT in the reserved region
 * when UEFI->XHCI handoff fails (see xhci_fault.h). This harness runs on the
 * BSP (core 0) in the UEFI app, right after the bridge is SIPI-started, and
 * BEFORE the OS is handed off. If the bridge faulted, it prints a compact,
 * human-readable diagnosis to the UEFI console (ConOut) and halts - it never
 * boots the OS. If the bridge is healthy, it returns and the app proceeds.
 *
 * Console budget: the UEFI spec guarantees mode 0 is 80 cols x 25 rows, so
 * the whole diagnosis is kept to ~12 lines to fit on one screen.
 */

#include <efi.h>
#include <efilib.h>

#include "uefi.h"
#include "../bridge/xhci_fault.h"
#include "../bridge/xhci_status.h"

/* Bounded spin so the bridge has time to run its first bring-up poll after
 * SIPI. Bring-up is microseconds-to-low-milliseconds; a few million empty
 * iterations is comfortably more than enough and well under a second. */
#define BRIDGE_WAIT_ITERS  4000000u

static void
spin(volatile UINT32 n)
{
    volatile UINT32 i;
    for (i = 0; i < n; i++)
        __asm__ __volatile__("" ::: "memory");
}

/* Human-readable names for the bring-up stages. Must match xhci_fault.h. */
static const CHAR16 *const g_stage_names[] = {
    L"none", L"verify", L"reset", L"rings", L"devices",
    L"transfer", L"run", L"doorbell", L"poll"
};

/* Human-readable hints for the harness readout. Must match xhci_fault.h. */
static const CHAR16 *const g_hint_names[] = {
    L"none",
    L"controller is not XHCI >= 1.0",
    L"HCRST/HCHalted never cleared",
    L"command/event ring setup failed",
    L"device context / DCBAA setup failed",
    L"transfer ring init failed",
    L"controller did not start (RUN)",
    L"doorbell write failed / no response",
    L"event ring desync / bad completion"
};

/* Wait for the bridge to publish its fault record and complete its first
 * bring-up attempt. Returns the fault record if the bridge faulted, else
 * NULL (bridge is healthy). */
static XHCI_FAULT *
wait_for_bridge_fault(void)
{
    XHCI_FAULT *fault;
    UINT32 i;

    /* Wait for the bridge to publish the fault-record pointer (it does so at
     * the start of its first poll). */
    for (i = 0; i < BRIDGE_WAIT_ITERS; i++) {
        fault = xhci_fault_lookup();
        if (fault != NULL)
            break;
        spin(1);
    }
    if (fault == NULL)
        return NULL;   /* bridge never started; treat as no fault */

    /* Give the bridge time to run its bring-up. If it faulted, magic is set;
     * if it is healthy, magic stays 0 and the bridge keeps polling. */
    spin(BRIDGE_WAIT_ITERS);
    if (fault->magic == XHCI_FAULT_MAGIC)
        return fault;

    return NULL;
}

/* Print a compact, one-screen diagnosis from the fault record. */
static void
print_fault(const XHCI_FAULT *f)
{
    UINT32 stage = f->stage;
    UINT32 hint  = f->hint;

    if (stage >= sizeof(g_stage_names) / sizeof(g_stage_names[0]))
        stage = 0;
    if (hint >= sizeof(g_hint_names) / sizeof(g_hint_names[0]))
        hint = 0;

    Print(L"\n");
    Print(L"*** BRIDGE FAULT: XHCI bring-up failed ***\n");
    Print(L"  Stage : %s (%d)\n", g_stage_names[stage], f->stage);
    Print(L"  Hint  : %s\n", g_hint_names[hint]);
    Print(L"  USBSTS: 0x%08X\n", f->usbsts);
    Print(L"  USBCMD: 0x%08X\n", f->usbcmd);
    Print(L"  CRCR  : 0x%08X\n", f->crcr);
    Print(L"  LastCC: 0x%02X  LastTRB: 0x%02X  Doorbell: 0x%02X\n",
          f->last_cc, f->last_trb_type, f->doorbell);
    Print(L"*** Halting; not handing off to the OS. ***\n");
}

#ifdef BRIDGE_DEBUG
/* Debug builds only: wait for the user to press a key so the on-screen
 * bring-up report can be photographed before the app proceeds or halts. */
static void
wait_for_key(void)
{
    EFI_INPUT_KEY key;
    EFI_STATUS status;

    Print(L"\nPRESS A KEY TO CONTINUE...\n");

    /* Drain any pending keystrokes, then block until a fresh key is pressed. */
    for (;;) {
        status = uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 1, ST->ConIn, &key);
        if (!EFI_ERROR(status))
            break;
    }
}

/* Debug builds only: wait for the bridge to publish its success record and
 * print the actual XHCI hardware state. Called when the bridge is healthy
 * (no fault), so the state is available for downstream verification. */
static void
print_bridge_status(void)
{
    XHCI_STATUS *st;
    UINT32 i;

    /* Wait (bounded) for the bridge to publish the success record. */
    for (i = 0; i < BRIDGE_WAIT_ITERS; i++) {
        st = xhci_status_lookup();
        if (st != NULL && st->magic == XHCI_STATUS_MAGIC)
            break;
        spin(1);
    }
    if (st == NULL || st->magic != XHCI_STATUS_MAGIC)
        return;   /* no success record (non-debug bridge or not reached) */

    Print(L"\n");
    Print(L"*** BRIDGE OK: XHCI bring-up succeeded ***\n");
    Print(L"  MMIO  : 0x%016llX  CAPLEN: 0x%02X\n",
          (unsigned long long)st->xhci_mmio_base, st->xhci_cap_len);
    Print(L"  Slots : %d  Eps: %d  Scratch: %d  Page: 0x%04X\n",
          st->max_slots, st->max_eps, st->max_scratchpad, st->page_size);
    Print(L"  USBSTS: 0x%08X  USBCMD: 0x%08X  CRCR: 0x%08X\n",
          st->usbsts, st->usbcmd, st->crcr);
    Print(L"  Kbd   : addr %d  ep 0x%02X  int %d  spd %d  pkt %d\n",
          st->kbd & 0xFF, (st->kbd >> 8) & 0xFF, (st->kbd >> 16) & 0xFF,
          (st->kbd >> 24) & 0xFF, st->kbd_max_packet);
    Print(L"  Mouse : addr %d  ep 0x%02X  int %d  spd %d  pkt %d\n",
          st->mouse & 0xFF, (st->mouse >> 8) & 0xFF, (st->mouse >> 16) & 0xFF,
          (st->mouse >> 24) & 0xFF, st->mouse_max_packet);

    /* Pause so the user can photograph the success report. */
    wait_for_key();
}
#endif /* BRIDGE_DEBUG */

/* Check the bridge fault record after bring-up. If the bridge faulted, print
 * the diagnosis and halt (never boot the OS). Otherwise return normally. */
void
uefi_check_bridge_fault(void)
{
    XHCI_FAULT *fault = wait_for_bridge_fault();

    if (fault == NULL) {
#ifdef BRIDGE_DEBUG
        /* Bridge is healthy; in debug builds print the hardware state. */
        print_bridge_status();
#endif
        return;   /* bridge is healthy; proceed to hand off */
    }

    print_fault(fault);

#ifdef BRIDGE_DEBUG
    /* Debug builds: pause so the user can photograph the fault report
     * before the BSP halts. */
    wait_for_key();
#endif

    /* Halt on the BSP so the diagnosis stays on screen. */
    for (;;)
        __asm__ __volatile__("hlt");
}
