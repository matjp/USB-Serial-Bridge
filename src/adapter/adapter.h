/*
 * adapter.h - Internal interface for the per-OS input adapter (Phase 2).
 *
 * The adapter runs on core 0 inside the OS. It drains the mailbox (O1) and
 * injects the virtual PS/2 byte stream into the OS's existing input path
 * (O2). The OS reuses its existing Set 1 decoder and 3-byte mouse-packet
 * parser unchanged. See docs/architecture.md section 7.2.
 */

#ifndef ADAPTER_H
#define ADAPTER_H

#include <efi.h>
#include <mailbox.h>

/* O1: Drain the mailbox, produce the virtual PS/2 byte stream. */
void adapter_drain_mailbox(MAILBOX *mb);

/* O2: Inject the virtual PS/2 byte stream into the OS input path. */
void adapter_inject_input(void);

#endif /* ADAPTER_H */
