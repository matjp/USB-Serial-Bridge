/*
 * ps2.h - PS/2 Set 1 scancode and mouse-packet definitions.
 *
 * The bridge translates HID reports into a virtual PS/2 byte stream that the
 * virtual 8042 port region delivers (see docs/architecture.md section 6). The
 * OS reuses its existing Set 1 decoder and 3-byte mouse-packet parser
 * unchanged.
 */

#ifndef PS2_H
#define PS2_H

#include <efi.h>

/* PS/2 Set 1 extended-prefix byte. */
#define PS2_EXT_PREFIX  0xE0

/* PS/2 Set 1 break-code bit (make | 0x80 = break). */
#define PS2_BREAK_BIT   0x80

/* PS/2 mouse packet: 3 bytes [buttons, dx, dy]. */
#define PS2_MOUSE_PKT_SIZE  3

typedef struct {
    UINT8 buttons;   /* bit0 left, bit1 right, bit2 middle */
    INT8  dx;        /* signed X delta */
    INT8  dy;        /* signed Y delta */
} PS2_MOUSE_PKT;

#endif /* PS2_H */
