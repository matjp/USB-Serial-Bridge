/*
 * test_layer2_reader.c - Host unit tests for the O1 virtual port reader
 * (src/adapter/virtual_ps2_reader.c), the Layer 2 stub harness.
 *
 * O1 is the consumer side of the "virtual IRQ + virtual ports" design: the
 * bridge (producer) writes a byte, sets the status bit, then sends a
 * virtual IRQ; the OS's ISR-driven reader reads the virtual status/data
 * registers (read-and-clear), feeds the byte to the OS's existing KBD/mouse
 * handler, and EOIs the local APIC.
 *
 * This test validates O1's read-and-clear behavior WITHOUT the real OS,
 * using a stub that mimics the OS's PS/2 driver (a fake PutKey/KBD buffer).
 * The fixed addresses (0x10000030) are unmapped on the host, so this test
 * overrides the weak accessors with a mock register file (same pattern as
 * test_virtual_ps2.c), and overrides the weak feed hooks to capture bytes
 * into fake KBD/mouse buffers.
 *
 * Build & run:
 *   make -C tests run
 */

#include <stdio.h>
#include <string.h>

#include <efi.h>      /* resolves to tests/efi/efi.h (shim) */
#include <virtual_ps2.h>

#include "adapter.h"  /* O1: adapter_read_virtual_ps2, adapter_apic_eoi */

/* ------------------------------------------------------------------ */
/* Mock virtual 8042 register file (overrides the weak accessors).     */
/* ------------------------------------------------------------------ */

static UINT8 g_mock_status;
static UINT8 g_mock_data;
static int   g_write_count;   /* number of data writes performed */
static int   g_irq_count;     /* number of virtual IRQs sent */
static UINT8 g_last_irq;      /* vector of the last virtual IRQ sent */

/* Strong overrides of the weak accessors in virtual_ps2_reader.c /
 * virtual_ps2_writer.c. The O1 reader calls read_status/read_data/
 * clear_status; the mock lets the test set up a pending byte and observe
 * the clear. */
UINT8
virtual_ps2_read_status(void)
{
    return g_mock_status;
}

UINT8
virtual_ps2_read_data(void)
{
    return g_mock_data;
}

