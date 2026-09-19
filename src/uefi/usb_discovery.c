/*
 * usb_discovery.c - U1: USB topology discovery + XHCI >= 1.0 verification.
 *
 * Phase 1, before ExitBootServices. Uses the standard UEFI USB stack
 * (EFI_USB2_HC_PROTOCOL / EFI_USB_IO_PROTOCOL) to find the single USB
 * keyboard and mouse and record their interrupt IN endpoints. Done once -
 * no runtime enumeration.
 *
 * NOTE: This is a scaffold. The endpoint-recording logic is filled in by the
 * Firmware Coder (see docs/architecture.md section 7.3, module U1).
 */

#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

#include "uefi.h"
#include "efiusb.h"
#include "../bridge/usb_topology.h"
#include "../bridge/xhci_observer.h"

/* The discovered fixed topology. U1 fills this; U3 copies it into the
 * reserved region and publishes its address (see usb_topology.h). */
USB_TOPOLOGY g_usb_topology;

/* XHCI MMIO base (BAR0, 64-bit) and CAPLENGTH, recorded by
 * uefi_verify_xhci() via the PCI walk and consumed by
 * uefi_discover_usb() to fill the topology. */
static UINT64 g_xhci_mmio_base;
static UINT32 g_xhci_cap_len;

/* The EFI_PCI_IO_PROTOCOL handle for the xHCI controller, retained so the
 * XHCI capability/operational registers can be read via the protocol's
 * Mem.Read accessor (BAR0) instead of a direct MMIO dereference. Direct
 * MMIO dereference of a PCI BAR can hang on real hardware if the region is
 * not mapped in the UEFI app's page tables; Mem.Read performs the access
 * through the firmware and is safe. */
static EFI_PCI_IO_PROTOCOL *g_xhci_pci = NULL;

/* ------------------------------------------------------------------ */
/* PCI helpers                                                         */
/* ------------------------------------------------------------------ */

/* Read a 32-bit PCI config register for the given bus/device/function via
 * the EFI_PCI_IO_PROTOCOL. Returns EFI_SUCCESS on success.
 *
 * The firmware's Pci.Read is built with the MS x64 ABI, while this app is
 * built with the SysV ABI (EFIAPI is empty). It MUST be invoked through
 * uefi_call_wrapper so the arguments are marshalled into the correct
 * registers; a direct call passes them in the wrong registers and can hang
 * or fault on real firmware. */
static EFI_STATUS
pci_read_config32(EFI_PCI_IO_PROTOCOL *pci, UINT32 offset, UINT32 *value)
{
    return uefi_call_wrapper(pci->Pci.Read, 5, pci, EfiPciIoWidthUint32,
                             offset, 1, value);
}

/* ------------------------------------------------------------------ */
/* XHCI capability-register helpers (MMIO)                             */
/* ------------------------------------------------------------------ */

/* Read a 32-bit XHCI capability register at the given offset from the
 * MMIO base, via the EFI_PCI_IO_PROTOCOL Mem.Read accessor (BAR0). This
 * avoids a direct MMIO dereference, which can hang on real hardware if the
 * BAR region is not mapped in the UEFI app's page tables. Returns 0 if the
 * PCI handle is unavailable. */
static UINT32
xhci_cap_read32(UINT64 mmio_base, UINT32 offset)
{
    UINT32 value = 0;
    if (g_xhci_pci != NULL) {
        /* The firmware's Mem.Read is built with the MS x64 ABI, while this
         * app is built with the SysV ABI (EFIAPI is empty). It MUST be
         * invoked through uefi_call_wrapper so the arguments are marshalled
         * into the correct registers; a direct call passes them in the wrong
         * registers and returns garbage (seen on real hardware: the XHCI
         * HCIVERSION read came back wrong, so the >= 1.0 check failed and
         * uefi_verify_xhci returned Unsupported). Mem.Read has 6 args
         * including This. */
        uefi_call_wrapper(g_xhci_pci->Mem.Read, 6, g_xhci_pci,
                          EfiPciIoWidthUint32, 0, offset, 1, &value);
    } else {
        volatile UINT32 *reg = (volatile UINT32 *)(UINTN)(mmio_base + offset);
        value = *reg;
    }
    return value;
}

