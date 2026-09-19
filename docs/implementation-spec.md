# Firmware Coder Implementation Spec
## Exact interfaces, structs, and function signatures for B1–B5, U1–U3, O1

**Document version:** 1.2 (Layer 2 — O1 reader + stub harness implemented)
**Status:** Approved — Layer 2 (O1) implemented; Layer 1 real-hardware bring-up pending
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
- **Build platform (GitHub Actions only):** the firmware is built exclusively on GitHub
  Actions (Ubuntu). Ubuntu's binutils ships the `efi-app-x86_64` objcopy target, so
  `objcopy --target=efi-app-x86_64` emits a valid PE32+ UEFI image directly from the
  linked `.so` — no post-processing. The build uses the **system** GNU-EFI crt0 and the
  standard installed linker script (`/usr/lib/elf_x86_64_efi.lds`, via `EFI_LDS`); no
  vendored crt0, linker script, or PE-patch script is needed. The CI workflow
  (`.github/workflows/build.yml`) builds both `build/bridge.efi` and
  `build-debug/bridge-debug.efi`, runs the host test suite, stages a bootable image, and
  boots the debug image in QEMU+OVMF to assert XHCI bring-up succeeds.
- **Existing headers are authoritative.** `include/hid.h`,
  `include/ps2.h`, `include/virtual_ps2.h` are the ABI. Do not change their public
  layout or the virtual-port byte-stream semantics (§6 of architecture.md). You may add
  internal-only helpers.
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
    UINT8  endpoint;      /* endpoint number, low 4 bits = addr, bit7 = IN */
    UINT8  interval;      /* bInterval (in frames/microframes) */
    UINT16 max_packet;    /* wMaxPacketSize */
    UINT8  speed;         /* 0=full, 1=low, 2=high, 3=super (we only use low/full) */
} USB_ENDPOINT;

