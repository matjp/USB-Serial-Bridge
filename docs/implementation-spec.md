# Firmware Coder Implementation Spec
## Exact interfaces, structs, and function signatures for B1–B5, U1–U3, O1–O2

**Document version:** 1.0
**Status:** Approved — ready for implementation
**Author:** Principal Software Architect
**Target:** GNU-EFI 4.0.2, x86_64 UEFI application (PE32+), ISO C99, no std headers
**Source of truth:** `docs/architecture.md` (design), `include/*.h` (ABI), `src/**` (stubs)

---

## 0. Conventions & Build Context

- **Language:** ISO C99, freestanding. **No std headers** (`<string.h>`, `<stdlib.h>`,
  `<stdint.h>`, etc.). Map all types to GNU-EFI `<efi.h>` types: `UINTN`, `UINT32`,
  `UINT8`, `INT8`, `EFI_STATUS`, `CHAR16`, `BOOLEAN`, `EFI_HANDLE`, `EFI_SYSTEM_TABLE`.
- **Calling convention:** `EFIAPI` on any function exposed to UEFI (protocol callbacks,
  `efi_main`). Internal functions are plain C.
- **Build:** `make -C /workspaces/USB-Serial-Bridge` produces `build/bridge.efi`.
  All sources are already wired into the Makefile. **Do not add new source files to the
  build** unless you also update `Makefile` `*_SRCS` and re-verify.
- **Existing headers are authoritative.** `include/mailbox.h`, `include/hid.h`,
  `include/ps2.h` are the ABI. Do not change their public layout or the mailbox
  byte-stream semantics (§6 of architecture.md). You may add internal-only helpers.
- **GNU-EFI does NOT ship USB protocol headers.** There is no `efiusb.h`. The
  `EFI_USB2_HC_PROTOCOL` and `EFI_USB_IO_PROTOCOL` structs must be **defined by the
  Firmware Coder** (from the UEFI 2.x spec) in a new internal header, OR the XHCI
  controller may be driven via MMIO registers directly. See U1/B1 notes.
- **Stub files already exist** with `TODO(Firmware Coder)` markers. Implement in place;
  keep the existing function signatures unless this spec explicitly changes them.

---

## 1. Shared Data Structures (new internal headers)

These are internal to the implementation (not part of the public ABI). Create them
under `src/` as needed. They are shared between the UEFI app (Phase 1) and the bridge
(Phase 2) via the reserved region.

### 1.1 `src/bridge/usb_topology.h` — recorded fixed topology

Captured once by U1, consumed by B1. Lives in the reserved region so the bridge core
can read it after `ExitBootServices`.

```c
#ifndef USB_TOPOLOGY_H
#define USB_TOPOLOGY_H

#include <efi.h>

/* One pre-discovered interrupt IN endpoint. */
typedef struct {
    UINT8  device_addr;   /* USB device address (1..127) */
    UINT8  endpoint;      /* endpoint number, low 4 bits = addr, bit7 = IN */
    UINT8  interval;      /* bInterval (in frames/microframes) */
    UINT16 max_packet;    /* wMaxPacketSize */
    UINT8  speed;         /* 0=full, 1=low, 2=high, 3=super (we only use low/full) */
} USB_ENDPOINT;

/* Fixed topology: exactly one keyboard and one mouse. */
typedef struct {
    USB_ENDPOINT kbd;            /* keyboard interrupt IN endpoint */
    USB_ENDPOINT mouse;          /* mouse interrupt IN endpoint */
    UINT32       xhci_mmio_base; /* XHCI MMIO base (BAR0), for B1 direct drive */
    UINT32       xhci_cap_len;   /* CAPLENGTH, for B1 register offsets */
} USB_TOPOLOGY;

#endif /* USB_TOPOLOGY_H */
```

> **Note on `xhci_mmio_base`:** if B1 drives XHCI via MMIO directly (recommended for
> runtime, since UEFI USB protocols are boot-time only), U1 must record the XHCI BAR0
> base and CAPLENGTH. If B1 instead re-uses `EFI_USB2_HC_PROTOCOL` at runtime (only
> valid if the protocol survives `ExitBootServices`, which is not guaranteed), this
> field is unused. The MMIO-direct path is the primary design.

### 1.2 `src/bridge/hid_event.h` — normalized HID events (B2 → B3)