/* ------------------------------------------------------------------ */
/* XHCI operational-register helpers (MMIO)                            */
/* ------------------------------------------------------------------ */

/* Root-hub PORTSC register layout (XHCI 1.x spec, section 5.4.8). */
#define XHCI_PORTSC_BASE   0x400   /* PORTSC for port 1 (op base + 0x400) */
#define XHCI_PORTSC_STRIDE 0x10    /* each PORTSC is 16 bytes apart */
#define XHCI_PORTSC_CCS    0x1     /* bit 0: Current Connect Status */
#define XHCI_PORTSC_SPEED  0x3C00  /* bits 10:13: device speed */

/* Read a 32-bit XHCI PORTSC register for the given 1-based root-hub port.
 * op_base is the operational register base (mmio_base + CAPLENGTH). Read
 * via the EFI_PCI_IO_PROTOCOL Mem.Read accessor (BAR0) to avoid a direct
 * MMIO dereference that can hang on real hardware. */
static UINT32
xhci_port_read32(UINT64 op_base, UINT32 port)
{
    UINT32 value = 0;
    UINT64 offset = (op_base - g_xhci_mmio_base) + XHCI_PORTSC_BASE +
                    (port - 1) * XHCI_PORTSC_STRIDE;
    if (g_xhci_pci != NULL) {
        /* Same calling-convention requirement as xhci_cap_read32: the
         * firmware's Mem.Read is MS ABI and must go through
         * uefi_call_wrapper (6 args including This). */
        uefi_call_wrapper(g_xhci_pci->Mem.Read, 6, g_xhci_pci,
                          EfiPciIoWidthUint32, 0, offset, 1, &value);
    } else {
        volatile UINT32 *reg = (volatile UINT32 *)(UINTN)
            (op_base + XHCI_PORTSC_BASE + (port - 1) * XHCI_PORTSC_STRIDE);
        value = *reg;
    }
    return value;
}

