/*
 * b1_stub.c - Host-only stub for the Layer 0 test build.
 *
 * The raw HID report globals (g_raw_kbd, g_raw_mouse, g_kbd_valid,
 * g_mouse_valid) are owned by B1 (src/bridge/xhci.c) in the firmware build.
 * The host test build does NOT compile xhci.c (it drives real XHCI MMIO and
 * cannot run on the host), so this tiny stub provides those four globals so
 * that B2 (hid_parser.c) and the Layer 0 tests link cleanly.
 *
 * This file is intentionally NOT part of the firmware build - it exists only
 * to keep the host test build green. See tests/Makefile.
 */

#include <efi.h>
#include <hid.h>

HID_KBD_REPORT   g_raw_kbd;
HID_MOUSE_REPORT g_raw_mouse;
BOOLEAN g_kbd_valid   = FALSE;
BOOLEAN g_mouse_valid = FALSE;
