/*
 * virtual_ps2_writer.c - B4 (virtual-port variant): 8042-style producer.
 *
 * Writes the translated PS/2 byte streams into the virtual 8042 port region
 * (see include/virtual_ps2.h) instead of the mailbox rings. This is the
 * bridge-side half of the "virtual ports" design: the OS reads the virtual
 * status/data registers directly, so no per-OS mailbox adapter is needed.
 *
 * Faithful 8042 semantics:
 *   - ONE byte in flight at a time (single data register, mirrors the 8042's
 *     single output buffer). A byte is written only once the previous one has
 *     been consumed (its status bit clear).
 *   - Keyboard is prioritized over mouse (matches the OS's poll order).
 *   - The producer writes the data byte, then mfence(), then sets the status
 *     bit, so the data is visible before the "ready" flag.
 *
 * The OS side must READ-AND-CLEAR the status bit when it reads the data byte
 * (a pure load does not clear it, unlike the real 8042 hardware).
 */

#include <efi.h>
#include <virtual_ps2.h>

#include "bridge.h"
#include "hid_event.h"

/* Write cursors: index of the next byte to emit from each PS/2 stream.
 *
 * B3 (bridge_translate_ps2) regenerates the full packet each poll, but the
 * 8042 model allows only ONE byte in flight at a time. The cursor lets the
 * writer replay a multi-byte packet (e.g. the 3-byte mouse packet) one byte
 * per poll across successive polls, instead of being stuck on byte 0. */
static UINTN g_vp_kbd_idx  = 0;
static UINTN g_vp_mouse_idx = 0;

/* Reset the write cursors (used by host tests between cases). */
void
bridge_virtual_ps2_reset(void)
{
    g_vp_kbd_idx  = 0;
    g_vp_mouse_idx = 0;
}

/* Write the pending PS/2 byte streams to the virtual 8042 port. */
void
bridge_write_virtual_ps2(void)
{
    UINT8 status;

    /* If a byte is still pending (not yet consumed by the OS), do not write
     * another one - the 8042 has a single output buffer. Wait for the OS to
     * read-and-clear the status bit. */
    status = virtual_ps2_read_status();
    if (status & VIRTUAL_PS2_STAT_ANY)
        goto done;

    /* Keyboard first (priority), then mouse. Write at most one byte per
     * call; the next byte is written on a later poll once consumed. */
    if (g_vp_kbd_idx < g_ps2_kbd_stream.count) {
        virtual_ps2_write_data(g_ps2_kbd_stream.bytes[g_vp_kbd_idx]);
        g_vp_kbd_idx++;
        __asm__ __volatile__("mfence" ::: "memory");
        virtual_ps2_set_status(VIRTUAL_PS2_STAT_KBD);
    } else if (g_vp_mouse_idx < g_ps2_mouse_stream.count) {
        virtual_ps2_write_data(g_ps2_mouse_stream.bytes[g_vp_mouse_idx]);
        g_vp_mouse_idx++;
        __asm__ __volatile__("mfence" ::: "memory");
        virtual_ps2_set_status(VIRTUAL_PS2_STAT_MOUSE);
    }

done:
    /* When a stream is exhausted (its packet fully sent), reset its cursor
     * so the next regenerated packet starts at byte 0. */
    if (g_vp_kbd_idx >= g_ps2_kbd_stream.count)
        g_vp_kbd_idx = 0;
    if (g_vp_mouse_idx >= g_ps2_mouse_stream.count)
        g_vp_mouse_idx = 0;

    /* Consume the PS/2 output streams. The stream is regenerated each poll
     * by B3 (bridge_translate_ps2); bytes not yet written here are dropped
     * (input is lossy - the latest HID state is what matters). */
    g_ps2_kbd_stream.count   = 0;
    g_ps2_mouse_stream.count = 0;
}