/* ------------------------------------------------------------------ */
/* uefi_verify_xhci(): verify the host controller is XHCI >= 1.0 (C6).  */
/*                                                                     */
/* Preferred path: walk PCI for a device with class code 0x0C0330       */
/* (USB 3.0 xHCI), read BAR0 (MMIO base, 64-bit) and the XHCI            */
/* capability registers, and check the spec version in HCIVERSION       */
/* (capability offset 0x00, bits 16:31, BCD; 0x0100 = 1.0).             */
/*                                                                     */
/* Alternative path: locate EFI_USB2_HC_PROTOCOL and check Revision     */
/* >= 0x00010000.                                                       */
/*                                                                     */
/* Returns EFI_SUCCESS if XHCI >= 1.0, else EFI_UNSUPPORTED. No         */
/* EHCI/UHCI/OHCI fallback.                                             */
/* ------------------------------------------------------------------ */
EFI_STATUS
uefi_verify_xhci(void)
{
    EFI_STATUS status;
    EFI_HANDLE *handles = NULL;
    UINTN num_handles = 0;
    UINTN i;
    EFI_PCI_IO_PROTOCOL *pci = NULL;
    UINT32 class_code = 0;
    UINT32 bar0 = 0;
    UINT32 bar0_hi = 0;
    UINT32 cap_len = 0;
    UINT32 hciversion = 0;
    UINT32 spec_version = 0;

    /* --- Preferred path: PCI walk for class code 0x0C0330 (xHCI). --- */
    Print(L"BRIDGE-DBG: verify: LocateHandleBuffer(PciIo)\n");
    status = uefi_call_wrapper(
        BS->LocateHandleBuffer, 5,
        ByProtocol, &gEfiPciIoProtocolGuid, NULL, &num_handles, &handles);
    Print(L"BRIDGE-DBG: verify: LocateHandleBuffer(PciIo) -> %r, %d handles\n",
          status, num_handles);
    if (EFI_ERROR(status) || num_handles == 0) {
        /* No PCI IO protocol handles - fall through to the USB2_HC path. */
        goto alt_path;
    }

    for (i = 0; i < num_handles; i++) {
        status = uefi_call_wrapper(
            BS->HandleProtocol, 3,
            handles[i], &gEfiPciIoProtocolGuid, (VOID **)&pci);
        if (EFI_ERROR(status) || pci == NULL)
            continue;

        /* Read the class code at config offset 0x08 (registers 0x08-0x0B:
         * 0x08 = revision, 0x09 = prog-if, 0x0A = subclass, 0x0B = base
         * class). The 32-bit read at 0x08 gives base<<24 | sub<<16 |
         * progif<<8 | rev. We want base=0x0C, sub=0x03, progif=0x30. */
        status = pci_read_config32(pci, 0x08, &class_code);
        if (EFI_ERROR(status))
            continue;

        /* class_code = rev | (progif<<8) | (subclass<<16) | (baseclass<<24).
         * xHCI: baseclass=0x0C, subclass=0x03, progif=0x30. */
        if (((class_code >> 24) & 0xFF) == 0x0C &&
            ((class_code >> 16) & 0xFF) == 0x03 &&
            ((class_code >> 8)  & 0xFF) == 0x30) {
            /* Found the xHCI controller. Read BAR0 (config offset 0x10).
             * BAR0 is a 64-bit MMIO BAR: the low 32 bits (offset 0x10)
             * hold the low base bits (31:4) plus BAR attributes in the
             * low 4 bits; the high 32 bits (offset 0x14) hold the upper
             * base. Read both so the MMIO base is correct even when the
             * BAR is allocated above 4 GB. */
            Print(L"BRIDGE-DBG: verify: found xHCI at PCI handle %d, "
                  L"class=%08X\n", i, class_code);
            g_xhci_pci = pci;   /* retain for safe BAR0 MMIO reads */
            Print(L"BRIDGE-DBG: verify:   Pci.Read BAR0 @0x10\n");
            status = pci_read_config32(pci, 0x10, &bar0);
            Print(L"BRIDGE-DBG: verify:   BAR0=%08X status=%r\n", bar0, status);
            if (EFI_ERROR(status))
                continue;
            Print(L"BRIDGE-DBG: verify:   Pci.Read BAR0hi @0x14\n");
            status = pci_read_config32(pci, 0x14, &bar0_hi);
            Print(L"BRIDGE-DBG: verify:   BAR0hi=%08X status=%r\n",
                  bar0_hi, status);
            if (EFI_ERROR(status))
                continue;

            g_xhci_mmio_base = ((UINT64)bar0_hi << 32) | (bar0 & 0xFFFFFFF0u);
            Print(L"BRIDGE-DBG: verify: BAR0=%08X BAR0hi=%08X mmio=%016llX\n",
                  bar0, bar0_hi, (unsigned long long)g_xhci_mmio_base);

            /* CAPLENGTH is the low byte of the first capability register
             * (offset 0x00 of the MMIO space). */
            Print(L"BRIDGE-DBG: verify:   Mem.Read CAPLEN @0x00\n");
            cap_len = xhci_cap_read32(g_xhci_mmio_base, 0x00) & 0xFF;
            g_xhci_cap_len = cap_len;
            Print(L"BRIDGE-DBG: verify: CAPLEN=%02X\n", cap_len);

            /* The XHCI spec version is HCIVERSION at capability offset
             * 0x00, bits 16:31, in BCD (0x0100 = 1.0). */
            Print(L"BRIDGE-DBG: verify:   Mem.Read HCIVERSION @0x00\n");
            hciversion = xhci_cap_read32(g_xhci_mmio_base, 0x00);
            spec_version = (hciversion >> 16) & 0xFFFF;
            Print(L"BRIDGE-DBG: verify: HCIVERSION=%04X spec=%04X\n",
                  hciversion, spec_version);

            if (spec_version >= 0x0100) {
                /* XHCI >= 1.0 (C6). */
                if (handles)
                    FreePool(handles);
                return EFI_SUCCESS;
            }
            /* Found an xHCI but it is < 1.0 - unsupported. */
            if (handles)
                FreePool(handles);
            return EFI_UNSUPPORTED;
        }
    }

    if (handles)
        FreePool(handles);

alt_path:
    /* --- Alternative path: EFI_USB2_HC_PROTOCOL revision check. --- */
    Print(L"BRIDGE-DBG: verify: alt path EFI_USB2_HC_PROTOCOL\n");
    {
        EFI_USB2_HC_PROTOCOL *hc = NULL;
        status = uefi_call_wrapper(
            BS->LocateProtocol, 3,
            &EFI_USB2_HC_PROTOCOL_GUID, NULL, (VOID **)&hc);
        Print(L"BRIDGE-DBG: verify: LocateProtocol(Usb2Hc) -> %r\n", status);
        if (EFI_ERROR(status) || hc == NULL)
            return EFI_UNSUPPORTED;

        if (hc->Revision >= 0x00010000) {
            /* USB 2.0 protocol revision implies XHCI 1.0. */
            return EFI_SUCCESS;
        }
    }

    return EFI_UNSUPPORTED;
}

