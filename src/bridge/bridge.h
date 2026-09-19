/*
 * bridge.h - Internal interface for the bridge core (Phase 2).
 *
 * The bridge runs on the highest core, TDM-shared with the OS's background
 * task. It drives the two pre-discovered low/full-speed interrupt endpoints,
 * translates HID -> virtual PS/2, and writes the byte stream to the virtual
 * 8042 port region. See docs/architecture.md section 7.1.
 */

#ifndef BRIDGE_H
#define BRIDGE_H

#include <efi.h>
#include "xhci_observer.h"

/* B1: Poll the two interrupt IN endpoints, produce raw HID reports. */
void bridge_poll_usb(void);

/* B1: Read-only passive observer (pre-ExitBootServices). Polls UEFI's
 * xHCI event ring read-only and fills the raw HID reports, duplicating
 * packet data into the virtual PS/2 region. NEVER writes to the xHCI
 * controller (the BSP's XhciDxe driver owns it before EBS). */
void bridge_observer_poll(const XHCI_OBSERVER *obs);

/* B1: Sync the passive observer's own event-ring dequeue index + cycle
 * bit to the controller's current ERDP (captured in obs->erdp during
 * extraction). Call once before the first bridge_observer_poll so the
 * observer starts reading genuinely-new events from where UEFI's XhciDxe
 * left off, instead of stale events (or a cycle-bit mismatch) at index 0. */
void bridge_observer_sync(const XHCI_OBSERVER *obs);

/* B1: True takeover of UEFI's xHCI rings (post-ExitBootServices). The AP
 * is now the sole owner: it continues polling UEFI's event ring and now
 * also writes ERDP + re-arms the transfer rings + rings the doorbells.
 * No reset, no ring/device-context re-creation. */
void bridge_takeover_poll(const XHCI_OBSERVER *obs);

/* B1: Return TRUE if the USB controller is unusable (non-XHCI>=1.0 or a
 * fatal fault). The bridge halts cleanly when this is set. */
BOOLEAN bridge_usb_fatal(void);

/* B2: Parse raw HID reports into key/mouse events. */
void bridge_parse_hid(void);

/* B3: Translate HID events into PS/2 Set 1 + mouse packets. */
void bridge_translate_ps2(void);

/* B4: Write the PS/2 byte streams to the virtual 8042 port region (see
 * include/virtual_ps2.h). One byte per call, 8042 single-output-buffer
 * semantics. */
void bridge_write_virtual_ps2(void);

/* B4: Reset the writer's internal write cursors. Used by host tests
 * between cases. */
void bridge_virtual_ps2_reset(void);

/* B5: Bridge core entry point (called after SIPI bring-up). */
void bridge_entry(void);

#endif /* BRIDGE_H */
