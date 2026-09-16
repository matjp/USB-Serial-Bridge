/*
 * test_layer2.c - Host unit tests for the Layer 2 input adapter (O2).
 *
 * Validates O2's drain-and-inject behavior against a STUB input queue that
 * mimics the OS's input structures (a fake PutKey/KBD buffer + a fake mouse
 * buffer). This isolates O2 from the rest of the system, so any OS-bring-up
 * issue is not confused with an adapter bug (see docs/architecture.md
 * section 9, Layer 2).
 *
 * The OS-independent parsing core of O2 (input_inject.c) is compiled for
 * real. Its OS-specific injection hook is overridden here with stub
 * injectors that capture the semantic events O2 would feed to the OS.
 *
 * Build & run:
 *   make -C tests run
 */

#include <stdio.h>
#include <string.h>

#include <efi.h>      /* resolves to tests/efi/efi.h (shim) */
#include <ps2.h>
#include <mailbox.h>

#include "adapter.h"  /* O1, O2 */

/* ------------------------------------------------------------------ */
/* Module internals the tests reach into.                              */
/* ------------------------------------------------------------------ */

/* O1 (mailbox_reader.c) defines the drained byte streams. ADAPTER_STREAM is
 * file-local there; we mirror its layout here (same as test_layer0.c). */
typedef struct {
    UINT8  bytes[MAILBOX_RING_SIZE];
    UINTN  count;
} ADAPTER_STREAM;
extern ADAPTER_STREAM g_adapter_kbd_stream;
extern ADAPTER_STREAM g_adapter_mouse_stream;

/* O2 (input_inject.c) exposes its swappable injection hooks and a state
 * reset. We override the hooks with stub injectors and call o2_reset_state()
 * between cases. */
typedef void (*O2_KEY_INJECTOR)(BOOLEAN make, UINT8 scancode, BOOLEAN extended);
typedef void (*O2_MOUSE_INJECTOR)(UINT8 buttons, INT8 dx, INT8 dy);
extern O2_KEY_INJECTOR   g_o2_key_injector;
extern O2_MOUSE_INJECTOR g_o2_mouse_injector;
extern void o2_reset_state(void);

/* ------------------------------------------------------------------ */
/* Stub input queue (mimics the OS's input structures).                */
/* ------------------------------------------------------------------ */

#define STUB_KBD_MAX 64
typedef struct {
    BOOLEAN make;
    UINT8   scancode;
    BOOLEAN extended;
} STUB_KEY_EVENT;

static STUB_KEY_EVENT g_stub_kbd[STUB_KBD_MAX];
static UINTN g_stub_kbd_count;

#define STUB_MOUSE_MAX 64
typedef struct {
    UINT8 buttons;
    INT8  dx;
    INT8  dy;
} STUB_MOUSE_EVENT;

static STUB_MOUSE_EVENT g_stub_mouse[STUB_MOUSE_MAX];
static UINTN g_stub_mouse_count;

/* Stub injectors: capture what O2 would feed to the OS. */
static void
stub_key_injector(BOOLEAN make, UINT8 scancode, BOOLEAN extended)
{
    if (g_stub_kbd_count < STUB_KBD_MAX) {
        g_stub_kbd[g_stub_kbd_count].make     = make;
        g_stub_kbd[g_stub_kbd_count].scancode = scancode;
        g_stub_kbd[g_stub_kbd_count].extended = extended;
        g_stub_kbd_count++;
    }
}

static void
stub_mouse_injector(UINT8 buttons, INT8 dx, INT8 dy)
{
    if (g_stub_mouse_count < STUB_MOUSE_MAX) {
        g_stub_mouse[g_stub_mouse_count].buttons = buttons;
        g_stub_mouse[g_stub_mouse_count].dx      = dx;
        g_stub_mouse[g_stub_mouse_count].dy      = dy;
        g_stub_mouse_count++;
    }
}

/* ------------------------------------------------------------------ */
/* Minimal test framework (same pattern as test_layer0.c).             */
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

/* ------------------------------------------------------------------ */
/* Test helpers                                                        */
/* ------------------------------------------------------------------ */

/* Reset the stub queues, O2 parser state, and both drained streams. */
static void
reset_all(void)
{
    g_stub_kbd_count   = 0;
    g_stub_mouse_count = 0;
    g_adapter_kbd_stream.count   = 0;
    g_adapter_mouse_stream.count = 0;
    o2_reset_state();
}