```c
#ifndef HID_EVENT_H
#define HID_EVENT_H

#include <efi.h>
#include <hid.h>

/* A single parsed keyboard event (one key transition). */
typedef struct {
    UINT8   usage;      /* HID usage code (1..0x65) */
    BOOLEAN make;       /* TRUE = make, FALSE = break */
    UINT8   modifier;   /* current modifier byte (shift/ctrl/alt/gui) */
} HID_KEY_EVENT;

/* A single parsed mouse event. */
typedef struct {
    UINT8 buttons;    /* bit0 left, bit1 right, bit2 middle */
    INT8  dx;         /* signed X delta */
    INT8  dy;         /* signed Y delta */
} HID_MOUSE_EVENT;

#endif /* HID_EVENT_H */
```

---

## 2. Bridge Modules (B1–B5) — highest core, OS-independent

### 2.1 B1 — XHCI periodic-IN driver (`src/bridge/xhci.c`)

**Responsibility:** Drive the XHCI controller directly (XHCI ≥ 1.0 only, C6) to perform
periodic interrupt IN transfers on the two pre-discovered low/full-speed endpoints.
No SuperSpeed, no enumeration, no hotplug, no interrupts.

**Public interface (already in `src/bridge/bridge.h`):**
```c
void bridge_poll_usb(void);
```

**Internal state (file-local, in the reserved region):**
```c
/* XHCI register block (subset needed). Offsets relative to xhci_mmio_base. */
typedef struct {
    volatile UINT32 *cap;      /* capability registers (HCSPARAMS1, HCCPARAMS) */
    volatile UINT32 *op;       /* operational registers (CMD, STS, CRCR, DCBAAP) */
    volatile UINT32 *doorbell; /* doorbell array */
    volatile UINT32 *rt;       /* runtime registers (ERSTBA, ERDP) */
    UINT32  max_slots;         /* from HCSPARAMS1 */
    UINT32  max_eps;           /* from HCSPARAMS1 */
    UINT32  max_scratchpad;    /* from HCSPARAMS1 */
    UINT32  page_size;         /* from PAGESIZE */
} XHCI;
```

**Required implementation steps:**
1. **Verify XHCI ≥ 1.0 (C6)** — read `HCSPARAMS1`/`HCCPARAMS` capability registers.
   The XHCI spec version is in `HCCPARAMS1` bits 31:24 (e.g. 0x10 = 1.0). If < 1.0,
   halt and signal failure (see B5 error handling). This is a belt-and-suspenders check
   on top of U1's check.
2. **Reset the controller** (`USBCMD` HCRST), wait for `USBSTS` HCHalted.
3. **Program the device context base array** (`DCBAAP`), allocate slot/endpoint context
   for the two devices. Because U1 already enumerated the devices, the bridge must
   **re-create the minimal device/endpoint context** for the two known endpoints — it
   does not re-enumerate.
4. **Set up two interrupt IN endpoints** as periodic transfers:
   - Allocate a transfer ring (TRB ring) per endpoint.
   - Program the endpoint context with the recorded `interval`, `max_packet`, `speed`.
   - Ring the doorbell to start the periodic IN.
5. **Poll for completion** — check the event ring / `ERDP` for Transfer Event TRBs.
   On a successful transfer, copy the received bytes into a raw HID report buffer.
6. **Produce raw HID reports** for B2:
   ```c
   /* file-local output consumed by bridge_parse_hid() */
   extern HID_KBD_REPORT   g_raw_kbd;    /* 8 bytes */
   extern HID_MOUSE_REPORT g_raw_mouse;  /* 3 bytes */
   extern BOOLEAN g_kbd_valid;           /* set when a fresh kbd report is ready */
   extern BOOLEAN g_mouse_valid;         /* set when a fresh mouse report is ready */
   ```

**Constraints:**
- XHCI ≥ 1.0 only. No EHCI/UHCI/OHCI.
- No SuperSpeed — only the two low/full-speed interrupt endpoints.
- No interrupts — poll the event ring in `bridge_poll_usb()`.
- No enumeration, no hotplug — fixed topology from U1.

### 2.2 B2 — HID report parser (`src/bridge/hid_parser.c`)

**Responsibility:** Parse the raw boot-protocol reports from B1 into normalized events.

**Public interface (already in `src/bridge/bridge.h`):**
```c
void bridge_parse_hid(void);
```

