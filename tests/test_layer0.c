/*
 * test_layer0.c - Host unit tests for the Layer 0 bridge modules.
 *
 * Covers B2 (HID parser), B3 (HID -> PS/2 Set 1 translator), and B4 (virtual
 * 8042 port writer).
 *
 * These modules are pure C and hardware-independent, so host tests are
 * authoritative (see docs/architecture.md section 9, Layer 0).
 *
 * Build & run:
 *   make -C tests run
 */

#include <stdio.h>
#include <string.h>

#include <efi.h>      /* resolves to tests/efi/efi.h (shim) */
#include <hid.h>
#include <ps2.h>

#include "bridge.h"   /* B2, B3, B4 */
#include "hid_event.h"

/* ------------------------------------------------------------------ */
/* Module internals the tests reach into.                              */
/*                                                                     */
/* B2 (hid_parser.c) defines these TEMP globals until B1 (xhci.c)      */
/* lands; the tests drive them directly to feed raw HID reports.       */
/* ------------------------------------------------------------------ */
extern HID_KBD_REPORT   g_raw_kbd;
extern HID_MOUSE_REPORT g_raw_mouse;
extern BOOLEAN g_kbd_valid;
extern BOOLEAN g_mouse_valid;

/* ------------------------------------------------------------------ */
/* Minimal test framework                                              */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do {                                          \
    if (cond) {                                                        \
        g_pass++;                                                      \
        printf("  PASS: %s\n", msg);                                   \
    } else {                                                           \
        g_fail++;                                                      \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__);               \
    }                                                                  \
} while (0)

#define CHECK_EQ_U8(actual, expected, msg) do {                        \
    if ((actual) == (expected)) {                                      \
        g_pass++;                                                      \
        printf("  PASS: %s (0x%02X)\n", msg, (unsigned)(actual));      \
    } else {                                                           \
        g_fail++;                                                      \
        printf("  FAIL: %s got 0x%02X want 0x%02X (line %d)\n",        \
               msg, (unsigned)(actual), (unsigned)(expected), __LINE__);\
    }                                                                  \
} while (0)

/* ------------------------------------------------------------------ */
/* Test helpers: reset module state between tests                      */
/* ------------------------------------------------------------------ */

/* Reset the B2/B3/B4 internal state (globals defined in the modules). */
static void
reset_bridge_state(void)
{
    g_kbd_valid   = FALSE;
    g_mouse_valid = FALSE;
    memset(&g_raw_kbd, 0, sizeof(g_raw_kbd));
    memset(&g_raw_mouse, 0, sizeof(g_raw_mouse));
    g_hid_events.key_count   = 0;
    g_hid_events.mouse_valid = FALSE;
    g_ps2_kbd_stream.count   = 0;
    g_ps2_mouse_stream.count = 0;
}

/* ------------------------------------------------------------------ */
/* Test 2: B2 HID parser - keyboard make/break                         */
/* ------------------------------------------------------------------ */

static void
test_b2_keyboard(void)
{
    printf("\n[Test 2] B2 HID parser - keyboard make/break\n");

    reset_bridge_state();

    /* Press 'A' (usage 0x04). */
    g_raw_kbd.key[0] = 0x04;
    g_kbd_valid = TRUE;
    g_hid_events.key_count = 0;   /* count only this parse's events */
    bridge_parse_hid();
    CHECK(g_hid_events.key_count == 1, "one make event for 'A'");
    if (g_hid_events.key_count == 1) {
        CHECK(g_hid_events.keys[0].usage == 0x04, "'A' usage 0x04");
        CHECK(g_hid_events.keys[0].make == TRUE, "'A' is a make");
    }

    /* Same report again: no new events (no transition). */
    g_kbd_valid = TRUE;
    g_hid_events.key_count = 0;
    bridge_parse_hid();
    CHECK(g_hid_events.key_count == 0, "no event on unchanged report");

    /* Release 'A'. */
    g_raw_kbd.key[0] = 0x00;
    g_kbd_valid = TRUE;
    g_hid_events.key_count = 0;
    bridge_parse_hid();
    CHECK(g_hid_events.key_count == 1, "one break event for 'A'");
    if (g_hid_events.key_count == 1) {
        CHECK(g_hid_events.keys[0].usage == 0x04, "'A' break usage 0x04");
        CHECK(g_hid_events.keys[0].make == FALSE, "'A' is a break");
    }
}

/* ------------------------------------------------------------------ */
/* Test 3: B2 HID parser - modifier keys                               */
/* ------------------------------------------------------------------ */

static void
test_b2_modifiers(void)
{
    printf("\n[Test 3] B2 HID parser - modifier keys\n");

    reset_bridge_state();

    /* Press LShift (modifier bit1). */
    g_raw_kbd.modifier = 0x02;   /* bit1 = LShift */
    g_kbd_valid = TRUE;
    g_hid_events.key_count = 0;
    bridge_parse_hid();
    CHECK(g_hid_events.key_count == 1, "one event for LShift");
    if (g_hid_events.key_count == 1) {
        CHECK(g_hid_events.keys[0].usage == 0xE1, "LShift usage 0xE1");
        CHECK(g_hid_events.keys[0].make == TRUE, "LShift is a make");
    }

    /* Release LShift. */
    g_raw_kbd.modifier = 0x00;
    g_kbd_valid = TRUE;
    g_hid_events.key_count = 0;
    bridge_parse_hid();
    CHECK(g_hid_events.key_count == 1, "one event for LShift release");
    if (g_hid_events.key_count == 1) {
        CHECK(g_hid_events.keys[0].usage == 0xE1, "LShift break usage 0xE1");
        CHECK(g_hid_events.keys[0].make == FALSE, "LShift is a break");
    }
}

