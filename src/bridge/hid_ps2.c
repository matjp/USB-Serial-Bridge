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
#include "hid_event.h"

/* ------------------------------------------------------------------ */
/* PS/2 Set 1 scancode table (HID usage -> Set 1 make code).          */
/* ------------------------------------------------------------------ */

/* One entry: the Set 1 make scancode (without any 0xE0 prefix) and whether
 * the key is an extended key (requires the 0xE0 prefix). A scancode of 0
 * means "no mapping" (the key is ignored). */
typedef struct {
    UINT8   scancode;   /* Set 1 make scancode (no 0xE0 prefix) */
    BOOLEAN extended;   /* TRUE if 0xE0 prefix is required */
} PS2_KEY_MAP;

/* Main table indexed directly by HID usage (0x04..0x63). Unused usages
 * (0x00-0x03, 0x32) and the complex multi-byte keys PrintScreen (0x46) and
 * Pause (0x48) map to scancode 0 (ignored) - see report. */
static const PS2_KEY_MAP g_ps2_map[0x64] = {
    /* 0x00-0x03: reserved / no mapping */
    [0x00] = {0x00, FALSE},
    [0x01] = {0x00, FALSE},
    [0x02] = {0x00, FALSE},
    [0x03] = {0x00, FALSE},
    /* 0x04-0x1D: A-Z */
    [0x04] = {0x1C, FALSE}, /* A */
    [0x05] = {0x32, FALSE}, /* B */
    [0x06] = {0x21, FALSE}, /* C */
    [0x07] = {0x23, FALSE}, /* D */
    [0x08] = {0x24, FALSE}, /* E */
    [0x09] = {0x2B, FALSE}, /* F */
    [0x0A] = {0x34, FALSE}, /* G */
    [0x0B] = {0x33, FALSE}, /* H */
    [0x0C] = {0x43, FALSE}, /* I */
    [0x0D] = {0x3B, FALSE}, /* J */
    [0x0E] = {0x42, FALSE}, /* K */
    [0x0F] = {0x4B, FALSE}, /* L */
    [0x10] = {0x3A, FALSE}, /* M */
    [0x11] = {0x31, FALSE}, /* N */
    [0x12] = {0x44, FALSE}, /* O */
    [0x13] = {0x4D, FALSE}, /* P */
    [0x14] = {0x15, FALSE}, /* Q */
    [0x15] = {0x2D, FALSE}, /* R */
    [0x16] = {0x1B, FALSE}, /* S */
    [0x17] = {0x2C, FALSE}, /* T */
    [0x18] = {0x3C, FALSE}, /* U */
    [0x19] = {0x2A, FALSE}, /* V */
    [0x1A] = {0x1D, FALSE}, /* W */
    [0x1B] = {0x22, FALSE}, /* X */
    [0x1C] = {0x35, FALSE}, /* Y */
    [0x1D] = {0x1A, FALSE}, /* Z */
    /* 0x1E-0x27: 1-0 */
    [0x1E] = {0x02, FALSE}, /* 1 */
    [0x1F] = {0x03, FALSE}, /* 2 */
    [0x20] = {0x04, FALSE}, /* 3 */
    [0x21] = {0x05, FALSE}, /* 4 */
    [0x22] = {0x06, FALSE}, /* 5 */
    [0x23] = {0x07, FALSE}, /* 6 */
    [0x24] = {0x08, FALSE}, /* 7 */
    [0x25] = {0x09, FALSE}, /* 8 */
    [0x26] = {0x0A, FALSE}, /* 9 */
    [0x27] = {0x0B, FALSE}, /* 0 */
    /* 0x28-0x38: punctuation & control */
    [0x28] = {0x1C, FALSE}, /* Enter */
    [0x29] = {0x01, FALSE}, /* Esc */
    [0x2A] = {0x0E, FALSE}, /* Backspace */
    [0x2B] = {0x0F, FALSE}, /* Tab */
    [0x2C] = {0x39, FALSE}, /* Space */
    [0x2D] = {0x0C, FALSE}, /* - (minus) */
    [0x2E] = {0x0D, FALSE}, /* = (equals) */
    [0x2F] = {0x1A, FALSE}, /* [ (left bracket) */
    [0x30] = {0x1B, FALSE}, /* ] (right bracket) */
    [0x31] = {0x2B, FALSE}, /* \ (backslash) */
    [0x32] = {0x00, FALSE}, /* (unused) */
    [0x33] = {0x27, FALSE}, /* ; (semicolon) */
    [0x34] = {0x28, FALSE}, /* ' (quote) */
    [0x35] = {0x29, FALSE}, /* ` (grave) */
    [0x36] = {0x33, FALSE}, /* , (comma) */
    [0x37] = {0x34, FALSE}, /* . (period) */
    [0x38] = {0x35, FALSE}, /* / (slash) */
    /* 0x39-0x45: CapsLock, F1-F12 */
    [0x39] = {0x3A, FALSE}, /* CapsLock */
    [0x3A] = {0x3B, FALSE}, /* F1 */
    [0x3B] = {0x3C, FALSE}, /* F2 */
    [0x3C] = {0x3D, FALSE}, /* F3 */
    [0x3D] = {0x3E, FALSE}, /* F4 */
    [0x3E] = {0x3F, FALSE}, /* F5 */
    [0x3F] = {0x40, FALSE}, /* F6 */
    [0x40] = {0x41, FALSE}, /* F7 */
    [0x41] = {0x42, FALSE}, /* F8 */
    [0x42] = {0x43, FALSE}, /* F9 */
    [0x43] = {0x44, FALSE}, /* F10 */
    [0x44] = {0x57, FALSE}, /* F11 */
    [0x45] = {0x58, FALSE}, /* F12 */
    /* 0x46-0x52: navigation */
    [0x46] = {0x00, FALSE}, /* PrintScreen (complex multi-byte; ignored) */
    [0x47] = {0x46, FALSE}, /* ScrollLock */
    [0x48] = {0x00, FALSE}, /* Pause (complex multi-byte; ignored) */
    [0x49] = {0x52, TRUE},  /* Insert */
    [0x4A] = {0x47, TRUE},  /* Home */
    [0x4B] = {0x49, TRUE},  /* PageUp */
    [0x4C] = {0x53, TRUE},  /* Delete */
    [0x4D] = {0x4F, TRUE},  /* End */
    [0x4E] = {0x51, TRUE},  /* PageDown */
    [0x4F] = {0x74, TRUE},  /* Right */
    [0x50] = {0x6B, TRUE},  /* Left */
    [0x51] = {0x72, TRUE},  /* Down */
    [0x52] = {0x75, TRUE},  /* Up */
    /* 0x53-0x63: keypad */
    [0x53] = {0x45, FALSE}, /* NumLock */
    [0x54] = {0x35, TRUE},  /* KP / (divide) */
    [0x55] = {0x37, FALSE}, /* KP * (multiply) */
    [0x56] = {0x4A, FALSE}, /* KP - (minus) */
    [0x57] = {0x4E, FALSE}, /* KP + (plus) */
    [0x58] = {0x1C, TRUE},  /* KP Enter */
    [0x59] = {0x4F, FALSE}, /* KP 1 */
    [0x5A] = {0x50, FALSE}, /* KP 2 */
    [0x5B] = {0x51, FALSE}, /* KP 3 */
    [0x5C] = {0x4B, FALSE}, /* KP 4 */
    [0x5D] = {0x4C, FALSE}, /* KP 5 */
    [0x5E] = {0x4D, FALSE}, /* KP 6 */
    [0x5F] = {0x47, FALSE}, /* KP 7 */
    [0x60] = {0x48, FALSE}, /* KP 8 */
    [0x61] = {0x49, FALSE}, /* KP 9 */
    [0x62] = {0x52, FALSE}, /* KP 0 */
    [0x63] = {0x53, FALSE}, /* KP . (period) */
};