/* Fixed topology: exactly one keyboard and one mouse. */
typedef struct {
    USB_ENDPOINT kbd;            /* keyboard interrupt IN endpoint */
    USB_ENDPOINT mouse;          /* mouse interrupt IN endpoint */
    UINT64       xhci_mmio_base; /* XHCI MMIO base (BAR0, 64-bit), for B1 */
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

**Debugging model (UEFI → XHCI handoff):** The controller is handed off from UEFI in
an unknown, partially-configured state, so re-configuring it is the most failure-prone
part of the design. A bare "fatal" flag is not debuggable. B1 therefore records a
**structured fault record** (`src/bridge/xhci_fault.h`) in the reserved region, published
so the Layer 1 harness on core 0 can read it after the bridge halts:
```c
typedef struct {
    UINT32 magic;           /* XHCI_FAULT_MAGIC sanity check */
    UINT32 stage;           /* XHCI_STAGE_* where bring-up stopped */
    UINT32 hint;            /* XHCI_FAULT_HINT_* for the readout */
    UINT32 usbsts;          /* USBSTS register at failure */
    UINT32 usbcmd;          /* USBCMD register at failure */
    UINT32 crcr;            /* CRCR register at failure */
    UINT32 last_cc;         /* last completion code seen (poll stage) */
    UINT32 last_trb_type;   /* last TRB type seen (poll stage) */
    UINT32 doorbell;        /* last doorbell value written */
    UINT32 reserved[7];
} XHCI_FAULT;
```
- **Staged bring-up:** each handoff step records its own `stage` (verify, reset, rings,
  devices, transfer, run, doorbell, poll) so the fault record says exactly where
  bring-up stopped.
- **Register snapshot:** on a fatal fault, B1 snapshots `USBSTS`/`USBCMD`/`CRCR` so the
  harness can report what the controller said.
- **Poll-stage errors:** a non-success completion code is recorded (stage = poll) but
  does NOT halt the bridge — a transient error on one endpoint should not kill the
  whole bridge.
- **Console readout (`src/uefi/l1_harness.c`):** after the bridge is started via
  `EFI_MP_SERVICES_PROTOCOL`, the
  UEFI app calls `uefi_check_bridge_fault()`. It waits (bounded) for the bridge to
  publish its fault record, and if the record's magic is set, prints a compact,
  human-readable diagnosis to the UEFI console (ConOut) and halts — it never boots the
  OS. The output is kept to ~12 lines to fit the UEFI-guaranteed 80×25 console (mode 0).
  If the bridge is healthy, the app proceeds to hand off to the OS. This is the primary
  (and on modern PCs, only) output channel — physical serial ports are gone, so ConOut
  is the surface.
- **Fault-injection host test (`tests/test_xhci_fault.c`):** the real B1 driver
  (`src/bridge/xhci.c`) is compiled on the host with a mock register file. Its register
  accessors (`xhci_read32`/`xhci_write32`) and the topology/fault indirection points
  (`usb_topology_get`/`xhci_fault_publish_rec`/`xhci_fault_get`) are weak symbols, so the
  test overrides them to drive `bridge_poll_usb()` into real failure paths and assert the
  fault record. It exercises the verify failure (spec < 1.0) and the reset failure
  (USBSTS.HCH never set), and confirms a healthy controller produces no fault. This
  verifies the new failure code end-to-end without hardware.
- **Debug-build success dump (`BRIDGE_DEBUG`):** the fault record covers failure, but
  for downstream debugging and verification it is also useful to capture the ACTUAL XHCI
  hardware state when bring-up SUCCEEDS. A `make debug` target builds
  `build-debug/bridge-debug.efi` with `-DBRIDGE_DEBUG`. In that build B1 fills and
  publishes a **success record** (`src/bridge/xhci_status.h`, `XHCI_STATUS`) at the next
  free reserved-region pointer slot (`0x10000020`) after bring-up completes — the
  register snapshot (`USBSTS`/`USBCMD`/`CRCR`), the controller capabilities
  (slots/eps/scratchpad/page size), the MMIO base + CAPLENGTH, and the discovered
  kbd/mouse endpoint configuration. The Layer 1 harness then prints this state to the
  console on success (bounded wait for the record, ~6 lines, fits 80×25). The normal
  `bridge.efi` build is unchanged (silent on success).
- **Persistent bridge debug channel — virtual debug serial (`BRIDGE_DEBUG`):** the BSP
  harness and the UEFI console are only available during the boot phase; once the bridge
  is handed off (or the BSP harness is gone), the bridge has no output channel. To debug
  the bridge after handoff, the bridge implements its own **virtual debug serial port**
  in shared memory — a fixed ring buffer in the reserved region that the bridge writes
  diagnostic text to (its "serial out") and that the OS (or the BSP harness, before
  handoff) reads (its "serial in"). Same producer/consumer pattern as the virtual 8042
  port, but for debug output instead of input.
  - **Why this design:** (1) **persistent** — lives in the reserved region, survives
    handoff and the BSP harness being gone; (2) **OS-independent** — the bridge just
    writes bytes, any OS can read them; (3) **no console-driver dependency** — plain
    memory stores to a fixed address, avoiding the page-table / firmware-driver concerns
    of calling `ConOut->OutputString` from the AP (which runs on the firmware's page
    tables, and whose console driver code may live above 4 GB); (4) **reuses the
    established pattern** — the same fixed-address shared-memory mechanism as the
    virtual 8042 port.
  - **Layout:** a fixed ring buffer of NUL-terminated diagnostic lines at a new address
    in the reserved region, clear of the existing slots (`0x10000000` mailbox,
    `0x10000008` topology, `0x10000018` fault, `0x10000020` status, `0x10000030` virtual
    8042). The bridge writes lines via a `bridge_debug_puts()` helper; a reader (BSP
    harness now, OS later) drains the buffer.
  - **Gating:** compiled in **only** under `BRIDGE_DEBUG` (the debug build). The normal
    `bridge.efi` build is unchanged — no debug-serial writes, no reserved-region debug
    buffer. Keeps the release image silent and minimal.

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
3. **Emit the byte streams** into file-local output buffers for B4. Keyboard and
   mouse are kept on **separate streams** (the bridge prioritizes keyboard over mouse
   when writing the single virtual data slot):
   ```c
   #define PS2_STREAM_MAX 32
   typedef struct {
       UINT8  bytes[PS2_STREAM_MAX];
       UINTN  count;
   } PS2_STREAM;
   extern PS2_STREAM g_ps2_kbd_stream;    /* keyboard scancodes only */
   extern PS2_STREAM g_ps2_mouse_stream;  /* mouse 3-byte packets only */
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

### 2.4 B4 — Virtual 8042 port writer + IRQ emitter (`src/bridge/virtual_ps2_writer.c`)

**Responsibility:** Write the translated PS/2 byte stream into the virtual 8042 port
region (see `include/virtual_ps2.h`), with faithful 8042 single-output-buffer
semantics, and send the **virtual IRQ** so the OS's existing ISR-driven input path
fires.

**Public interface (already in `src/bridge/bridge.h`):**
```c
void bridge_write_virtual_ps2(void);
void bridge_virtual_ps2_reset(void);   /* reset write cursors (host tests) */
```

**Required implementation steps:**
1. **One byte in flight:** read the virtual status register. If any status bit is set
   (`VIRTUAL_PS2_STAT_ANY`), the OS has not yet consumed the previous byte — do **not**
   write another. Return (the next byte is written on a later poll once consumed).
2. **Write one byte:** keyboard first (priority), then mouse. Write the byte to the
   virtual data register, `mfence()`, then set the status bit (`STAT_KBD` or
   `STAT_MOUSE`).
3. **Send the virtual IRQ:** after the data byte is written and the status bit is set,
   call `virtual_ps2_send_irq(VIRTUAL_PS2_IRQ_KBD)` (or `_MOUSE`). This delivers an IPI
   via the local APIC ICR to the OS core on the device's vector, so the OS's existing
   keyboard/mouse ISR fires and reads the virtual data port. The IRQ is sent only after
   the status bit is set, so the data is visible when the ISR runs.
4. **Write cursor:** B3 regenerates the full packet each poll, but the 8042 allows only
   one byte in flight. Maintain a per-device write cursor (`g_vp_kbd_idx`,
   `g_vp_mouse_idx`) that advances through the regenerated packet one byte per poll, so
   a multi-byte packet (e.g. the 3-byte mouse packet) is emitted byte-by-byte across
   polls. Reset a cursor when it reaches the stream's count.
5. **Consume the streams:** reset `g_ps2_kbd_stream.count = 0` and
   `g_ps2_mouse_stream.count = 0` after writing. Bytes not yet written are dropped
   (input is lossy — the latest HID state is what matters).
6. **Weak accessors:** the writer reads/writes the fixed addresses and sends the IRQ via
   weak functions (`virtual_ps2_read_status`, `virtual_ps2_write_data`,
   `virtual_ps2_set_status`, `virtual_ps2_send_irq`) so host tests can override them
   with a mock register file (the fixed addresses are unmapped on the host).

### 2.5 B5 — Bridge core entry / poll loop (`src/bridge/bridge_entry.c`)

**Responsibility:** Bridge core entry point and TDM poll loop.

**Public interface (already in `src/bridge/bridge.h`):**
```c
void bridge_entry(void);
```

**Required implementation steps:**
1. Run the poll loop:
   ```c
   void bridge_entry(void) {
       for (;;) {
           bridge_poll_usb();          /* B1 */
           bridge_parse_hid();         /* B2 */
           bridge_translate_ps2();     /* B3 */
           bridge_write_virtual_ps2(); /* B4 */
           /* TDM: yield the core back to the OS background task (Seth) until
              the next bridge time slot. Implemented by the timer ISR that
              switches between bridge context and OS task context. */
       }
   }
   ```
2. **TDM handshake:** the bridge runs in its own time slot on the highest core. The
   mechanism is a timer ISR on that core that round-robin switches between the bridge
   context and the OS background task context (save/restore registers + stack). The
   Firmware Coder implements the context-switch primitive (see §5 U2 for the bring-up
   side). The bridge loop itself is a simple `for(;;)`; the timer ISR preempts it at
   slot boundaries.
3. **Error handling:** if B1 detects a non-XHCI-≥1.0 controller or a fatal XHCI fault,
   the bridge should halt cleanly (e.g., enter an infinite idle loop) rather than
   corrupt the virtual port region. Log via a reserved status word if available.

---

## 3. Input Adapter Modules (O1) — core 0, per-OS

### 3.1 O1 — Virtual port reader (OS PS/2 driver read, ISR-driven)

**Responsibility:** Read the virtual 8042 port region (consumer side) from the OS's
keyboard/mouse ISR — which the bridge's virtual IRQ triggers — and feed the OS's
existing PS/2 input handlers.

**Status:** ✅ **Implemented** (`src/adapter/virtual_ps2_reader.c`), validated by the
Layer 2 stub harness (`tests/test_layer2_reader.c`, 20 assertions).

**Public interface (in `src/adapter/adapter.h`):**
```c
void adapter_read_virtual_ps2(void);
void adapter_apic_eoi(void);

/* Weak hooks (the only OS-specific part of O1). Default no-op in
   virtual_ps2_reader.c; the OS or the Layer 2 stub overrides them. */
void adapter_feed_kbd_byte(UINT8 byte);
void adapter_feed_mouse_byte(UINT8 byte);
void adapter_apic_eoi_write(UINT32 value);   /* routes the LAPIC EOI MMIO store */
```

**Required implementation steps (implemented):**
1. **Called from the ISR:** the bridge sends a virtual IRQ (IPI) on the device's vector
   (0x21 keyboard / 0x2C mouse) after writing a byte. The OS's existing ISR for that
   vector calls `adapter_read_virtual_ps2()`.
2. Read the virtual status register. If no status bit is set (`VIRTUAL_PS2_STAT_ANY`),
   there is no pending byte — return.
3. Read the virtual data register, then **read-and-clear** the status bit (a pure load
   does NOT clear it, unlike a real 8042). Feed the byte to the OS's existing KBD/mouse
   handler, disambiguated by which status bit was set (`STAT_KBD` vs `STAT_MOUSE`).
4. **EOI the local APIC:** the virtual IRQ is APIC-sourced (an IPI), so the ISR must
   call `adapter_apic_eoi()` (write 0 to the LAPIC EOI register) in addition to the PIC
   EOI the OS already does. This is the one OS-side accommodation for the virtual IRQ.
5. **O1 is OS-specific** only in the final feed step (which OS handler to call). The
   read-and-clear of the virtual port and the APIC EOI are OS-independent. The feed
   step is a **weak hook** (`adapter_feed_kbd_byte` / `adapter_feed_mouse_byte`) so the
   repo stays OS-agnostic; the OS (or the Layer 2 stub) overrides it. The LAPIC EOI
   MMIO store is routed through the weak `adapter_apic_eoi_write()` so the host test
   can observe it (0xFEE000B0 is unmapped on the host).

> **Consumer-side ABI accessors (added to `include/virtual_ps2.h`):** the design
> specifies `virtual_ps2_read_data()` and `virtual_ps2_clear_status()` for the
> read-and-clear, but the ABI header originally declared only the producer-side
> accessors. These two consumer-side weak accessors were added (Architect-approved ABI
> completion) so O1 can read the data byte and clear the status bit.

> **OS-side change (documented, not implemented in this repo):** the OS's PS/2 driver
> must replace its `in 0x60`/`in 0x64` reads with loads from the virtual port region,
> its data reads must be read-and-clear, and its ISRs must EOI the local APIC. This is
> the minimal OS change — the OS reuses 100% of its existing decoder/parser/PutKey
> logic, and the bridge's virtual IRQ triggers the OS's existing ISR path.

> **O2 (input injection) is no longer a separate module.** In the virtual-port design,
> the OS's own PS/2 driver reads the virtual port directly from its ISR and feeds its
> existing input path — there is no separate injection adapter. The only OS-side change
> is the read-and-clear at the data-read sites plus the APIC EOI (documented above,
> implemented in the OS, not
> in this repo).

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

**Responsibility:** Determine core count; start the highest AP via the UEFI
`EFI_MP_SERVICES_PROTOCOL` (`StartupThisAP`), which wakes it in its native
long-mode environment and runs the bridge entry.

**Public interface (already in `src/uefi/uefi.h`):**
```c
EFI_STATUS uefi_bringup_highest_core(void);
```

**Required implementation steps:**
1. **Locate `EFI_MP_SERVICES_PROTOCOL`** via `BS->LocateProtocol`. Because this
   application runs inside the UEFI environment (before `ExitBootServices`), we
   MUST NOT issue manual INIT-SIPI-SIPI sequences. The firmware abstracts the
   underlying architecture: it wakes the AP in its native environment (already in
   long mode, with UEFI's page tables, GDT, and stack set up) and runs a plain C
   function on it.
2. **Enumerate processors:** `GetNumberOfProcessors`, then `GetProcessorInfo` for
   each to identify the BSP (`PROCESSOR_AS_BSP_BIT`) and pick the highest-numbered
   AP as the bridge core. Use the processor NUMBER (MP Services index), not a raw
   APIC ID (APIC IDs are not guaranteed contiguous).
3. **Allocate the bridge stack** in the reserved region (survives
   `ExitBootServices`; the firmware-provided AP stack may be reclaimed).
4. **Start the highest AP via `StartupThisAP`**, passing the AP procedure
   `bridge_ap_entry` (an `EFIAPI` function that switches to the reserved-region
   bridge stack, sets `g_ap_booted`, and calls `bridge_entry`, B5). Use a finite
   timeout so the BSP can detect a failed bring-up.
5. **Load the input adapter** into the reserved region on core 0 (for O1).

### 4.3 U3 — Memory reservation (`src/uefi/mem_reserve.c`)

**Responsibility:** Allocate bridge + virtual port region; mark reserved.

**Public interface (already in `src/uefi/uefi.h`):**
```c
EFI_STATUS uefi_reserve_memory(void);
```

**Required implementation steps:**
1. Reserve the **virtual 8042 port region** at `VIRTUAL_PS2_BASE` (0x10000030) — a
   small fixed region holding the status and data registers (see
   `include/virtual_ps2.h`). Mark it `EfiReservedMemoryType` so the OS never allocates
   over it.
2. Allocate + reserve the bridge code region and the `USB_TOPOLOGY` region with
   `EfiReservedMemoryType`.
3. **Critical (TempleOS / E820-collecting OS):** `EfiReservedMemoryType` alone is NOT
   sufficient. The region must also be carved out of the E820 map the OS collects, or
   placed **above** the OS's physical memory space so it never allocates over it.
   (See architecture.md §9 risk row.)
4. The virtual port region is a **fixed address** (not a returned pointer) so the OS's
   PS/2 driver can reference it directly without a lookup.

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
| 1 | Virtual 8042 port ABI (already done) | — | `include/virtual_ps2.h`, `src/bridge/virtual_ps2_writer.c` | — | Layer 0 host test |
| 2 | HID → PS/2 translator | B3 | `src/bridge/hid_ps2.c` | — | Layer 0 host test (pure C) |
| 3 | HID report parser | B2 | `src/bridge/hid_parser.c` | — | Layer 0 host test (pure C) |
| 4 | Virtual port writer + IRQ emitter | B4 | `src/bridge/virtual_ps2_writer.c` | 1 | Layer 0 host test |
| 5 | Virtual port reader (ISR-driven) | O1 | `src/adapter/virtual_ps2_reader.c` | 1 | ✅ Layer 0 host test (test_layer2_reader, 20 asserts) |
| 6 | XHCI periodic-IN driver | B1 | `src/bridge/xhci.c` | 1, U1 | Layer 1 (QEMU + real HW) |
| 7 | USB topology discovery | U1 | `src/uefi/usb_discovery.c` | — | Layer 1 |
| 8 | Memory reservation | U3 | `src/uefi/mem_reserve.c` | 1 | Layer 1 |
| 9 | Highest-core bring-up + TDM | U2, B5 | `src/uefi/core_bringup.c`, `src/bridge/bridge_entry.c`, `src/bridge/tdm.h` | 6,7,8 | Layer 1 (real HW) |
| 10 | OS PS/2 driver read-and-clear + APIC EOI (in the OS, not this repo) | O1 | OS source | 5 | Layer 3 (OS boot) |
| 11 | Second-OS virtual-port reader (portability demo) | O1' | new | 5 | Layer 2 |

**Verification layers** (from architecture.md §9):
- **Layer 0:** host unit tests, no QEMU/OS — B2, B3, B4, O1 logic. ✅ **DONE** (133 asserts: 33+30+10+40+20).
- **Layer 1:** UEFI app + bridge, no OS — B1–B5, U1–U3. QEMU/OVMF + real hardware.
  - ✅ **UEFI setup phase (U1 verify+discover, U3 reserve, U2 scaffold, harness) PASSED on
    real hardware** (Toshiba Satellite P50, 2026-09-17): app boots, verifies XHCI ≥ 1.0,
    discovers the real USB kbd/mouse (VID=0x045E PID=0x07B2, kbd ep 0x81 / mouse ep 0x82),
    reserves memory, runs the scaffold bring-up, and hands off to the next boot device.
    Two real-HW hangs found and fixed: (1) direct MMIO dereference in `uefi_verify_xhci`
    → use `EFI_PCI_IO_PROTOCOL.Mem.Read` (commit `c66eaf6`); (2) unzeroed reserved page
    in `uefi_check_bridge_fault` → zero the page after `AllocatePages` (commit `2bd7081`).
  - ⏳ **Bridge core (B1 XHCI handoff) PENDING:** U2 is still a scaffold — the bridge code
    is not loaded onto the AP, so `bridge_poll_usb()` never runs and no `BRIDGE OK` marker
    is produced. Full AP bring-up is the remaining Layer 1 work.
- **Layer 2:** O1 read against a stub PS/2 driver — real hardware. ✅ **Host portion DONE** (test_layer2_reader); real-hardware portion pending.
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
2. **Layer 0 passes:** host unit tests for B2, B3, B4, O1 (virtual port read/write
   correctness, virtual IRQ emission, HID→PS/2 translation, make/break, 0xE0 extended,
   mouse packets). ✅ **DONE** — 133 assertions pass.
3. **Layer 1 passes (QEMU + real hardware):** the UEFI app enumerates USB kbd/mouse,
   reserves memory, starts the highest core via `EFI_MP_SERVICES_PROTOCOL`, and the
   harness on core 0 reads the virtual port region and asserts the PS/2 byte stream —
   with **no OS loaded**.
   - ✅ **UEFI setup phase PASSED on real hardware** (Toshiba Satellite P50, 2026-09-17):
     verify XHCI, discover real kbd/mouse, reserve memory, scaffold bring-up, handoff.
     See architecture.md §9.2 for the trace and the two real-HW bugs fixed.
   - ⏳ **Bridge core (B1 XHCI handoff) PENDING:** U2 is a scaffold; the bridge code is
     not loaded onto the AP, so `bridge_poll_usb()` never runs and no `BRIDGE OK` marker
     is produced. Full AP bring-up is the remaining Layer 1 work.
4. **Layer 2 passes (real hardware):** O1 read-and-clear + APIC EOI against a stub PS/2
   driver works correctly. ✅ **Host portion DONE** (`tests/test_layer2_reader.c`, 20
   asserts); real-hardware portion pending.
5. **Interface matches spec:** the virtual 8042 port ABI + virtual IRQ (§6 of
   architecture.md) is unchanged and the byte-stream format is exactly PS/2 Set 1 +
   3-byte mouse packets. Validated by the Architect.
6. **XHCI ≥ 1.0 only (C6):** verified at boot; clean abort if not XHCI ≥ 1.0. No
   SuperSpeed, no EHCI/UHCI/OHCI.
7. **OS-independent:** only the OS-side read-and-clear + APIC EOI (O1) is OS-specific;
   the bridge + virtual port ABI are untouched for a different OS.

---

## 9. Open Items for the Firmware Coder to Confirm

1. **XHCI MMIO vs UEFI USB stack at runtime:** confirm whether the `EFI_USB2_HC_PROTOCOL`
   survives `ExitBootServices` on the target. Primary design drives XHCI via MMIO
   directly (recorded BAR0/CAPLENGTH from U1).
2. **Full Set 1 scancode table:** fill the complete HID-usage → Set 1 table and verify
   against the reference OS's `NORMAL_KEY_SCAN_DECODE_TABLE`.
3. **Virtual port consumption handshake:** confirm the OS's PS/2 driver data reads are
   read-and-clear (a pure load does not clear the status bit, unlike a real 8042).
   ✅ **Resolved by Layer 2 host test** (`tests/test_layer2_reader.c` validates the
   read-and-clear + kbd-priority + APIC EOI behavior of O1).
4. **Virtual IRQ delivery:** confirm the bridge can send an IPI via the local APIC ICR
   to core 0 on the device's vector (0x21 kbd / 0x2C mouse), and that the OS's ISR EOIs
   the local APIC (the virtual IRQ is APIC-sourced, not PIC-sourced). This is the one
   OS-side accommodation for the virtual IRQ design.
5. **TDM slot length:** choose a slot length (e.g. 1–5 ms) that gives the bridge enough
   time to poll USB without starving the OS background task.