void
virtual_ps2_clear_status(UINT8 bits)
{
    g_mock_status &= (UINT8)~bits;
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

void
virtual_ps2_send_irq(UINT8 vector)
{
    g_irq_count++;
    g_last_irq = vector;
}

/* ------------------------------------------------------------------ */
/* Fake OS PS/2 driver buffers (override the weak feed hooks).         */
/* ------------------------------------------------------------------ */

#define FAKE_BUF_MAX 16

static UINT8 g_fake_kbd[FAKE_BUF_MAX];
static int   g_fake_kbd_count;
static UINT8 g_fake_mouse[FAKE_BUF_MAX];
static int   g_fake_mouse_count;

/* Strong overrides of the weak feed hooks in virtual_ps2_reader.c. */
void
adapter_feed_kbd_byte(UINT8 byte)
{
    if (g_fake_kbd_count < FAKE_BUF_MAX)
        g_fake_kbd[g_fake_kbd_count++] = byte;
}

void
adapter_feed_mouse_byte(UINT8 byte)
{
    if (g_fake_mouse_count < FAKE_BUF_MAX)
        g_fake_mouse[g_fake_mouse_count++] = byte;
}

/* ------------------------------------------------------------------ */
/* Mock APIC EOI write (override the weak EOI-write hook).             */
/* ------------------------------------------------------------------ */

static int     g_eoi_write_count;   /* number of EOI writes performed */
static UINT32  g_eoi_write_value;   /* value of the last EOI write */

void
adapter_apic_eoi_write(UINT32 value)
{
    g_eoi_write_count++;
    g_eoi_write_value = value;
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
reset_state(void)
{
    g_mock_status      = 0;
    g_mock_data        = 0;
    g_write_count      = 0;
    g_irq_count        = 0;
    g_last_irq         = 0;
    g_fake_kbd_count   = 0;
    g_fake_mouse_count = 0;
    g_eoi_write_count  = 0;
    g_eoi_write_value  = 0;
    memset(g_fake_kbd,   0, sizeof(g_fake_kbd));
    memset(g_fake_mouse, 0, sizeof(g_fake_mouse));
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* No status bit set -> reader returns without feeding any byte and
 * without clearing anything. */
static void
test_no_pending_byte(void)
{
    printf("\n[Test 1] no status bit -> no feed, no clear\n");

    reset_state();
    g_mock_status = 0;   /* nothing pending */

    adapter_read_virtual_ps2();

    CHECK(g_fake_kbd_count == 0, "no kbd byte fed");
    CHECK(g_fake_mouse_count == 0, "no mouse byte fed");
    CHECK_EQ_U8(g_mock_status, 0, "status unchanged (nothing cleared)");
}

/* KBD status bit set with a data byte -> reader feeds the byte to the
 * kbd hook, clears the KBD status bit, leaves mouse bit untouched. */
static void
test_kbd_byte(void)
{
    printf("\n[Test 2] kbd byte -> fed to kbd hook, kbd bit cleared\n");

    reset_state();
    g_mock_status = VIRTUAL_PS2_STAT_KBD;
    g_mock_data   = 0x1C;   /* 'A' make */

    adapter_read_virtual_ps2();

    CHECK(g_fake_kbd_count == 1, "one kbd byte fed");
    CHECK_EQ_U8(g_fake_kbd[0], 0x1C, "kbd byte is 0x1C");
    CHECK(g_fake_mouse_count == 0, "no mouse byte fed");
    CHECK_EQ_U8(g_mock_status, 0, "kbd status bit cleared");
}

/* MOUSE status bit set with a data byte -> reader feeds the byte to the
 * mouse hook, clears the MOUSE status bit. */
static void
test_mouse_byte(void)
{
    printf("\n[Test 3] mouse byte -> fed to mouse hook, mouse bit cleared\n");

    reset_state();
    g_mock_status = VIRTUAL_PS2_STAT_MOUSE;
    g_mock_data   = 0x01;   /* mouse buttons */

    adapter_read_virtual_ps2();

    CHECK(g_fake_mouse_count == 1, "one mouse byte fed");
    CHECK_EQ_U8(g_fake_mouse[0], 0x01, "mouse byte is 0x01");
    CHECK(g_fake_kbd_count == 0, "no kbd byte fed");
    CHECK_EQ_U8(g_mock_status, 0, "mouse status bit cleared");
}

/* Both KBD and MOUSE bits set -> keyboard prioritized: feeds kbd byte,
 * clears only KBD bit (mouse byte still pending). */
static void
test_kbd_priority(void)
{
    printf("\n[Test 4] both bits set -> kbd prioritized, mouse still pending\n");

    reset_state();
    g_mock_status = VIRTUAL_PS2_STAT_KBD | VIRTUAL_PS2_STAT_MOUSE;
    g_mock_data   = 0x1C;   /* single data register holds the kbd byte */

    adapter_read_virtual_ps2();

    CHECK(g_fake_kbd_count == 1, "kbd byte fed (prioritized)");
    CHECK_EQ_U8(g_fake_kbd[0], 0x1C, "kbd byte is 0x1C");
    CHECK(g_fake_mouse_count == 0, "mouse byte NOT fed (still pending)");
    CHECK_EQ_U8(g_mock_status, VIRTUAL_PS2_STAT_MOUSE,
                "only kbd bit cleared; mouse bit still set");
}

/* Read-and-clear semantics: after the reader runs, the status bit is
 * cleared (a pure load would NOT have cleared it). */
static void
test_read_and_clear(void)
{
    printf("\n[Test 5] read-and-clear: status bit cleared after read\n");

    reset_state();
    g_mock_status = VIRTUAL_PS2_STAT_KBD;
    g_mock_data   = 0x9C;   /* 'A' break */

    /* A pure load would leave the status bit set; the reader must clear it. */
    adapter_read_virtual_ps2();

    CHECK_EQ_U8(g_mock_status, 0, "status bit cleared (read-and-clear)");
    CHECK(g_fake_kbd_count == 1, "kbd byte fed");
    CHECK_EQ_U8(g_fake_kbd[0], 0x9C, "kbd byte is 0x9C");
}

/* adapter_apic_eoi() writes 0 to the LAPIC EOI (via the weak EOI-write
 * hook, since 0xFEE000B0 is unmapped on the host). */
static void
test_apic_eoi(void)
{
    printf("\n[Test 6] adapter_apic_eoi writes 0 to LAPIC EOI\n");

    reset_state();

    adapter_apic_eoi();

    CHECK(g_eoi_write_count == 1, "EOI write performed");
    CHECK_EQ_U8((UINT8)g_eoi_write_value, 0, "EOI write value is 0");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int
main(void)
{
    printf("test_layer2_reader: O1 virtual port reader (Layer 2 stub harness)\n");

    test_no_pending_byte();
    test_kbd_byte();
    test_mouse_byte();
    test_kbd_priority();
    test_read_and_clear();
    test_apic_eoi();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