/* Modifier keys (HID usages 0xE0-0xE7). These never appear in the key[]
 * array; B2 emits them from the modifier-byte diff. */
typedef struct {
    UINT8   usage;
    UINT8   scancode;
    BOOLEAN extended;
} PS2_MOD_MAP;

static const PS2_MOD_MAP g_ps2_mods[] = {
    {0xE0, 0x1D, FALSE}, /* LCtrl  */
    {0xE1, 0x2A, FALSE}, /* LShift */
    {0xE2, 0x38, FALSE}, /* LAlt   */
    {0xE3, 0x5B, TRUE},  /* LGui   */
    {0xE4, 0x1D, TRUE},  /* RCtrl  */
    {0xE5, 0x36, FALSE}, /* RShift */
    {0xE6, 0x38, TRUE},  /* RAlt   */
    {0xE7, 0x5C, TRUE},  /* RGui   */
};

/* Internal PS/2 output streams consumed by B4. Keyboard and mouse are kept
 * on separate streams (and, downstream, on separate mailbox rings) so the
 * two byte streams are never ambiguous. */
PS2_STREAM g_ps2_kbd_stream;
PS2_STREAM g_ps2_mouse_stream;

/* Append one byte to the keyboard output stream, dropping it if the stream
 * is full (never corrupts the buffer). */