/* ------------------------------------------------------------------ */
/* Test 4: B2 HID parser - mouse                                       */
/* ------------------------------------------------------------------ */

static void
test_b2_mouse(void)
{
    printf("\n[Test 4] B2 HID parser - mouse\n");

    reset_bridge_state();

    g_raw_mouse.buttons = 0x01;   /* left button */
    g_raw_mouse.dx      = 3;
    g_raw_mouse.dy      = -2;
    g_mouse_valid = TRUE;
    bridge_parse_hid();
    CHECK(g_hid_events.mouse_valid == TRUE, "mouse event produced");
    CHECK(g_hid_events.mouse.buttons == 0x01, "mouse buttons 0x01");
    CHECK(g_hid_events.mouse.dx == 3, "mouse dx 3");
    CHECK(g_hid_events.mouse.dy == -2, "mouse dy -2");
}

/* ------------------------------------------------------------------ */
/* Test 5: B3 HID -> PS/2 Set 1 translator                             */
/* ------------------------------------------------------------------ */

static void
test_b3_translate(void)
{
    printf("\n[Test 5] B3 HID -> PS/2 Set 1 translator\n");

    reset_bridge_state();

    /* Feed a make for 'A' (usage 0x04) directly into the event queue. */
    g_hid_events.keys[0].usage = 0x04;
    g_hid_events.keys[0].make  = TRUE;
    g_hid_events.key_count     = 1;
    bridge_translate_ps2();
    CHECK(g_ps2_kbd_stream.count == 1, "'A' make emits 1 kbd byte");
    if (g_ps2_kbd_stream.count == 1)
        CHECK_EQ_U8(g_ps2_kbd_stream.bytes[0], 0x1C, "'A' make scancode 0x1C");
    CHECK(g_ps2_mouse_stream.count == 0, "no mouse bytes for a key");

    /* Break for 'A'. */
    reset_bridge_state();
    g_hid_events.keys[0].usage = 0x04;
    g_hid_events.keys[0].make  = FALSE;
    g_hid_events.key_count     = 1;
    bridge_translate_ps2();
    CHECK(g_ps2_kbd_stream.count == 1, "'A' break emits 1 kbd byte");
    if (g_ps2_kbd_stream.count == 1)
        CHECK_EQ_U8(g_ps2_kbd_stream.bytes[0], 0x9C, "'A' break scancode 0x9C");

    /* Extended key: Right arrow (usage 0x4F) make = 0xE0 0x74. */
    reset_bridge_state();
    g_hid_events.keys[0].usage = 0x4F;
    g_hid_events.keys[0].make  = TRUE;
    g_hid_events.key_count     = 1;
    bridge_translate_ps2();
    CHECK(g_ps2_kbd_stream.count == 2, "Right arrow make emits 2 kbd bytes");
    if (g_ps2_kbd_stream.count == 2) {
        CHECK_EQ_U8(g_ps2_kbd_stream.bytes[0], PS2_EXT_PREFIX, "Right: 0xE0 prefix");
        CHECK_EQ_U8(g_ps2_kbd_stream.bytes[1], 0x74, "Right: scancode 0x74");
    }

    /* Extended key break: Right arrow break = 0xE0 0xF4. */
    reset_bridge_state();
    g_hid_events.keys[0].usage = 0x4F;
    g_hid_events.keys[0].make  = FALSE;
    g_hid_events.key_count     = 1;
    bridge_translate_ps2();
    CHECK(g_ps2_kbd_stream.count == 2, "Right arrow break emits 2 kbd bytes");
    if (g_ps2_kbd_stream.count == 2) {
        CHECK_EQ_U8(g_ps2_kbd_stream.bytes[0], PS2_EXT_PREFIX, "Right break: 0xE0");
        CHECK_EQ_U8(g_ps2_kbd_stream.bytes[1], 0xF4, "Right break: 0xF4");
    }

    /* Mouse packet: [buttons, dx, dy] goes to the mouse stream. */
    reset_bridge_state();
    g_hid_events.mouse.buttons = 0x01;
    g_hid_events.mouse.dx      = 3;
    g_hid_events.mouse.dy      = -2;
    g_hid_events.mouse_valid   = TRUE;
    bridge_translate_ps2();
    CHECK(g_ps2_mouse_stream.count == 3, "mouse emits 3-byte packet");
    if (g_ps2_mouse_stream.count == 3) {
        CHECK_EQ_U8(g_ps2_mouse_stream.bytes[0], 0x01, "mouse buttons 0x01");
        CHECK_EQ_U8(g_ps2_mouse_stream.bytes[1], 0x03, "mouse dx 3");
        CHECK_EQ_U8(g_ps2_mouse_stream.bytes[2], 0xFE, "mouse dy -2 (0xFE)");
    }
    CHECK(g_ps2_kbd_stream.count == 0, "no kbd bytes for a mouse move");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int
main(void)
{
    printf("Layer 0 host unit tests\n");
    printf("=======================\n");

    test_b2_keyboard();
    test_b2_modifiers();
    test_b2_mouse();
    test_b3_translate();

    printf("\n=======================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