**Required implementation steps:**
1. If `g_kbd_valid`, parse `g_raw_kbd` (`HID_KBD_REPORT`):
   - Compare against the previous keyboard report to detect **make/break** transitions
     per key (a key present now but not before = make; present before but not now =
     break). This is the standard HID keyboard diff.
   - Emit a `HID_KEY_EVENT` per transition into a small file-local queue.
   - Track the current `modifier` byte.
2. If `g_mouse_valid`, parse `g_raw_mouse` (`HID_MOUSE_REPORT`):
   - Emit a `HID_MOUSE_EVENT` with `buttons`, `dx`, `dy`.
3. Clear the `g_*_valid` flags after consuming.

**Internal output (file-local, consumed by B3):**
```c
#define HID_EVENT_QUEUE_MAX 16
typedef struct {
    HID_KEY_EVENT   keys[HID_EVENT_QUEUE_MAX];
    UINTN           key_count;
    HID_MOUSE_EVENT mouse;
    BOOLEAN         mouse_valid;
} HID_EVENT_QUEUE;
extern HID_EVENT_QUEUE g_hid_events;
```

### 2.3 B3 — HID → PS/2 Set 1 translator (`src/bridge/hid_ps2.c`)

**Responsibility:** Translate normalized HID events into the virtual PS/2 byte stream.

**Public interface (already in `src/bridge/bridge.h`):**
```c
void bridge_translate_ps2(void);
```

**Required implementation steps:**
1. **Keyboard:** map each `HID_KEY_EVENT.usage` to a PS/2 **Set 1** scancode via a
   static table. Emit:
   - Make: the scancode byte (or `0xE0` + extended scancode for extended keys).
   - Break: the scancode byte with `PS2_BREAK_BIT` (0x80) set (or `0xE0` + extended
     scancode + 0x80).
   - Use `PS2_EXT_PREFIX` (0xE0) from `include/ps2.h` for extended keys (arrows, etc.).
2. **Mouse:** assemble a 3-byte packet `[buttons, dx, dy]` (`PS2_MOUSE_PKT`) from the
   `HID_MOUSE_EVENT`.
3. **Emit the byte stream** into a file-local output buffer for B4:
   ```c
   #define PS2_STREAM_MAX 32
   typedef struct {
       UINT8  bytes[PS2_STREAM_MAX];
       UINTN  count;
   } PS2_STREAM;
   extern PS2_STREAM g_ps2_stream;
   ```

**Reference table (partial — Firmware Coder fills the full Set 1 table):**
| HID usage | Set 1 make | Set 1 break |
|-----------|------------|-------------|
| 0x04 (A)          | 0x1C      | 0x9C        |
| 0x05 (B)          | 0x32      | 0xB2        |
| 0x1E (1)          | 0x02      | 0x82        |
| 0x28 (Enter)      | 0x1C      | 0x9C        |
| 0x29 (Esc)        | 0x01      | 0x81        |
| 0x2A (Backspace)  | 0x0E      | 0x8E        |
| 0x2B (Tab)        | 0x0F      | 0x8F        |
| 0x39 (Space)      | 0x39      | 0xB9        |
| 0x4F (Right, ext) | 0xE0 0x74 | 0xE0 0xF4    |
| 0x50 (Left, ext)  | 0xE0 0x6B | 0xE0 0xEB    |
| 0x51 (Down, ext)  | 0xE0 0x72 | 0xE0 0xF2    |
| 0x52 (Up, ext)    | 0xE0 0x75 | 0xE0 0xF5    |

> The full table must match the OS's `NORMAL_KEY_SCAN_DECODE_TABLE` (PS/2 Set 1) so
> the OS decodes bytes identically to a real PS/2 keyboard. Verify against the
> reference OS (TempleOS `Keyboard.HC`).

### 2.4 B4 — Mailbox writer (`src/bridge/mailbox_writer.c`)

**Responsibility:** Write the translated PS/2 byte stream into the cross-core mailbox.

**Public interface (already in `src/bridge/bridge.h`):**
```c
void bridge_write_mailbox(MAILBOX *mb);
```

**Required implementation steps:**
1. For each byte in `g_ps2_stream`, call `mailbox_write(mb, byte)` (from
   `include/mailbox.h`). This already does the `mfence` + `head++` ordering.