static void
kbd_put(UINT8 byte)
{
    if (g_ps2_kbd_stream.count < PS2_STREAM_MAX)
        g_ps2_kbd_stream.bytes[g_ps2_kbd_stream.count++] = byte;
}

/* Append one byte to the mouse output stream, dropping it if the stream is
 * full (never corrupts the buffer). */
static void
mouse_put(UINT8 byte)
{
    if (g_ps2_mouse_stream.count < PS2_STREAM_MAX)
        g_ps2_mouse_stream.bytes[g_ps2_mouse_stream.count++] = byte;
}

/* Emit one key make/break sequence for a Set 1 scancode.
 *
 * PS/2 Set 1 break codes are a single byte: (make | 0x80). For extended
 * keys the 0xE0 prefix precedes the make or break byte. A break must NOT
 * also emit the make byte - it is just the 0x80-OR'd code (optionally
 * 0xE0-prefixed). */
static void
emit_key(UINT8 scancode, BOOLEAN extended, BOOLEAN make)
{
    if (extended)
        kbd_put(PS2_EXT_PREFIX);
    if (make)
        kbd_put(scancode);
    else
        kbd_put((UINT8)(scancode | PS2_BREAK_BIT));
}

void
bridge_translate_ps2(void)
{
    UINTN i, m;
    UINT8 usage;
    const PS2_KEY_MAP *km;
    const PS2_MOD_MAP *mm;

    /* Keyboard: translate each queued key event. */
    for (i = 0; i < g_hid_events.key_count; i++) {
        usage = g_hid_events.keys[i].usage;

        if (usage >= 0x04 && usage <= 0x63) {
            km = &g_ps2_map[usage];
            if (km->scancode != 0)
                emit_key(km->scancode, km->extended,
                         g_hid_events.keys[i].make);
        } else {
            /* Modifier usages 0xE0-0xE7. */
            for (m = 0; m < sizeof(g_ps2_mods) / sizeof(g_ps2_mods[0]); m++) {
                mm = &g_ps2_mods[m];
                if (mm->usage == usage) {
                    emit_key(mm->scancode, mm->extended,
                             g_hid_events.keys[i].make);
                    break;
                }
            }
        }
    }

    /* Mouse: assemble the 3-byte packet [buttons, dx, dy] into the mouse
     * stream (separate from the keyboard stream). */
    if (g_hid_events.mouse_valid) {
        mouse_put(g_hid_events.mouse.buttons);
        mouse_put((UINT8)g_hid_events.mouse.dx);
        mouse_put((UINT8)g_hid_events.mouse.dy);
    }

    /* Consume the event queue. */
    g_hid_events.key_count   = 0;
    g_hid_events.mouse_valid = FALSE;
}
