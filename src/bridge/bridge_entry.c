/*
 * bridge_entry.c - B5: Bridge core entry point.
 *
 * Runs on the highest core after SIPI bring-up. TDM-shares the core with the
 * OS's background task: the bridge polls the USB endpoints in its time
 * slice, then yields the core back to the OS task.
 *
 * The bridge polls the two interrupt IN endpoints (B1), parses HID (B2),
 * translates to PS/2 (B3), and writes the byte stream into the virtual 8042
 * port region (B4) within its 2 ms time slice. It then yields the core back
 * to the OS task via the TDM handshake (tdm_switch_to_os) until the next
 * bridge time slice.
 *
 * If B1 detects a non-XHCI>=1.0 controller or a fatal fault, the bridge
 * halts cleanly in an idle loop instead of spinning on a dead controller.
 */

#include <efi.h>

#include "bridge.h"
#include "tdm.h"
#include "xhci_observer.h"

/* Post-ExitBootServices flag, set by the BSP's ExitBootServices
 * notification (see core_bringup.c). Before EBS the bridge AP must remain
 * a completely PASSIVE read-only observer of the xHCI rings; only after
 * EBS may it take over the rings (write ERDP + re-arm + doorbell). */
extern volatile UINT8 g_ebs_occurred;

void
bridge_entry(void)
{
    /* Before ExitBootServices, the bridge AP must be a completely PASSIVE
     * data observer of the xHCI rings. The UEFI firmware is single-threaded
     * on the BSP and its XhciDxe driver owns the xHCI state machine; if the
     * AP wrote to the xHCI MMIO operational registers or PCI config space,
     * it would race with the BSP and trigger a #GP / halt / failed
     * ExitBootServices. The observer (bridge_observer_poll) reads UEFI's
     * event ring read-only and duplicates packets into VIRTUAL_PS2_BASE,
     * never writing to the controller. After ExitBootServices, the AP
     * becomes the sole owner and TAKES OVER UEFI's rings (bridge_takeover_poll):
     * it continues polling UEFI's event ring and now also writes ERDP +
     * re-arms the transfer rings + rings the doorbells. No reset, no
     * ring/device-context re-creation. */
    const XHCI_OBSERVER *obs = (const XHCI_OBSERVER *)(UINTN)XHCI_OBSERVER_ADDR;

    for (;;) {
        /* If the USB controller is unusable (non-XHCI>=1.0 or a fatal
         * fault), halt cleanly in an idle loop. */
        if (bridge_usb_fatal()) {
            for (;;)
                __asm__ __volatile__("hlt");
        }

        /* Read-only passive observer (B1, pre-EBS): poll UEFI's event ring
         * and fill the raw HID reports. No writes to the xHCI controller. */
        bridge_observer_poll(obs);

        /* After ExitBootServices, the AP is the sole owner: take over UEFI's
         * rings (write ERDP + re-arm + doorbell). This is the production
         * post-EBS path; bridge_poll_usb() (full from-scratch bring-up) is
         * only kept for the host fault-injection test.
         *
         * CRITICAL: before ExitBootServices the BSP's XhciDxe driver owns
         * the xHCI controller. Writing ERDP / ringing doorbells from the AP
         * before EBS races with the BSP and corrupts the controller state
         * (observed as a #PF on the AP). The AP must therefore remain a
         * passive read-only observer until g_ebs_occurred is set. */
        if (g_ebs_occurred)
            bridge_takeover_poll(obs);

        /* Parse HID reports (B2) and translate to PS/2 (B3). */
        bridge_parse_hid();
        bridge_translate_ps2();

        /* Write the byte streams to the virtual 8042 port region (B4,
         * virtual-port variant). One byte per poll, 8042 semantics. */
        bridge_write_virtual_ps2();

        /* TDM handshake: yield the core back to the OS background task
         * until the next bridge time slice. The timer ISR on this core
         * fires at each slot boundary and switches back to the bridge
         * context (tdm_switch_to_bridge). */
        tdm_switch_to_os();
    }
}
