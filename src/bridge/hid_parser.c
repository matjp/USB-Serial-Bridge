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
#include "hid_event.h"

/* ------------------------------------------------------------------ */
/* B1 (xhci.c) defines these; declared extern here.                    */
/* ------------------------------------------------------------------ */
extern HID_KBD_REPORT   g_raw_kbd;    /* 8 bytes */
extern HID_MOUSE_REPORT g_raw_mouse;  /* 3 bytes */
extern BOOLEAN g_kbd_valid;           /* set when a fresh kbd report is ready */
extern BOOLEAN g_mouse_valid;         /* set when a fresh mouse report is ready */

/* Internal event queue consumed by B3. */
HID_EVENT_QUEUE g_hid_events;

/* Previous keyboard report, for make/break diffing. */
static HID_KBD_REPORT g_prev_kbd;
/* Previous modifier byte, for modifier make/break diffing. */
static UINT8 g_prev_modifier;

/* Modifier bit -> HID usage (bit0 LCtrl ... bit7 RGui). These usages
 * (0xE0-0xE7) never appear in the key[] array; they are tracked via the
 * report's modifier byte. */
static const UINT8 g_mod_usage[8] = {
    0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7
};

/* Append one key event to the queue, dropping it if the queue is full. */
static void
emit_key_event(UINT8 usage, BOOLEAN make, UINT8 modifier)
{
    if (g_hid_events.key_count < HID_EVENT_QUEUE_MAX) {
        g_hid_events.keys[g_hid_events.key_count].usage    = usage;
        g_hid_events.keys[g_hid_events.key_count].make     = make;
        g_hid_events.keys[g_hid_events.key_count].modifier = modifier;
        g_hid_events.key_count++;
    }
}

/* Diff the current keyboard report against the previous one and emit one
 * HID_KEY_EVENT per make/break transition. */
static void
parse_keyboard(void)
{
    UINTN i, j;
    UINT8 cur, prev;
    BOOLEAN found;

    /* Modifier byte diff: emit a make/break for each changed modifier bit.
     * This is the only path by which the modifier keys (0xE0-0xE7) reach B3,
     * since they are not present in the key[] array. */
    for (i = 0; i < 8; i++) {
        UINT8 mask = (UINT8)(1u << i);
        if ((g_raw_kbd.modifier & mask) != (g_prev_modifier & mask)) {
            emit_key_event(g_mod_usage[i],
                           (g_raw_kbd.modifier & mask) != 0,
                           g_raw_kbd.modifier);
        }
    }
    g_prev_modifier = g_raw_kbd.modifier;

    /* Make: keys present now but not before. */
    for (i = 0; i < 6; i++) {
        cur = g_raw_kbd.key[i];
        if (cur == 0)
            continue;
        found = FALSE;
        for (j = 0; j < 6; j++) {
            if (g_prev_kbd.key[j] == cur) {
                found = TRUE;
                break;
            }
        }
        if (!found)
            emit_key_event(cur, TRUE, g_raw_kbd.modifier);
    }

    /* Break: keys present before but not now. */
    for (j = 0; j < 6; j++) {
        prev = g_prev_kbd.key[j];
        if (prev == 0)
            continue;
        found = FALSE;
        for (i = 0; i < 6; i++) {
            if (g_raw_kbd.key[i] == prev) {
                found = TRUE;
                break;
            }
        }
        if (!found)
            emit_key_event(prev, FALSE, g_raw_kbd.modifier);
    }

    /* Save the current report as the new previous report. */
    g_prev_kbd = g_raw_kbd;
}

/* Emit one mouse event from the current mouse report. */
static void
parse_mouse(void)
{
    g_hid_events.mouse.buttons = g_raw_mouse.buttons;
    g_hid_events.mouse.dx      = g_raw_mouse.dx;
    g_hid_events.mouse.dy      = g_raw_mouse.dy;
    g_hid_events.mouse_valid   = TRUE;
}

void
bridge_parse_hid(void)
{
    if (g_kbd_valid)
        parse_keyboard();
    if (g_mouse_valid)
        parse_mouse();

    /* Consume the raw reports. */
    g_kbd_valid   = FALSE;
    g_mouse_valid = FALSE;
}