2. Reset `g_ps2_stream.count = 0` after writing.
3. **Overflow policy:** if the mailbox is full (producer would lap the consumer), the
   bridge must not corrupt the ring. Because the consumer drains continuously, the
   simplest safe policy is to **drop the oldest pending bytes** (advance `head` past
   them) rather than block. Document the chosen policy in a comment. (The consumer on
   core 0 drains fast, so overflow is unlikely in practice.)

### 2.5 B5 — Bridge core entry / poll loop (`src/bridge/bridge_entry.c`)

**Responsibility:** Bridge core entry point and TDM poll loop.

**Public interface (already in `src/bridge/bridge.h`):**
```c
void bridge_entry(void);
```

**Required implementation steps:**
1. On entry (after SIPI bring-up), locate the mailbox via `mailbox_lookup()`.
2. Run the poll loop:
   ```c
   void bridge_entry(void) {
       MAILBOX *mb = mailbox_lookup();
       for (;;) {
           bridge_poll_usb();          /* B1 */
           bridge_parse_hid();         /* B2 */
           bridge_translate_ps2();     /* B3 */
           if (mb) bridge_write_mailbox(mb); /* B4 */
           /* TDM: yield the core back to the OS background task (Seth) until
              the next bridge time slot. Implemented by the timer ISR that
              switches between bridge context and OS task context. */
       }
   }
   ```
3. **TDM handshake:** the bridge runs in its own time slot on the highest core. The
   mechanism is a timer ISR on that core that round-robin switches between the bridge
   context and the OS background task context (save/restore registers + stack). The
   Firmware Coder implements the context-switch primitive (see §5 U2 for the bring-up
   side). The bridge loop itself is a simple `for(;;)`; the timer ISR preempts it at
   slot boundaries.
4. **Error handling:** if B1 detects a non-XHCI-≥1.0 controller or a fatal XHCI fault,
   the bridge should halt cleanly (e.g., enter an infinite idle loop) rather than
   corrupt the mailbox. Log via a reserved status word if available.

---

## 3. Input Adapter Modules (O1–O2) — core 0, per-OS

### 3.1 O1 — Mailbox reader (`src/adapter/mailbox_reader.c`)

**Responsibility:** Drain the cross-core mailbox (consumer side).

**Public interface (already in `src/adapter/adapter.h`):**
```c
void adapter_drain_mailbox(MAILBOX *mb);
```

**Required implementation steps:**
1. Loop `mailbox_read(mb, &byte)` until it returns 0 (ring empty).
2. Reassemble the virtual PS/2 byte stream:
   - **Keyboard:** accumulate bytes; a byte with `PS2_BREAK_BIT` (0x80) set is a break;
     a preceding `PS2_EXT_PREFIX` (0xE0) marks an extended key. Pass the raw scancode
     bytes to O2.
   - **Mouse:** accumulate 3-byte packets `[buttons, dx, dy]` (`PS2_MOUSE_PKT_SIZE`).
     Pass each complete packet to O2.
3. **O1 is OS-independent** — it only produces the byte stream. It does not know the OS.

**Internal output (consumed by O2):**
```c
/* file-local, or via adapter.h if shared */
typedef struct {
    UINT8  bytes[MAILBOX_RING_SIZE];
    UINTN  count;
} ADAPTER_STREAM;
extern ADAPTER_STREAM g_adapter_stream;
```

### 3.2 O2 — Input injection (`src/adapter/input_inject.c`)

**Responsibility:** Feed the drained scancodes/packets into the OS's existing input path.

**Public interface (already in `src/adapter/adapter.h`):**
```c
void adapter_inject_input(void);
```

**Required implementation steps (per-OS):**
1. **TempleOS reference:** the injection point is `KeyDev.HC` `PutKey(ch, sc)` — the
   same queue the OS's KBD driver consumes. The adapter writes scancodes into that
   in-memory queue. For the mouse, feed the 3-byte packets into the same buffer
   `Mouse.HC` `MsHardHndlr()` reads.
2. Because the OS is identity-mapped and its input structures live at known addresses,
   the adapter (loaded code) locates and feeds them directly. **No OS source change.**
3. This is the **only OS-specific module.** For a different OS, only O2 changes.

> **O2 is a stub for the reference OS.** The Firmware Coder implements the TempleOS
> hook. A second-OS O2 is a separate portability-demo task (see §6).

---

## 4. UEFI App Modules (U1–U3) — Phase 1, before ExitBootServices

