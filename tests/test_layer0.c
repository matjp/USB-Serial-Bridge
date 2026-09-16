/*
 * test_layer0.c - Host unit tests for the Layer 0 bridge modules.
 *
 * Covers B2 (HID parser), B3 (HID -> PS/2 Set 1 translator), B4 (mailbox
 * writer), and O1 (mailbox reader), plus the mailbox ring buffer itself.
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
#include <mailbox.h>

#include "bridge.h"   /* B2, B3, B4 */
#include "adapter.h"  /* O1 */
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

/* O1 (mailbox_reader.c) defines the drained byte streams. The ADAPTER_STREAM
 * type is file-local there, so we mirror its layout here for the test. */
typedef struct {
    UINT8  bytes[MAILBOX_RING_SIZE];
    UINTN  count;
} ADAPTER_STREAM;
extern ADAPTER_STREAM g_adapter_kbd_stream;
extern ADAPTER_STREAM g_adapter_mouse_stream;

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
/* Test 1: mailbox ring buffer (producer/consumer, wrap-around)        */
/* ------------------------------------------------------------------ */

static void
test_mailbox_ring(void)
{
    MAILBOX mb;
    UINT8 byte;
    UINTN i;

    printf("\n[Test 1] Mailbox ring buffer\n");

    mailbox_init(&mb);
    CHECK(mb.head == 0 && mb.tail == 0, "init: head=tail=0");

    /* Empty read returns 0. */
    CHECK(mailbox_read(&mb, &byte) == 0, "empty read returns 0");

    /* Write/read round-trip. */
    mailbox_write(&mb, 0x1C);   /* 'A' make */
    mailbox_write(&mb, 0x9C);   /* 'A' break */
    CHECK(mailbox_read(&mb, &byte) == 1 && byte == 0x1C, "read back 0x1C");
    CHECK(mailbox_read(&mb, &byte) == 1 && byte == 0x9C, "read back 0x9C");
    CHECK(mailbox_read(&mb, &byte) == 0, "empty after drain");

    /* Wrap-around: write more than MAILBOX_RING_SIZE bytes. */
    mailbox_init(&mb);
    for (i = 0; i < MAILBOX_RING_SIZE + 10; i++)
        mailbox_write(&mb, (UINT8)(i & 0xFF));
    for (i = 0; i < MAILBOX_RING_SIZE + 10; i++) {
        CHECK(mailbox_read(&mb, &byte) == 1 &&
              byte == (UINT8)(i & 0xFF), "wrap-around byte order");
    }
    CHECK(mailbox_read(&mb, &byte) == 0, "empty after wrap-around drain");
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
/* Test 6: B4 mailbox writer + O1 mailbox reader (end-to-end)          */
/* ------------------------------------------------------------------ */

static void
test_b4_o1_roundtrip(void)
{
    MAILBOX kbd_mb;
    MAILBOX mouse_mb;
    UINTN i;

    printf("\n[Test 6] B4 mailbox writer + O1 mailbox reader (round-trip)\n");

    mailbox_init(&kbd_mb);
    mailbox_init(&mouse_mb);
    reset_bridge_state();

    /* Build a small PS/2 kbd stream: 'A' make (0x1C), 'A' break (0x9C). */
    g_ps2_kbd_stream.bytes[0] = 0x1C;
    g_ps2_kbd_stream.bytes[1] = 0x9C;
    g_ps2_kbd_stream.count    = 2;

    /* B4: write to the two mailboxes. */
    bridge_write_mailbox(&kbd_mb, &mouse_mb);
    CHECK(g_ps2_kbd_stream.count == 0, "B4 consumed the kbd stream");
    CHECK(kbd_mb.head == 2, "kbd mailbox head advanced to 2");
    CHECK(mouse_mb.head == 0, "mouse mailbox head unchanged");

    /* O1: drain the two mailboxes. */
    adapter_drain_mailbox(&kbd_mb, &mouse_mb);
    CHECK(g_adapter_kbd_stream.count == 2, "O1 drained 2 kbd bytes");
    if (g_adapter_kbd_stream.count == 2) {
        CHECK_EQ_U8(g_adapter_kbd_stream.bytes[0], 0x1C, "O1 kbd byte 0 = 0x1C");
        CHECK_EQ_U8(g_adapter_kbd_stream.bytes[1], 0x9C, "O1 kbd byte 1 = 0x9C");
    }
    CHECK(g_adapter_mouse_stream.count == 0, "O1 drained 0 mouse bytes");
    CHECK(kbd_mb.tail == kbd_mb.head, "O1 drained kbd fully (tail == head)");

    /* Full pipeline: B2 -> B3 -> B4 -> O1 for a key press. */
    mailbox_init(&kbd_mb);
    mailbox_init(&mouse_mb);
    reset_bridge_state();

    g_raw_kbd.key[0] = 0x04;   /* 'A' */
    g_kbd_valid = TRUE;
    bridge_parse_hid();        /* B2 */
    bridge_translate_ps2();    /* B3 */
    bridge_write_mailbox(&kbd_mb, &mouse_mb); /* B4 */
    adapter_drain_mailbox(&kbd_mb, &mouse_mb);/* O1 */

    CHECK(g_adapter_kbd_stream.count == 1, "pipeline: 1 kbd byte for 'A' make");
    if (g_adapter_kbd_stream.count == 1)
        CHECK_EQ_U8(g_adapter_kbd_stream.bytes[0], 0x1C, "pipeline: 'A' make 0x1C");
    CHECK(g_adapter_mouse_stream.count == 0, "pipeline: no mouse bytes for a key");

    /* Full pipeline for a mouse move. */
    mailbox_init(&kbd_mb);
    mailbox_init(&mouse_mb);
    reset_bridge_state();

    g_raw_mouse.buttons = 0x00;
    g_raw_mouse.dx      = 5;
    g_raw_mouse.dy      = 0;
    g_mouse_valid = TRUE;
    bridge_parse_hid();        /* B2 */
    bridge_translate_ps2();    /* B3 */
    bridge_write_mailbox(&kbd_mb, &mouse_mb); /* B4 */
    adapter_drain_mailbox(&kbd_mb, &mouse_mb);/* O1 */

    CHECK(g_adapter_mouse_stream.count == 3, "pipeline: 3-byte mouse packet");
    if (g_adapter_mouse_stream.count == 3) {
        CHECK_EQ_U8(g_adapter_mouse_stream.bytes[0], 0x00, "pipeline: mouse buttons 0");
        CHECK_EQ_U8(g_adapter_mouse_stream.bytes[1], 0x05, "pipeline: mouse dx 5");
        CHECK_EQ_U8(g_adapter_mouse_stream.bytes[2], 0x00, "pipeline: mouse dy 0");
    }
    CHECK(g_adapter_kbd_stream.count == 0, "pipeline: no kbd bytes for a mouse move");

    /* Unused loop var guard. */
    (void)i;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int
main(void)
{
    printf("Layer 0 host unit tests\n");
    printf("=======================\n");

    test_mailbox_ring();
    test_b2_keyboard();
    test_b2_modifiers();
    test_b2_mouse();
    test_b3_translate();
    test_b4_o1_roundtrip();

    printf("\n=======================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
