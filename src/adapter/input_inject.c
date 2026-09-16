/*
 * input_inject.c - O2: Input injection into the OS input path.
 *
 * Injects the virtual PS/2 byte streams into the OS's existing input path.
 * The OS reuses its existing Set 1 decoder and 3-byte mouse-packet parser
 * unchanged - the adapter only feeds bytes into the same queue the OS's
 * native PS/2 driver would use.
 *
 * Design (see docs/architecture.md section 7.2, module O2):
 *   - OS-independent parsing core: parses the drained byte streams
 *     (g_adapter_kbd_stream / g_adapter_mouse_stream, produced by O1) into
 *     semantic events:
 *       * Keyboard: (make/break, scancode, extended) from PS/2 Set 1 bytes.
 *       * Mouse:    3-byte packets [buttons, dx, dy].
 *     The keyboard and mouse arrive on SEPARATE streams (separate mailbox
 *     rings), so there is no ambiguity between them.
 *   - OS-specific injection hook: the core calls a swappable injector for
 *     each semantic event. The default injectors are safe no-ops; the OS
 *     integration (O2) wires them to the target OS's input path, and the
 *     host test overrides them with a stub queue. This keeps O2 testable on
 *     host and portable to any OS (only the hook changes).
 */

#include <efi.h>
#include <ps2.h>

#include "adapter.h"

/* ------------------------------------------------------------------ */
/* Drained byte streams produced by O1 (mailbox_reader.c).            */
/*                                                                     */
/* ADAPTER_STREAM is file-local in mailbox_reader.c; we mirror its      */
/* layout here (same pattern as the host test) to reach the streams.   */
/* ------------------------------------------------------------------ */
typedef struct {
    UINT8  bytes[MAILBOX_RING_SIZE];
    UINTN  count;
} ADAPTER_STREAM;

extern ADAPTER_STREAM g_adapter_kbd_stream;
extern ADAPTER_STREAM g_adapter_mouse_stream;

/* ------------------------------------------------------------------ */
/* OS-specific injection hooks (the ONLY OS-specific part of O2).     */
/* ------------------------------------------------------------------ */

/* A single keyboard event: make/break, Set 1 scancode, extended flag. */
typedef void (*O2_KEY_INJECTOR)(BOOLEAN make, UINT8 scancode, BOOLEAN extended);

/* A single mouse packet: buttons + signed deltas. */
typedef void (*O2_MOUSE_INJECTOR)(UINT8 buttons, INT8 dx, INT8 dy);

/* Default injection hooks (wired to the target OS at OS-integration time;
 * the OS input-path addresses are NOT available at build time):
 *   - Keyboard: write the scancode into the OS's keyboard input queue.
 *   - Mouse: feed the 3-byte packet into the OS's mouse input buffer.
 * These default bodies are safe no-ops: they do NOT dereference any
 * address, so they cannot crash. Do NOT hardcode fake addresses here. */
static void
o2_inject_key_default(BOOLEAN make, UINT8 scancode, BOOLEAN extended)
{
    /* TODO(OS integration): write into the target OS's keyboard queue. */
    (void)make;
    (void)scancode;
    (void)extended;
}

static void
o2_inject_mouse_default(UINT8 buttons, INT8 dx, INT8 dy)
{
    /* TODO(OS integration): write into the target OS's mouse buffer. */
    (void)buttons;
    (void)dx;
    (void)dy;
}

/* Swappable hooks. Default to the safe no-op injectors. The OS integration
 * (O2) wires these to the target OS's input path; the host test overrides
 * them to capture what O2 would inject into a stub queue. */
O2_KEY_INJECTOR   g_o2_key_injector   = o2_inject_key_default;
O2_MOUSE_INJECTOR g_o2_mouse_injector = o2_inject_mouse_default;

/* ------------------------------------------------------------------ */
/* OS-independent parsing core.                                       */
/* ------------------------------------------------------------------ */

/* Keyboard parser state. */
static BOOLEAN g_kbd_pending_ext;   /* a 0xE0 prefix precedes the next code */

/* Mouse parser state (persistent across drain batches so a packet split
 * across a batch boundary is still completed). */
static UINT8 g_mouse_buf[PS2_MOUSE_PKT_SIZE];
static UINTN g_mouse_idx;

/* Reset the parser state (used by the host test between cases). */
void
o2_reset_state(void)
{
    g_kbd_pending_ext = FALSE;
    g_mouse_idx       = 0;
}

/* Feed one keyboard scancode byte. */
static void
o2_parse_kbd_byte(UINT8 byte)
{
    BOOLEAN make;
    UINT8   scancode;

    if (byte == PS2_EXT_PREFIX) {
        g_kbd_pending_ext = TRUE;
        return;
    }

    make     = ((byte & PS2_BREAK_BIT) == 0);
    scancode = (UINT8)(byte & ~PS2_BREAK_BIT);

    g_o2_key_injector(make, scancode, g_kbd_pending_ext);
    g_kbd_pending_ext = FALSE;
}

/* Feed one mouse byte; emit a complete packet when 3 bytes accumulate. */
static void
o2_parse_mouse_byte(UINT8 byte)
{
    g_mouse_buf[g_mouse_idx++] = byte;

    if (g_mouse_idx == PS2_MOUSE_PKT_SIZE) {
        g_o2_mouse_injector(g_mouse_buf[0],
                            (INT8)g_mouse_buf[1],
                            (INT8)g_mouse_buf[2]);
        g_mouse_idx = 0;
    }
}

void
adapter_inject_input(void)
{
    UINTN i;

    /* Keyboard stream: every byte is a Set 1 scancode or 0xE0 prefix. */
    for (i = 0; i < g_adapter_kbd_stream.count; i++)
        o2_parse_kbd_byte(g_adapter_kbd_stream.bytes[i]);

    /* Mouse stream: every byte is part of a 3-byte packet. */
    for (i = 0; i < g_adapter_mouse_stream.count; i++)
        o2_parse_mouse_byte(g_adapter_mouse_stream.bytes[i]);
}