### 4.1 U1 — USB topology discovery (`src/uefi/usb_discovery.c`)

**Responsibility:** Verify XHCI ≥ 1.0 (C6); find kbd+mouse; record endpoints.

**Public interface (already in `src/uefi/uefi.h`):**
```c
EFI_STATUS uefi_verify_xhci(void);
EFI_STATUS uefi_discover_usb(void);
```

**Required implementation steps:**

`uefi_verify_xhci()`:
1. Locate the XHCI controller. Two options:
   - **Preferred:** walk PCI for a device with class code 0x0C0330 (USB 3.0 xHCI).
     Read its BAR0 (MMIO base) and the XHCI capability registers. Check the spec
     version in `HCCPARAMS1` bits 31:24 ≥ 0x10 (1.0).
   - **Alternative:** locate `EFI_USB2_HC_PROTOCOL` and check its `Revision` field
     (≥ 0x00010000 for 1.0). Note: GNU-EFI has no `efiusb.h`; define the protocol
     struct manually if using this path.
2. Return `EFI_SUCCESS` if XHCI ≥ 1.0, else `EFI_UNSUPPORTED` (abort cleanly, no
   EHCI/UHCI/OHCI fallback).

`uefi_discover_usb()`:
1. Use the UEFI USB stack (`EFI_USB2_HC_PROTOCOL` / `EFI_USB_IO_PROTOCOL`) to find the
   single keyboard and single mouse. **Define these protocol structs manually** (GNU-EFI
   lacks `efiusb.h`) — see §7 for the required struct definitions.
2. For each device, record into a `USB_TOPOLOGY` (in the reserved region):
   - device address, configuration, interface.
   - the interrupt IN endpoint: `endpoint`, `interval`, `max_packet`, `speed`.
   - the XHCI MMIO base + CAPLENGTH (for B1 direct drive).
3. **Done once** — no runtime enumeration. Store the `USB_TOPOLOGY` at a known address
   in the reserved region (see U3).

### 4.2 U2 — Highest-core bring-up (`src/uefi/core_bringup.c`)

**Responsibility:** Determine core count; set up the highest core's GDT/stack/page
tables; start it via SIPI, loading the bridge code.

**Public interface (already in `src/uefi/uefi.h`):**
```c
EFI_STATUS uefi_bringup_highest_core(void);
```

**Required implementation steps:**
1. **Determine core count:** CPUID leaf 0xB (x2APIC topology) or the ACPI MADT table.
   Pick the highest-numbered core as the bridge core.
2. **Set up the bridge core's environment:**
   - GDT (flat 64-bit code/data segments).
   - Stack (allocate in the reserved region).
   - Page tables (identity-map the reserved region + XHCI MMIO).
3. **Load the bridge code** into the reserved region (the bridge code is linked into
   the same UEFI image; copy it to the reserved region).
4. **Start the core via SIPI:**
   - Send INIT IPI (vector 0xC4500) then STARTUP IPI (0xC4600 + MPN_VECT), per the
     reference OS's `MultiProc.HC` conventions.
   - The AP starts executing the bridge entry (`bridge_entry`, B5) in its own context.
5. **Load the input adapter** into the reserved region on core 0 (for O1/O2).

### 4.3 U3 — Memory reservation (`src/uefi/mem_reserve.c`)

**Responsibility:** Allocate bridge + mailbox; mark reserved.

**Public interface (already in `src/uefi/uefi.h`):**
```c
EFI_STATUS uefi_reserve_memory(MAILBOX **out_mailbox);
```

**Required implementation steps:**
1. Allocate a page for the `MAILBOX` via `AllocatePages` with
   `EfiReservedMemoryType`. Initialize it (`mailbox_init`) and publish it
   (`mailbox_publish`).
2. Allocate + reserve the bridge code region and the `USB_TOPOLOGY` region with
   `EfiReservedMemoryType`.
3. **Critical (TempleOS / E820-collecting OS):** `EfiReservedMemoryType` alone is NOT
   sufficient. The region must also be carved out of the E820 map the OS collects, or
   placed **above** the OS's physical memory space so it never allocates over it.
   (See architecture.md §9 risk row.)
4. Return the mailbox pointer via `*out_mailbox`.

---

## 5. Cross-Cutting: TDM Context Switch (B5 + U2)

The bridge and the OS background task share the highest core via time-division
multiplexing. This requires a minimal context-switch primitive.

