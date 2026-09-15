/*
 * xhci.c - B1: XHCI driver for the two pre-discovered endpoints.
 *
 * Drives the single keyboard and mouse interrupt IN endpoints discovered at
 * boot (U1). XHCI >= 1.0 only (C6); no SuperSpeed support - keyboards and
 * mice are low/full-speed on USB 2.0 root-hub ports, so only two low/full-
 * speed interrupt IN endpoints are driven. No enumeration at runtime, no
 * hotplug, no interrupts - minimal periodic polling.
 *
 * NOTE: This is a scaffold. The XHCI register programming (doorbells, TRB
 * rings, event rings) is filled in by the Firmware Coder
 * (see docs/architecture.md section 7.1, module B1).
 */

#include <efi.h>

#include "bridge.h"

void
bridge_poll_usb(void)
{
    /* TODO(Firmware Coder):
     *   1. Program the two interrupt IN endpoints (keyboard + mouse) as
     *      periodic IN transfers on the pre-discovered endpoints.
     *   2. Poll the event ring / doorbell for completed transfers.
     *   3. Produce raw HID reports for B2. */
}