/* Feed a byte stream into g_adapter_kbd_stream (simulating what O1 would
 * produce after draining the keyboard mailbox) and run O2. The mouse stream
 * is cleared so only the keyboard bytes are injected. */
static void
feed_kbd_and_inject(const UINT8 *bytes, UINTN count)
{
    UINTN i;
    g_adapter_mouse_stream.count = 0;
    g_adapter_kbd_stream.count = count;
    for (i = 0; i < count; i++)
        g_adapter_kbd_stream.bytes[i] = bytes[i];
    adapter_inject_input();
}

/* Feed a byte stream into g_adapter_mouse_stream (simulating what O1 would
 * produce after draining the mouse mailbox) and run O2. The keyboard stream
 * is cleared so only the mouse bytes are injected. */
static void
feed_mouse_and_inject(const UINT8 *bytes, UINTN count)
{
    UINTN i;
    g_adapter_kbd_stream.count = 0;
    g_adapter_mouse_stream.count = count;
    for (i = 0; i < count; i++)
        g_adapter_mouse_stream.bytes[i] = bytes[i];
    adapter_inject_input();
}

/* ------------------------------------------------------------------ */
/* Test 1: simple key make + break ('A' = 0x1C make, 0x9C break)       */
/* ------------------------------------------------------------------ */

static void
test_simple_key(void)
{
    static const UINT8 stream[] = { 0x1C, 0x9C };

    printf("\n[Test 1] Simple key make + break ('A')\n");

    reset_all();
    feed_kbd_and_inject(stream, sizeof(stream));

    CHECK(g_stub_kbd_count == 2, "two keyboard events");
    if (g_stub_kbd_count >= 1) {
        CHECK(g_stub_kbd[0].make == TRUE, "'A' make");
        CHECK(g_stub_kbd[0].scancode == 0x1C, "'A' make scancode 0x1C");
        CHECK(g_stub_kbd[0].extended == FALSE, "'A' make not extended");
    }
    if (g_stub_kbd_count >= 2) {
        CHECK(g_stub_kbd[1].make == FALSE, "'A' break");
        CHECK(g_stub_kbd[1].scancode == 0x1C, "'A' break scancode 0x1C");
        CHECK(g_stub_kbd[1].extended == FALSE, "'A' break not extended");
    }
    CHECK(g_stub_mouse_count == 0, "no mouse events");
}

/* ------------------------------------------------------------------ */
/* Test 2: extended key (Right arrow = 0xE0 0x74 make, 0xE0 0xF4 break)*/
/* ------------------------------------------------------------------ */

static void
test_extended_key(void)
{
    static const UINT8 stream[] = { 0xE0, 0x74, 0xE0, 0xF4 };

    printf("\n[Test 2] Extended key (Right arrow)\n");

    reset_all();
    feed_kbd_and_inject(stream, sizeof(stream));

    CHECK(g_stub_kbd_count == 2, "two keyboard events");
    if (g_stub_kbd_count >= 1) {
        CHECK(g_stub_kbd[0].make == TRUE, "Right make");
        CHECK(g_stub_kbd[0].scancode == 0x74, "Right make scancode 0x74");
        CHECK(g_stub_kbd[0].extended == TRUE, "Right make extended");
    }
    if (g_stub_kbd_count >= 2) {
        CHECK(g_stub_kbd[1].make == FALSE, "Right break");
        CHECK(g_stub_kbd[1].scancode == 0x74, "Right break scancode 0x74");
        CHECK(g_stub_kbd[1].extended == TRUE, "Right break extended");
    }
    CHECK(g_stub_mouse_count == 0, "no mouse events");
}

/* ------------------------------------------------------------------ */
/* Test 3: modifier key (LShift = 0x2A make, 0xAA break)               */
/* ------------------------------------------------------------------ */

static void
test_modifier_key(void)
{
    static const UINT8 stream[] = { 0x2A, 0xAA };

    printf("\n[Test 3] Modifier key (LShift)\n");

    reset_all();
    feed_kbd_and_inject(stream, sizeof(stream));

    CHECK(g_stub_kbd_count == 2, "two keyboard events");
    if (g_stub_kbd_count >= 1) {
        CHECK(g_stub_kbd[0].make == TRUE, "LShift make");
        CHECK(g_stub_kbd[0].scancode == 0x2A, "LShift make scancode 0x2A");
        CHECK(g_stub_kbd[0].extended == FALSE, "LShift make not extended");
    }
    if (g_stub_kbd_count >= 2) {
        CHECK(g_stub_kbd[1].make == FALSE, "LShift break");
        CHECK(g_stub_kbd[1].scancode == 0x2A, "LShift break scancode 0x2A");
        CHECK(g_stub_kbd[1].extended == FALSE, "LShift break not extended");
    }
    CHECK(g_stub_mouse_count == 0, "no mouse events");
}

