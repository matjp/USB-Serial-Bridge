/*
 * test_virtual_ps2.c - Host unit tests for the virtual 8042 port writer
 * (B4 virtual-port variant, src/bridge/virtual_ps2_writer.c).
 *
 * The writer produces bytes into the virtual 8042 port region
 * (include/virtual_ps2.h). The fixed addresses (0x10000030) are unmapped on
 * the host, so this test overrides the weak accessors with a mock register
 * file (same pattern as the XHCI mock in test_xhci_fault.c).
 *
 * Build & run:
 *   make -C tests run
 */

#include <stdio.h>
#include <string.h>

#include <efi.h>      /* resolves to tests/efi/efi.h (shim) */
#include <virtual_ps2.h>

#include "bridge.h"   /* B2, B3, B4 */
#include "hid_event.h"

/* Raw HID report globals owned by B1 (src/bridge/xhci.c) in the firmware
 * build; provided by b1_stub.c on the host. The pipeline tests drive them
 * directly to feed B2. */
extern HID_KBD_REPORT   g_raw_kbd;
extern HID_MOUSE_REPORT g_raw_mouse;
extern BOOLEAN g_kbd_valid;
extern BOOLEAN g_mouse_valid;

/* ------------------------------------------------------------------ */
/* Mock virtual 8042 register file (overrides the weak accessors).     */
/* ------------------------------------------------------------------ */

static UINT8 g_mock_status;
static UINT8 g_mock_data;
static int   g_write_count;   /* number of data writes performed */

/* Strong overrides of the weak accessors in virtual_ps2_writer.c. */
UINT8
virtual_ps2_read_status(void)
{
    return g_mock_status;
}

void
virtual_ps2_write_data(UINT8 byte)
{
    g_mock_data = byte;
    g_write_count++;
}

void
virtual_ps2_set_status(UINT8 bits)
{
    g_mock_status |= bits;
}

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
/* Test helpers                                                        */
/* ------------------------------------------------------------------ */

