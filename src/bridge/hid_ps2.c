/*
 * hid_ps2.c - B3: HID -> PS/2 translation.
 *
 * Translates parsed HID events into a virtual PS/2 byte stream:
 *   - Keyboard: PS/2 Set 1 scancodes, make and break (0xE0-prefixed extended
 *     codes included). The OS reuses its existing Set 1 decoder.
 *   - Mouse: 3-byte packets [buttons, dx, dy] (two's-complement deltas),
 *     matching the standard PS/2 mouse packet the OS already parses.
 *
 * NOTE: This is a scaffold. The HID usage -> Set 1 scancode table and the
 * mouse packet assembly are filled in by the Firmware Coder
 * (see docs/architecture.md section 7.1, module B3).
 */

#include <efi.h>
#include <ps2.h>

#include "bridge.h"

void
bridge_translate_ps2(void)
{
    /* TODO(Firmware Coder):
     *   1. Map HID keyboard usage codes to PS/2 Set 1 scancodes (make/break,
     *      with 0xE0 extended prefix where required).
     *   2. Assemble 3-byte mouse packets (PS2_MOUSE_PKT).
     *   3. Emit the byte stream for B4. */
}