**Interface (new internal header `src/bridge/tdm.h`):**
```c
#ifndef TDM_H
#define TDM_H

#include <efi.h>

/* Save the current (bridge) context and switch to the OS task context.
 * Called from the timer ISR on the highest core at slot boundaries. */
void tdm_switch_to_os(void);

/* Save the OS task context and switch back to the bridge context. */
void tdm_switch_to_bridge(void);

/* Install the timer ISR that drives the TDM round-robin on the highest core. */
void tdm_install_timer(void);

#endif /* TDM_H */
```

**Implementation notes:**
- The timer ISR on the highest core fires at each slot boundary and alternates between
  `tdm_switch_to_os()` and `tdm_switch_to_bridge()`.
- Each switch saves/restores the callee-saved registers + stack pointer (a minimal
  cooperative context switch). No memory protection — both contexts are ring 0 in the
  same address space.
- The bridge context is the `bridge_entry()` stack; the OS task context is the Seth
  task's stack on that core.
- This is the mechanism that gives the bridge its own thread of control (C5) without
  stealing CPU from the OS main thread on core 0.

---

## 6. Task Breakdown & Ordering

Implement in dependency order. Each task is independently verifiable.

| # | Task | Module | Files | Depends on | Verify |
|---|------|--------|-------|------------|--------|
| 1 | Mailbox ABI (already done) | — | `include/mailbox.h`, `src/common/mailbox.c` | — | Layer 0 host test |
| 2 | HID → PS/2 translator | B3 | `src/bridge/hid_ps2.c` | — | Layer 0 host test (pure C) |
| 3 | HID report parser | B2 | `src/bridge/hid_parser.c` | — | Layer 0 host test (pure C) |
| 4 | Mailbox writer | B4 | `src/bridge/mailbox_writer.c` | 1 | Layer 0 host test |
| 5 | Mailbox reader | O1 | `src/adapter/mailbox_reader.c` | 1 | Layer 0 host test |
| 6 | XHCI periodic-IN driver | B1 | `src/bridge/xhci.c` | 1, U1 | Layer 1 (QEMU + real HW) |
| 7 | USB topology discovery | U1 | `src/uefi/usb_discovery.c` | — | Layer 1 |
| 8 | Memory reservation | U3 | `src/uefi/mem_reserve.c` | 1 | Layer 1 |
| 9 | Highest-core bring-up + TDM | U2, B5 | `src/uefi/core_bringup.c`, `src/bridge/bridge_entry.c`, `src/bridge/tdm.h` | 6,7,8 | Layer 1 (real HW) |
| 10 | Input injection (TempleOS) | O2 | `src/adapter/input_inject.c` | 5 | Layer 2 (stub queue) |
| 11 | Second-OS adapter (portability demo) | O2' | new | 10 | Layer 2 |

**Verification layers** (from architecture.md §9):
- **Layer 0:** host unit tests, no QEMU/OS — B2, B3, B4, O1 logic.
- **Layer 1:** UEFI app + bridge, no OS — B1–B5, U1–U3, O1. QEMU/OVMF + real hardware.
- **Layer 2:** O2 against a stub input queue — real hardware.
- **Layer 3:** end-to-end OS boot (optional, final).

---

## 7. Required Manual Protocol Definitions (GNU-EFI lacks `efiusb.h`)

GNU-EFI does not ship the USB protocol headers. The Firmware Coder must define these
(from the UEFI 2.x spec) in a new internal header, e.g. `src/uefi/efiusb.h`, if using
the UEFI USB stack for U1. **Only the fields actually used need to be declared.**

### 7.1 `EFI_USB2_HC_PROTOCOL` (subset)

```c
typedef struct _EFI_USB2_HC_PROTOCOL EFI_USB2_HC_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *EFI_USB2_HC_GET_CAPABILITY)(
    IN  EFI_USB2_HC_PROTOCOL *This,
    OUT UINT8  *MaxSpeed,
    OUT UINT8  *PortNumber,
    OUT UINT8  *Is64BitCapable
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB2_HC_RESET)(
    IN EFI_USB2_HC_PROTOCOL *This,
    IN UINT16 Attributes
    );

struct _EFI_USB2_HC_PROTOCOL {
    EFI_USB2_HC_GET_CAPABILITY GetCapability;
    EFI_USB2_HC_RESET          Reset;
    /* ... remaining members per spec ... */
    UINT32 Revision;   /* 0x00010000 = USB 2.0 (XHCI 1.0) */
};
```