static void
reset_virtual_state(void)
{
    g_mock_status  = 0;
    g_mock_data    = 0;
    g_write_count  = 0;
    g_ps2_kbd_stream.count   = 0;
    g_ps2_mouse_stream.count = 0;
    bridge_virtual_ps2_reset();
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* One pending kbd byte: written to data, kbd status bit set, stream consumed. */
static void
test_single_kbd_byte(void)
{
    printf("\n[Test 1] single kbd byte -> data + kbd status\n");

    reset_virtual_state();
    g_ps2_kbd_stream.bytes[0] = 0x1C;   /* 'A' make */
    g_ps2_kbd_stream.count    = 1;

    bridge_write_virtual_ps2();

    CHECK_EQ_U8(g_mock_data, 0x1C, "data register holds 0x1C");
    CHECK_EQ_U8(g_mock_status, VIRTUAL_PS2_STAT_KBD, "kbd status bit set");
    CHECK(g_write_count == 1, "exactly one data write");
    CHECK(g_ps2_kbd_stream.count == 0, "kbd stream consumed");
}

/* One pending mouse byte: written to data, mouse status bit set. */
static void
test_single_mouse_byte(void)
{
    printf("\n[Test 2] single mouse byte -> data + mouse status\n");

    reset_virtual_state();
    g_ps2_mouse_stream.bytes[0] = 0x01;   /* mouse buttons */
    g_ps2_mouse_stream.count    = 1;

    bridge_write_virtual_ps2();

    CHECK_EQ_U8(g_mock_data, 0x01, "data register holds 0x01");
    CHECK_EQ_U8(g_mock_status, VIRTUAL_PS2_STAT_MOUSE, "mouse status bit set");
    CHECK(g_write_count == 1, "exactly one data write");
    CHECK(g_ps2_mouse_stream.count == 0, "mouse stream consumed");
}

/* Keyboard is prioritized over mouse when both have pending bytes. */
static void
test_kbd_priority(void)
{
    printf("\n[Test 3] kbd prioritized over mouse\n");

    reset_virtual_state();
    g_ps2_kbd_stream.bytes[0]   = 0x1C;
    g_ps2_kbd_stream.count      = 1;
    g_ps2_mouse_stream.bytes[0] = 0x01;
    g_ps2_mouse_stream.count    = 1;

    bridge_write_virtual_ps2();

    CHECK_EQ_U8(g_mock_data, 0x1C, "kbd byte written first");
    CHECK_EQ_U8(g_mock_status, VIRTUAL_PS2_STAT_KBD, "kbd status bit set");
    CHECK(g_write_count == 1, "only one byte written (single output buffer)");
}

/* If a byte is still pending (status bit set), do NOT write another. */
static void
test_blocked_when_pending(void)
{
    printf("\n[Test 4] no write while a byte is pending\n");

    reset_virtual_state();
    g_mock_status = VIRTUAL_PS2_STAT_KBD;   /* OS has not consumed yet */
    g_ps2_kbd_stream.bytes[0] = 0x1C;
    g_ps2_kbd_stream.count    = 1;

    bridge_write_virtual_ps2();

    CHECK(g_write_count == 0, "no data write while pending");
    CHECK_EQ_U8(g_mock_status, VIRTUAL_PS2_STAT_KBD, "status unchanged");
    CHECK(g_ps2_kbd_stream.count == 0, "stream still consumed (regenerated each poll)");
}

/* After the OS consumes (clears status), the next byte is written. */
static void
test_consumption_handshake(void)
{
    printf("\n[Test 5] consumption handshake: write, consume, write next\n");

    reset_virtual_state();
    g_ps2_kbd_stream.bytes[0] = 0x1C;   /* 'A' make */
    g_ps2_kbd_stream.bytes[1] = 0x9C;   /* 'A' break */
    g_ps2_kbd_stream.count    = 2;

    /* First poll: write byte 0, set kbd status. */
    bridge_write_virtual_ps2();
    CHECK_EQ_U8(g_mock_data, 0x1C, "poll 1 writes 0x1C");
    CHECK_EQ_U8(g_mock_status, VIRTUAL_PS2_STAT_KBD, "poll 1 sets kbd status");

    /* OS consumes: reads data, clears status (read-and-clear). */
    g_mock_status = 0;

    /* Second poll: B3 regenerates the same packet; the cursor advances to
     * byte 1. */
    g_ps2_kbd_stream.bytes[0] = 0x1C;
    g_ps2_kbd_stream.bytes[1] = 0x9C;
    g_ps2_kbd_stream.count    = 2;
    bridge_write_virtual_ps2();
    CHECK_EQ_U8(g_mock_data, 0x9C, "poll 2 writes 0x9C");
    CHECK_EQ_U8(g_mock_status, VIRTUAL_PS2_STAT_KBD, "poll 2 sets kbd status");
    CHECK(g_write_count == 2, "two data writes total");
}

/* Full pipeline: B2 -> B3 -> B4(virtual) for a key press. */
static void
test_pipeline_key(void)
{
    printf("\n[Test 6] pipeline B2 -> B3 -> B4(virtual) for a key\n");

    reset_virtual_state();
    g_raw_kbd.key[0] = 0x04;   /* 'A' */
    g_kbd_valid = TRUE;

    bridge_parse_hid();        /* B2 */
    bridge_translate_ps2();    /* B3 */
    bridge_write_virtual_ps2();/* B4 virtual */

    CHECK_EQ_U8(g_mock_data, 0x1C, "pipeline: 'A' make 0x1C");
    CHECK_EQ_U8(g_mock_status, VIRTUAL_PS2_STAT_KBD, "pipeline: kbd status set");
    CHECK(g_ps2_kbd_stream.count == 0, "pipeline: kbd stream consumed");
}

/* Full pipeline for a mouse move: 3-byte packet, one byte per poll. In the
 * real poll loop B2/B3 regenerate the stream each iteration, so the packet
 * bytes are re-produced each poll; the writer emits one byte per poll. */
static void
test_pipeline_mouse(void)
{
    printf("\n[Test 7] pipeline B2 -> B3 -> B4(virtual) for a mouse move\n");

    reset_virtual_state();
    g_raw_mouse.buttons = 0x00;
    g_raw_mouse.dx      = 5;
    g_raw_mouse.dy      = 0;
    g_mouse_valid = TRUE;

    /* Poll 1: byte 0 (buttons). A fresh USB report arrives each poll. */
    g_mouse_valid = TRUE;
    bridge_parse_hid();        /* B2 */
    bridge_translate_ps2();    /* B3 */
    bridge_write_virtual_ps2();/* B4 virtual */
    CHECK_EQ_U8(g_mock_data, 0x00, "mouse byte 0 = buttons 0x00");
    CHECK_EQ_U8(g_mock_status, VIRTUAL_PS2_STAT_MOUSE, "mouse status set");
    g_mock_status = 0;   /* OS consumes */

    /* Poll 2: byte 1 (dx). */
    g_mouse_valid = TRUE;
    bridge_parse_hid();
    bridge_translate_ps2();
    bridge_write_virtual_ps2();
    CHECK_EQ_U8(g_mock_data, 0x05, "mouse byte 1 = dx 0x05");
    g_mock_status = 0;

    /* Poll 3: byte 2 (dy). */
    g_mouse_valid = TRUE;
    bridge_parse_hid();
    bridge_translate_ps2();
    bridge_write_virtual_ps2();
    CHECK_EQ_U8(g_mock_data, 0x00, "mouse byte 2 = dy 0x00");
    CHECK(g_write_count == 3, "three mouse bytes written");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int
main(void)
{
    printf("test_virtual_ps2: virtual 8042 port writer (B4 virtual variant)\n");

    test_single_kbd_byte();
    test_single_mouse_byte();
    test_kbd_priority();
    test_blocked_when_pending();
    test_consumption_handshake();
    test_pipeline_key();
    test_pipeline_mouse();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