/* ------------------------------------------------------------------ */
/* Root-hub port scan.                                                 */
/*                                                                     */
/* The XHCI slot context's Root Hub Port Number must match the physical */
/* root-hub port the device is attached to, or the controller rejects   */
/* the slot/endpoint (Context State Error). U1 records the real port    */
/* numbers here by scanning the root-hub PORTSC registers directly.     */
/*                                                                     */
/* Port count = HCSPARAMS1 bits 31:24 (MaxPorts). HCSPARAMS1 is at      */
/* capability offset 0x04. PORTSC for port N = op_base + 0x400 +        */
/* (N-1)*0x10. CCS (bit 0) = device present; SPEED (bits 10:13) =       */
/* 1=low, 2=full.                                                       */
/*                                                                     */
/* The two connected ports are matched to the discovered kbd/mouse by   */
/* SPEED. If both devices are the same speed, they are assigned in      */
/* port-scan order (first connected port = kbd, second = mouse), which  */
/* matches the UEFI enumeration order for this fixed-topology design.   */
/* If fewer than 2 connected ports are found, the ports are left as 0   */
/* (B1 will still use them; the harness surfaces the fault).            */
/* ------------------------------------------------------------------ */
static void
record_root_hub_ports(void)
{
    UINT32 hcsparams1;
    UINT32 max_ports;
    UINT64 op_base;
    UINT32 port;
    UINT32 portsc;
    UINT32 speed;
    UINT32 kbd_speed;
    UINT32 mouse_speed;
    BOOLEAN kbd_found = FALSE;
    BOOLEAN mouse_found = FALSE;

    if (g_xhci_mmio_base == 0)
        return;

    /* MaxPorts = HCSPARAMS1 bits 31:24 (capability offset 0x04). */
    hcsparams1 = xhci_cap_read32(g_xhci_mmio_base, 0x04);
    max_ports = (hcsparams1 >> 24) & 0xFF;
    if (max_ports == 0)
        return;

    op_base = g_xhci_mmio_base + g_xhci_cap_len;

    /* Convert the topology speed field (0=full, 1=low, 2=high) to the
     * PORTSC SPEED encoding (1=low, 2=full, 3=high) so we can match by
     * speed. */
    kbd_speed   = (g_usb_topology.kbd.speed == 1) ? 1 :
                  (g_usb_topology.kbd.speed == 2) ? 3 : 2;
    mouse_speed = (g_usb_topology.mouse.speed == 1) ? 1 :
                  (g_usb_topology.mouse.speed == 2) ? 3 : 2;

    for (port = 1; port <= max_ports; port++) {
        portsc = xhci_port_read32(op_base, port);

        /* CCS (bit 0): a device is currently connected on this port. */
        if ((portsc & XHCI_PORTSC_CCS) == 0)
            continue;

        /* SPEED (bits 10:13): 1=low, 2=full. */
        speed = (portsc & XHCI_PORTSC_SPEED) >> 10;

        /* Match by speed. If both devices share a speed, the first
         * connected port is the kbd and the second is the mouse
         * (port-scan order matches UEFI enumeration order). */
        if (!kbd_found && speed == kbd_speed) {
            g_usb_topology.kbd.port = (UINT8)port;
            kbd_found = TRUE;
        } else if (!mouse_found && speed == mouse_speed) {
            g_usb_topology.mouse.port = (UINT8)port;
            mouse_found = TRUE;
        }

        if (kbd_found && mouse_found)
            break;
    }
}

