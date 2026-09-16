/*
 * hid_event.h - Internal event/stream structures shared between the bridge
 * translation modules (B2 -> B3 -> B4).
 *
 * This is an INTERNAL header, not part of the public ABI. It defines the
 * normalized HID event queue produced by B2 (hid_parser.c) and consumed by
 * B3 (hid_ps2.c), and the PS/2 output stream produced by B3 and consumed by
 * B4 (virtual_ps2_writer.c).
 *
 * The queue/stream objects themselves are defined (file-local) in their
 * producing translation unit and exposed here via extern so the consuming
 * module can see them.
 */

#ifndef HID_EVENT_H
#define HID_EVENT_H

#include <efi.h>
#include <hid.h>

/* One normalized HID key event: a single make/break transition. */
typedef struct {
    UINT8   usage;      /* HID usage code (1..0x65) */
    BOOLEAN make;       /* TRUE = make, FALSE = break */
    UINT8   modifier;   /* current modifier byte */
} HID_KEY_EVENT;

/* One normalized HID mouse event: buttons + signed deltas. */
typedef struct {
    UINT8 buttons;    /* bit0 left, bit1 right, bit2 middle */
    INT8  dx;
    INT8  dy;
} HID_MOUSE_EVENT;

/* Internal event queue produced by B2, consumed by B3. */
#define HID_EVENT_QUEUE_MAX 16

typedef struct {
    HID_KEY_EVENT   keys[HID_EVENT_QUEUE_MAX];
    UINTN           key_count;
    HID_MOUSE_EVENT mouse;
    BOOLEAN         mouse_valid;
} HID_EVENT_QUEUE;

extern HID_EVENT_QUEUE g_hid_events;

/* Internal PS/2 output streams produced by B3, consumed by B4.
 *
 * The keyboard and mouse are kept on SEPARATE streams so the two byte
 * streams are never ambiguous (the bridge prioritizes keyboard over mouse
 * when writing the single virtual data slot). */
#define PS2_STREAM_MAX 32

typedef struct {
    UINT8  bytes[PS2_STREAM_MAX];
    UINTN  count;
} PS2_STREAM;

/* Keyboard: PS/2 Set 1 scancodes (make/break, 0xE0-prefixed extended). */
extern PS2_STREAM g_ps2_kbd_stream;

/* Mouse: 3-byte packets [buttons, dx, dy]. */
extern PS2_STREAM g_ps2_mouse_stream;

#endif /* HID_EVENT_H */