### 7.2 `EFI_USB_IO_PROTOCOL` (subset)

```c
typedef struct _EFI_USB_IO_PROTOCOL EFI_USB_IO_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *EFI_USB_IO_CONTROL_TRANSFER)(
    IN  EFI_USB_IO_PROTOCOL *This,
    IN  EFI_USB_DEVICE_REQUEST *Request,
    IN  EFI_USB_DIRECTION Direction,
    IN  UINT32 Timeout,
    IN  OUT VOID *Data,
    IN  UINTN DataLength,
    OUT UINT32 *Status
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB_IO_BULK_TRANSFER)(
    IN  EFI_USB_IO_PROTOCOL *This,
    IN  UINT8 DeviceEndpoint,
    IN  OUT VOID *Data,
    IN  OUT UINTN *DataLength,
    IN  UINTN Timeout,
    OUT UINT32 *Status
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB_IO_SYNC_INTERRUPT_TRANSFER)(
    IN  EFI_USB_IO_PROTOCOL *This,
    IN  UINT8 DeviceEndpoint,
    IN  OUT VOID *Data,
    IN  OUT UINTN *DataLength,
    IN  UINTN Timeout,
    OUT UINT32 *Status
    );

struct _EFI_USB_IO_PROTOCOL {
    EFI_USB_IO_CONTROL_TRANSFER       ControlTransfer;
    EFI_USB_IO_BULK_TRANSFER          BulkTransfer;
    /* ... AsyncInterruptTransfer, SyncInterruptTransfer, IsochronousTransfer,
       AsyncIsochronousTransfer, GetDeviceDescriptor, GetConfigDescriptor,
       GetInterfaceDescriptor, GetEndpointDescriptor, GetStringDescriptor,
       Reset, GetDevice, GetBus, GetParent, Configure, PortReset, ... */
};
```

> **Recommendation:** For U1, use `EFI_USB_IO_PROTOCOL` to enumerate and read the
> descriptors (device/config/interface/endpoint) to record the topology. For B1 at
> runtime, drive XHCI via MMIO directly (the UEFI USB protocols are boot-time only and
> may not survive `ExitBootServices`).

---

## 8. Acceptance Criteria

The implementation is complete when:

1. **Builds clean:** `make -C /workspaces/USB-Serial-Bridge` produces `build/bridge.efi`
   with no warnings/errors.
2. **Layer 0 passes:** host unit tests for B2, B3, B4, O1 (mailbox ring correctness,
   HID→PS/2 translation, make/break, 0xE0 extended, mouse packets).
3. **Layer 1 passes (QEMU + real hardware):** the UEFI app enumerates USB kbd/mouse,
   reserves memory, SIPI-starts the highest core, and the harness on core 0 drains the
   mailbox and asserts the PS/2 byte stream — with **no OS loaded**.
4. **Layer 2 passes (real hardware):** O2 injects into a stub input queue correctly.
5. **Interface matches spec:** the mailbox ABI (§6 of architecture.md) is unchanged and
   the byte-stream format is exactly PS/2 Set 1 + 3-byte mouse packets. Validated by the
   Architect.
6. **XHCI ≥ 1.0 only (C6):** verified at boot; clean abort if not XHCI ≥ 1.0. No
   SuperSpeed, no EHCI/UHCI/OHCI.
7. **OS-independent:** only O2 is OS-specific; the bridge + ABI are untouched for a
   different OS.

---

## 9. Open Items for the Firmware Coder to Confirm

1. **XHCI MMIO vs UEFI USB stack at runtime:** confirm whether the `EFI_USB2_HC_PROTOCOL`
   survives `ExitBootServices` on the target. Primary design drives XHCI via MMIO
   directly (recorded BAR0/CAPLENGTH from U1).
2. **Full Set 1 scancode table:** fill the complete HID-usage → Set 1 table and verify
   against the reference OS's `NORMAL_KEY_SCAN_DECODE_TABLE`.
3. **Mailbox overflow policy:** confirm the drop-oldest policy is acceptable (consumer
   drains fast, so overflow is unlikely).
4. **TDM slot length:** choose a slot length (e.g. 1–5 ms) that gives the bridge enough
   time to poll USB without starving the OS background task.