/* ------------------------------------------------------------------ */
/* XHCI ring extraction (read-only passive observer).                  */
/*                                                                     */
/* Before ExitBootServices, the bridge AP must NOT write to the xHCI   */
/* controller (the BSP's XhciDxe driver owns it). Instead, the BSP     */
/* extracts the physical addresses of UEFI's xHCI event ring and the   */
/* keyboard/mouse transfer rings (all read-only), stores them in the   */
/* reserved page, and passes them to the AP via the StartupThisAP       */
/* procedure argument. The AP then polls them read-only, duplicating    */
/* packets into VIRTUAL_PS2_BASE.                                      */
/*                                                                     */
/* Event ring:  runtime ERSTBA -> ERST[0] -> segment base + size.      */
/* Transfer rings: op DCBAAP -> DCBAA -> device context -> endpoint    */
/* context TR Dequeue Pointer, matched to kbd/mouse by root-hub port.  */
/* ------------------------------------------------------------------ */

/* Read a 32-bit XHCI MMIO register at an absolute offset from the MMIO
 * base, via the EFI_PCI_IO_PROTOCOL Mem.Read accessor (BAR0). Read-only.
 * Reuses xhci_cap_read32's safe access path (no direct MMIO dereference
 * that could hang on real hardware). */
static UINT32
xhci_mmio_read32(UINT64 mmio_base, UINT64 offset)
{
    return xhci_cap_read32(mmio_base, (UINT32)offset);
}

/* Extract the UEFI xHCI event ring and kbd/mouse transfer ring addresses
 * from the xHCI MMIO registers (read-only). Fills *obs. Returns TRUE on
 * success (both rings located). */
