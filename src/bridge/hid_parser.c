/*
 * hid_parser.c - B2: HID report parsing.
 *
 * Parses the raw boot-protocol reports from B1 into key/mouse events.
 * Keyboard: 8-byte boot report (modifier + up to 6 usage codes). Mouse:
 * 3-byte boot report (buttons + signed dx/dy).
 *
 * NOTE: This is a scaffold. The HID usage-code parsing is filled in by the
 * Firmware Coder (see docs/architecture.md section 7.1, module B2).
 */

#include <efi.h>
#include <hid.h>

#include "bridge.h"

void
bridge_parse_hid(void)
{
    /* TODO(Firmware Coder):
     *   1. Parse the 8-byte keyboard boot report (HID_KBD_REPORT).
     *   2. Parse the 3-byte mouse boot report (HID_MOUSE_REPORT).
     *   3. Produce normalized key/mouse events for B3. */
}
