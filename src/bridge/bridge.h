/*
 * bridge.h - Internal interface for the bridge core (Phase 2).
 *
 * The bridge runs on the highest core, TDM-shared with the OS's background
 * task (Seth for TempleOS). It drives the two pre-discovered low/full-speed
 * interrupt endpoints, translates HID -> virtual PS/2, and writes the byte
 * stream to the mailbox. See docs/architecture.md section 7.1.
 */

#ifndef BRIDGE_H
#define BRIDGE_H

#include <efi.h>
#include <mailbox.h>

/* B1: Poll the two interrupt IN endpoints, produce raw HID reports. */
void bridge_poll_usb(void);

/* B1: Return TRUE if the USB controller is unusable (non-XHCI>=1.0 or a
 * fatal fault). The bridge halts cleanly when this is set. */
BOOLEAN bridge_usb_fatal(void);

/* B2: Parse raw HID reports into key/mouse events. */
void bridge_parse_hid(void);

/* B3: Translate HID events into PS/2 Set 1 + mouse packets. */
void bridge_translate_ps2(void);

/* B4: Write the PS/2 byte streams to the two mailboxes (kbd + mouse). */
void bridge_write_mailbox(MAILBOX *kbd_mb, MAILBOX *mouse_mb);

/* B5: Bridge core entry point (called after SIPI bring-up). */
void bridge_entry(void);

#endif /* BRIDGE_H */