static BOOLEAN
extract_xhci_observer(XHCI_OBSERVER *obs)
{
    UINT64 mmio = g_xhci_mmio_base;
    UINT32 cap_len = g_xhci_cap_len;
    UINT32 rt_off;
    UINT64 erst_addr;
    UINT64 evt_addr;
    UINT32 evt_size;
    UINT64 dcbaa_addr;
    UINT32 hcsparams1;
    UINT32 max_slots;
    UINT32 slot;
    BOOLEAN kbd_found = FALSE;
    BOOLEAN mouse_found = FALSE;

    if (mmio == 0)
        return FALSE;

    /* Runtime register base = mmio + RTSOFF (capability offset 0x18). */
    rt_off = xhci_cap_read32(mmio, 0x18) & 0xFFFFFFF0u;

    /* ERSTBA at runtime offset 0x10 (low), 0x14 (high). */
    erst_addr = ((UINT64)xhci_mmio_read32(mmio, rt_off + 0x14) << 32) |
                xhci_mmio_read32(mmio, rt_off + 0x10);
    if (erst_addr == 0)
        return FALSE;

    /* ERST[0]: seg_addr_low at +0, seg_addr_high at +4, seg_size at +8.
     * The ERST is in memory (allocated by XhciDxe), so a direct read is
     * safe and read-only. */
    {
        volatile UINT32 *erst = (volatile UINT32 *)(UINTN)erst_addr;
        evt_addr = ((UINT64)erst[1] << 32) | erst[0];
        evt_size = erst[2];
    }
    if (evt_addr == 0 || evt_size == 0)
        return FALSE;

    obs->event_ring_addr = evt_addr;
    obs->event_ring_size = evt_size;

    /* DCBAAP at op offset 0x30 (low), 0x34 (high). */
    dcbaa_addr = ((UINT64)xhci_mmio_read32(mmio, cap_len + 0x34) << 32) |
                 xhci_mmio_read32(mmio, cap_len + 0x30);
    if (dcbaa_addr == 0)
        return FALSE;

    hcsparams1 = xhci_cap_read32(mmio, 0x04);
    max_slots = hcsparams1 & 0xFF;

    /* Walk the slots, match by root-hub port number (slot context DWORD 1
     * bits 7:0). The device contexts are in memory (allocated by XhciDxe),
     * so direct reads are safe and read-only. */
    for (slot = 1; slot <= max_slots; slot++) {
        UINT64 dev_ctx_addr;
        volatile UINT32 *slot_ctx;
        UINT32 port;
        UINT8 ep_num;
        UINT32 ep_index;
        volatile UINT32 *ep_ctx;
        UINT64 tr_dequeue;

        dev_ctx_addr = ((volatile UINT64 *)(UINTN)dcbaa_addr)[slot];
        if (dev_ctx_addr == 0)
            continue;

        slot_ctx = (volatile UINT32 *)(UINTN)dev_ctx_addr;
        port = slot_ctx[1] & 0xFF;   /* Root Hub Port Number (bits 7:0) */

        if (port == g_usb_topology.kbd.port && !kbd_found) {
            ep_num = g_usb_topology.kbd.endpoint & 0x0F;
            ep_index = 2 * ep_num + 1;   /* IN endpoint context index */
            ep_ctx = (volatile UINT32 *)(UINTN)
                (dev_ctx_addr + 32 + (UINT64)ep_index * 32);
            tr_dequeue = ((UINT64)ep_ctx[3] << 32) | ep_ctx[2];
            obs->kbd_tr_addr = tr_dequeue;
            obs->kbd_slot = slot;
            kbd_found = TRUE;
        } else if (port == g_usb_topology.mouse.port && !mouse_found) {
            ep_num = g_usb_topology.mouse.endpoint & 0x0F;
            ep_index = 2 * ep_num + 1;   /* IN endpoint context index */
            ep_ctx = (volatile UINT32 *)(UINTN)
                (dev_ctx_addr + 32 + (UINT64)ep_index * 32);
            tr_dequeue = ((UINT64)ep_ctx[3] << 32) | ep_ctx[2];
            obs->mouse_tr_addr = tr_dequeue;
            obs->mouse_slot = slot;
            mouse_found = TRUE;
        }

        if (kbd_found && mouse_found)
            break;
    }

    return kbd_found && mouse_found;
}