/* ------------------------------------------------------------------ */
/* Test 4: mouse packet ([0x01, 0x05, 0x00] = left, dx=+5, dy=0)       */
/* ------------------------------------------------------------------ */

static void
test_mouse_packet(void)
{
    static const UINT8 stream[] = { 0x01, 0x05, 0x00 };

    printf("\n[Test 4] Mouse packet (left, dx=+5, dy=0)\n");

    reset_all();
    feed_mouse_and_inject(stream, sizeof(stream));

    CHECK(g_stub_mouse_count == 1, "one mouse packet");
    if (g_stub_mouse_count >= 1) {
        CHECK(g_stub_mouse[0].buttons == 0x01, "mouse buttons 0x01 (left)");
        CHECK(g_stub_mouse[0].dx == 5, "mouse dx +5");
        CHECK(g_stub_mouse[0].dy == 0, "mouse dy 0");
    }
    CHECK(g_stub_kbd_count == 0, "no keyboard events");
}

/* ------------------------------------------------------------------ */
/* Test 5: multi-event stream mixing keyboard and mouse                */
/* ------------------------------------------------------------------ */

static void
test_mixed_stream(void)
{
    /* Keyboard: 'A' make+break, Right arrow make+break. */
    static const UINT8 kbd_stream[] = {
        0x1C, 0x9C,          /* 'A' make, break */
        0xE0, 0x74, 0xE0, 0xF4  /* Right arrow make, break */
    };
    /* Mouse: one packet. */
    static const UINT8 mouse_stream[] = {
        0x01, 0x05, 0x00     /* mouse: left, dx=+5, dy=0 */
    };

    printf("\n[Test 5] Multi-event (keyboard + mouse on separate streams)\n");

    reset_all();
    feed_kbd_and_inject(kbd_stream, sizeof(kbd_stream));
    feed_mouse_and_inject(mouse_stream, sizeof(mouse_stream));

    /* Keyboard: 'A' make, 'A' break, Right make, Right break. */
    CHECK(g_stub_kbd_count == 4, "four keyboard events");
    if (g_stub_kbd_count >= 1) {
        CHECK(g_stub_kbd[0].make == TRUE && g_stub_kbd[0].scancode == 0x1C
              && g_stub_kbd[0].extended == FALSE, "event 0: 'A' make");
    }
    if (g_stub_kbd_count >= 2) {
        CHECK(g_stub_kbd[1].make == FALSE && g_stub_kbd[1].scancode == 0x1C
              && g_stub_kbd[1].extended == FALSE, "event 1: 'A' break");
    }
    if (g_stub_kbd_count >= 3) {
        CHECK(g_stub_kbd[2].make == TRUE && g_stub_kbd[2].scancode == 0x74
              && g_stub_kbd[2].extended == TRUE, "event 2: Right make");
    }
    if (g_stub_kbd_count >= 4) {
        CHECK(g_stub_kbd[3].make == FALSE && g_stub_kbd[3].scancode == 0x74
              && g_stub_kbd[3].extended == TRUE, "event 3: Right break");
    }

    /* Mouse: one packet. */
    CHECK(g_stub_mouse_count == 1, "one mouse packet");
    if (g_stub_mouse_count >= 1) {
        CHECK(g_stub_mouse[0].buttons == 0x01, "mouse buttons 0x01");
        CHECK(g_stub_mouse[0].dx == 5, "mouse dx +5");
        CHECK(g_stub_mouse[0].dy == 0, "mouse dy 0");
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int
main(void)
{
    printf("Layer 2 host unit tests (O2 input injection)\n");
    printf("============================================\n");

    /* Wire O2's OS-specific hook to the stub input queue. */
    g_o2_key_injector   = stub_key_injector;
    g_o2_mouse_injector = stub_mouse_injector;

    test_simple_key();
    test_extended_key();
    test_modifier_key();
    test_mouse_packet();
    test_mixed_stream();

    printf("\n=======================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
