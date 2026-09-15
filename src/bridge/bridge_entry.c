/*
 * bridge_entry.c - B5: Bridge core entry point.
 *
 * Runs on the highest core after SIPI bring-up. TDM-shares the core with the
 * OS's background task (Seth for TempleOS): the bridge polls the USB
 * endpoints in its time slice, then yields the core back to the OS task.
 *
 * NOTE: This is a scaffold. The TDM scheduling handshake with the OS task is
 * filled in by the Firmware Coder (see docs/architecture.md section 7.1,
 * module B5).
 */

#include <efi.h>
#include <mailbox.h>

#include "bridge.h"

void
bridge_entry(void)
{
    MAILBOX *mb = mailbox_lookup();

    for (;;) {
        /* Poll the two interrupt IN endpoints (B1). */
        bridge_poll_usb();

        /* Parse HID reports (B2) and translate to PS/2 (B3). */
        bridge_parse_hid();
        bridge_translate_ps2();

        /* Write the byte stream to the mailbox (B4). */
        if (mb)
            bridge_write_mailbox(mb);

        /* TODO(Firmware Coder): TDM handshake - yield the core back to the
         * OS background task (Seth) until the next bridge time slice. */
    }
}
