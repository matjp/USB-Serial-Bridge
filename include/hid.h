/*
 * hid.h - USB HID boot-protocol report structures.
 *
 * The bridge reads boot-protocol reports from the two pre-discovered
 * low/full-speed interrupt endpoints (see docs/architecture.md section 7.1,
 * module B2). Fixed topology: exactly one keyboard and one mouse, no hotplug.
 */

#ifndef HID_H
#define HID_H

#include <efi.h>

/* Boot-protocol keyboard report: 8 bytes. */
#define HID_KBD_REPORT_SIZE  8

typedef struct {
    UINT8 modifier;   /* bit0 LCtrl, bit1 LShift, bit2 LAlt, bit3 LGUI,
                         bit4 RCtrl, bit5 RShift, bit6 RAlt, bit7 RGUI */
    UINT8 reserved;
    UINT8 key[6];     /* up to 6 simultaneously pressed usage codes */
} HID_KBD_REPORT;

/* Boot-protocol mouse report: 3 bytes. */
#define HID_MOUSE_REPORT_SIZE  3

typedef struct {
    UINT8 buttons;    /* bit0 left, bit1 right, bit2 middle */
    INT8  dx;         /* signed X delta */
    INT8  dy;         /* signed Y delta */
} HID_MOUSE_REPORT;

#endif /* HID_H */
