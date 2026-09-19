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
#include "bridge_debug.h"

void
bridge_entry(void)
{
#ifdef BRIDGE_DEBUG
    /* Persistent virtual debug serial: mark that the bridge AP is alive and
     * entered its main loop. This is the first thing the BSP harness (and
     * later the OS) looks for to confirm the AP is actually executing the
     * bridge code. */
    bridge_debug_puts((const CHAR8 *)"BRIDGE AP ALIVE: bridge_entry entered");
#endif

    for (;;) {
        /* If the USB controller is unusable (non-XHCI>=1.0 or a fatal
         * fault), halt cleanly in an idle loop. */
        if (bridge_usb_fatal()) {
#ifdef BRIDGE_DEBUG
            bridge_debug_puts((const CHAR8 *)"BRIDGE FATAL: halting in idle loop");
#endif
            for (;;)
                __asm__ __volatile__("hlt");
        }

        /* Poll the two interrupt IN endpoints (B1). */
        bridge_poll_usb();

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
