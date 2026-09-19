/*
 * virtual_ps2_writer.c - B4: 8042-style producer + virtual IRQ emitter.
 *
 * Writes the translated PS/2 byte streams into the virtual 8042 port region
 * (see include/virtual_ps2.h). This is the bridge-side half of the "virtual
 * IRQ + virtual ports" design: the OS reads the virtual status/data registers
 * from its existing keyboard/mouse ISRs, which are triggered by the virtual
 * interrupt the bridge sends here.
 *
 * Faithful 8042 semantics:
 *   - ONE byte in flight at a time (single data register, mirrors the 8042's
 *     single output buffer). A byte is written only once the previous one has
 *     been consumed (its status bit clear).
 *   - Keyboard is prioritized over mouse (matches the OS's poll order).
 *   - The producer writes the data byte, then mfence(), then sets the status
 *     bit, so the data is visible before the "ready" flag.
 *
 * Virtual IRQ:
 *   - After the data byte is written and the status bit is set, the bridge
 *     sends a virtual interrupt (an IPI via the local APIC ICR) to the OS
 *     core on the vector the OS already uses for that device (0x21 keyboard,
 *     0x2C mouse). The OS's existing ISR fires and reads the virtual data
 *     port. This is what makes the OS's ISR-driven input path work without
 *     the real 8042 producing IRQs.
 *
 * The OS side must READ-AND-CLEAR the status bit when it reads the data byte
 * (a pure load does not clear it, unlike the real 8042 hardware), and must
 * EOI the local APIC in its ISR (the virtual IRQ is APIC-sourced, not
 * PIC-sourced).
 */

#include <efi.h>
#include <virtual_ps2.h>

#include "bridge.h"
#include "bridge_debug.h"
#include "hid_event.h"

/* Write cursors: index of the next byte to emit from each PS/2 stream.
 *
 * B3 (bridge_translate_ps2) regenerates the full packet each poll, but the
 * 8042 model allows only ONE byte in flight at a time. The cursor lets the
 * writer replay a multi-byte packet (e.g. the 3-byte mouse packet) one byte
 * per poll across successive polls, instead of being stuck on byte 0. */
static UINTN g_vp_kbd_idx  = 0;
static UINTN g_vp_mouse_idx = 0;

/* Default (weak) implementation of the bridge debug capture. Appends one
 * serial byte to the shared-memory ring buffer at BRIDGE_DEBUG_ADDR so the
 * BSP can read it back and log it before ExitBootServices. Host tests
 * override this weak function with a mock (the fixed address is unmapped
 * on the host). */
void
bridge_debug_capture(UINT8 byte, BOOLEAN is_kbd)
{
    BRIDGE_DEBUG_REC *dbg = (BRIDGE_DEBUG_REC *)(UINTN)BRIDGE_DEBUG_ADDR;
    UINT32 idx;

    if (is_kbd)
        dbg->kbd_bytes++;
    else
        dbg->mouse_bytes++;

    idx = dbg->head & (BRIDGE_DEBUG_CAP - 1);
    dbg->data[idx] = byte;
    __asm__ __volatile__("mfence" ::: "memory");
    dbg->head++;
    if (dbg->head == 0)
        dbg->wrap++;
}

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
        /* Capture the actual serial output to shared memory for the BSP
         * to read back and log before ExitBootServices (debug aid). */
        bridge_debug_capture(g_ps2_kbd_stream.bytes[g_vp_kbd_idx], TRUE);
        g_vp_kbd_idx++;
        __asm__ __volatile__("mfence" ::: "memory");
        virtual_ps2_set_status(VIRTUAL_PS2_STAT_KBD);
        /* Virtual IRQ: fire the OS's keyboard ISR so it reads the byte. */
        virtual_ps2_send_irq(VIRTUAL_PS2_IRQ_KBD);
    } else if (g_vp_mouse_idx < g_ps2_mouse_stream.count) {
        virtual_ps2_write_data(g_ps2_mouse_stream.bytes[g_vp_mouse_idx]);
        /* Capture the actual serial output to shared memory for the BSP
         * to read back and log before ExitBootServices (debug aid). */
        bridge_debug_capture(g_ps2_mouse_stream.bytes[g_vp_mouse_idx], FALSE);
        g_vp_mouse_idx++;
        __asm__ __volatile__("mfence" ::: "memory");
        virtual_ps2_set_status(VIRTUAL_PS2_STAT_MOUSE);
        /* Virtual IRQ: fire the OS's mouse ISR so it reads the byte. */
        virtual_ps2_send_irq(VIRTUAL_PS2_IRQ_MOUSE);
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