/* ------------------------------------------------------------------ */
/* uefi_discover_usb(): find the single boot-protocol keyboard and      */
/* mouse, record their interrupt IN endpoints into g_usb_topology.      */
/* ------------------------------------------------------------------ */
EFI_STATUS
uefi_discover_usb(void)
{
    EFI_STATUS status;
    EFI_HANDLE *handles = NULL;
    UINTN num_handles = 0;
    UINTN i;
    EFI_USB_IO_PROTOCOL *usbio = NULL;
    EFI_USB_DEVICE_DESCRIPTOR dev_desc;
    EFI_USB_INTERFACE_DESCRIPTOR iface_desc;
    EFI_USB_ENDPOINT_DESCRIPTOR ep_desc;
    UINTN ep_idx;
    BOOLEAN found_kbd = FALSE;
    BOOLEAN found_mouse = FALSE;
    UINT8 speed = 0;

    /* Record the XHCI MMIO base + CAPLENGTH from uefi_verify_xhci(). */
    g_usb_topology.xhci_mmio_base = g_xhci_mmio_base;
    g_usb_topology.xhci_cap_len   = g_xhci_cap_len;

    /* Locate all USB device handles (each exposes EFI_USB_IO_PROTOCOL). */
    status = uefi_call_wrapper(
        BS->LocateHandleBuffer, 5,
        ByProtocol, &EFI_USB_IO_PROTOCOL_GUID, NULL, &num_handles, &handles);
    Print(L"BRIDGE-DBG: discover: LocateHandleBuffer(UsbIo) -> %r, %d handles\n",
          status, num_handles);
    if (EFI_ERROR(status) || num_handles == 0) {
        if (handles)
            FreePool(handles);
        return EFI_NOT_FOUND;
    }

    for (i = 0; i < num_handles; i++) {
        status = uefi_call_wrapper(
            BS->HandleProtocol, 3,
            handles[i], &EFI_USB_IO_PROTOCOL_GUID, (VOID **)&usbio);
        if (EFI_ERROR(status) || usbio == NULL) {
            Print(L"BRIDGE-DBG: discover: handle %d HandleProtocol failed (%r)\n",
                  i, status);
            continue;
        }

        /* Read the device descriptor to get the device address. */
        status = uefi_call_wrapper(usbio->GetDeviceDescriptor, 2, usbio, &dev_desc);
        if (EFI_ERROR(status)) {
            Print(L"BRIDGE-DBG: discover: handle %d GetDeviceDescriptor failed (%r)\n",
                  i, status);
            continue;
        }
        Print(L"BRIDGE-DBG: discover: handle %d VID=%04X PID=%04X class=%02X "
              L"sub=%02X proto=%02X maxpkt0=%d\n",
              i, dev_desc.IdVendor, dev_desc.IdProduct, dev_desc.DeviceClass,
              dev_desc.DeviceSubClass, dev_desc.DeviceProtocol,
              dev_desc.MaxPacketSize0);

        /* Get the active interface descriptor. NOTE: the real EDK2
         * EFI_USB_IO_PROTOCOL.GetInterfaceDescriptor takes only (This,
         * InterfaceDescriptor) - it returns the single active interface,
         * NOT an indexed walk. A HID kbd/mouse has exactly one interface,
         * so a single call suffices. */
        status = uefi_call_wrapper(
            usbio->GetInterfaceDescriptor, 2, usbio, &iface_desc);
        if (EFI_ERROR(status)) {
            Print(L"BRIDGE-DBG: discover:   GetInterfaceDescriptor failed (%r)\n",
                  status);
            continue;
        }

        Print(L"BRIDGE-DBG: discover:   iface class=%02X sub=%02X "
              L"proto=%02X eps=%d\n",
              iface_desc.InterfaceClass,
              iface_desc.InterfaceSubClass, iface_desc.InterfaceProtocol,
              iface_desc.NumEndpoints);

        /* Boot-protocol HID: class 3, subclass 1 (boot), protocol 1
         * (keyboard) or 2 (mouse). */
        if (iface_desc.InterfaceClass != USB_CLASS_HID ||
            iface_desc.InterfaceSubClass != USB_HID_SUBCLASS_BOOT)
            continue;

        /* Find the interrupt IN endpoint on this interface. NOTE: the real
         * EDK2 GetEndpointDescriptor takes only (This, EndpointIndex,
         * EndpointDescriptor) - no interface index. */
        for (ep_idx = 0; ep_idx < iface_desc.NumEndpoints; ep_idx++) {
            status = uefi_call_wrapper(
                usbio->GetEndpointDescriptor, 3,
                usbio, (UINT8)ep_idx, &ep_desc);
            if (EFI_ERROR(status))
                break;

            /* Interrupt endpoint, IN direction. */
            if ((ep_desc.Attributes & USB_ENDPOINT_TYPE_MASK) !=
                    USB_ENDPOINT_TYPE_INTERRUPT)
                continue;
            if ((ep_desc.EndpointAddress & USB_ENDPOINT_DIR_IN) == 0)
                continue;

            /* Determine speed from the device descriptor's
             * MaxPacketSize0 (USB 2.0 spec, table 9-8): low-speed = 8,
             * full-speed = 8/16/32/64, high-speed = 64. For the fixed
             * low/full-speed HID topology, 8 => low, 64 => high, and
             * anything else (16/32) => full. */
            if (dev_desc.MaxPacketSize0 == 8)
                speed = 1;   /* low speed */
            else if (dev_desc.MaxPacketSize0 == 64)
                speed = 2;   /* high speed */
            else
                speed = 0;   /* full speed */

            if (iface_desc.InterfaceProtocol == USB_HID_PROTOCOL_KEYBOARD &&
                !found_kbd) {
                g_usb_topology.kbd.endpoint    = ep_desc.EndpointAddress;
                g_usb_topology.kbd.interval    = ep_desc.Interval;
                g_usb_topology.kbd.max_packet  = ep_desc.MaxPacketSize;
                g_usb_topology.kbd.speed       = speed;
                found_kbd = TRUE;
                Print(L"BRIDGE-DBG: discover:   KBD ep=%02X int=%d "
                      L"maxpkt=%d speed=%d\n",
                      ep_desc.EndpointAddress, ep_desc.Interval,
                      ep_desc.MaxPacketSize, speed);
            } else if (iface_desc.InterfaceProtocol == USB_HID_PROTOCOL_MOUSE &&
                       !found_mouse) {
                g_usb_topology.mouse.endpoint    = ep_desc.EndpointAddress;
                g_usb_topology.mouse.interval    = ep_desc.Interval;
                g_usb_topology.mouse.max_packet  = ep_desc.MaxPacketSize;
                g_usb_topology.mouse.speed       = speed;
                found_mouse = TRUE;
                Print(L"BRIDGE-DBG: discover:   MOUSE ep=%02X int=%d "
                      L"maxpkt=%d speed=%d\n",
                      ep_desc.EndpointAddress, ep_desc.Interval,
                      ep_desc.MaxPacketSize, speed);
            }
            break;   /* one interrupt IN endpoint per interface is enough */
        }

        if (found_kbd && found_mouse)
            break;
    }

    if (handles)
        FreePool(handles);

    Print(L"BRIDGE-DBG: discover: done, found_kbd=%d found_mouse=%d\n",
          found_kbd, found_mouse);

    if (!found_kbd || !found_mouse)
        return EFI_NOT_FOUND;

    /* Record the real root-hub port numbers for the slot context. */
    record_root_hub_ports();

    /* Extract the UEFI xHCI event ring + kbd/mouse transfer ring addresses
     * (read-only) and store them in the reserved page for the bridge AP's
     * pre-ExitBootServices passive observer. Best-effort: if the rings are
     * not yet set up, the observer simply idles (read-only) and the full
     * bring-up happens after ExitBootServices. */
    {
        XHCI_OBSERVER *obs = (XHCI_OBSERVER *)(UINTN)XHCI_OBSERVER_ADDR;
        if (extract_xhci_observer(obs)) {
            Print(L"BRIDGE-DBG: discover: observer evt=%016llX size=%d "
                  L"kbd_tr=%016llX mouse_tr=%016llX\n",
                  (unsigned long long)obs->event_ring_addr,
                  obs->event_ring_size,
                  (unsigned long long)obs->kbd_tr_addr,
                  (unsigned long long)obs->mouse_tr_addr);
        } else {
            Print(L"BRIDGE-DBG: discover: observer extraction failed "
                  L"(rings not ready); AP will idle read-only\n");
        }
    }

    return EFI_SUCCESS;
}
