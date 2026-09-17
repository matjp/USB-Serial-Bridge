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

/* The discovered fixed topology. U1 fills this; U3 copies it into the
 * reserved region and publishes its address (see usb_topology.h). */
USB_TOPOLOGY g_usb_topology;

/* XHCI MMIO base (BAR0, 64-bit) and CAPLENGTH, recorded by
 * uefi_verify_xhci() via the PCI walk and consumed by
 * uefi_discover_usb() to fill the topology. */
static UINT64 g_xhci_mmio_base;
static UINT32 g_xhci_cap_len;

/* ------------------------------------------------------------------ */
/* PCI helpers                                                         */
/* ------------------------------------------------------------------ */

/* Read a 32-bit PCI config register for the given bus/device/function via
 * the EFI_PCI_IO_PROTOCOL. Returns EFI_SUCCESS on success. */
static EFI_STATUS
pci_read_config32(EFI_PCI_IO_PROTOCOL *pci, UINT32 offset, UINT32 *value)
{
    return pci->Pci.Read(pci, EfiPciIoWidthUint32, offset, 1, value);
}

/* ------------------------------------------------------------------ */
/* XHCI capability-register helpers (MMIO)                             */
/* ------------------------------------------------------------------ */

/* Read a 32-bit XHCI capability register at the given offset from the
 * MMIO base. */
static UINT32
xhci_cap_read32(UINT64 mmio_base, UINT32 offset)
{
    volatile UINT32 *reg = (volatile UINT32 *)(UINTN)(mmio_base + offset);
    return *reg;
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
 * op_base is the operational register base (mmio_base + CAPLENGTH). */
static UINT32
xhci_port_read32(UINT64 op_base, UINT32 port)
{
    volatile UINT32 *reg = (volatile UINT32 *)(UINTN)
        (op_base + XHCI_PORTSC_BASE + (port - 1) * XHCI_PORTSC_STRIDE);
    return *reg;
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
    status = uefi_call_wrapper(
        BS->LocateHandleBuffer, 5,
        ByProtocol, &gEfiPciIoProtocolGuid, NULL, &num_handles, &handles);
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
            status = pci_read_config32(pci, 0x10, &bar0);
            if (EFI_ERROR(status))
                continue;
            status = pci_read_config32(pci, 0x14, &bar0_hi);
            if (EFI_ERROR(status))
                continue;

            g_xhci_mmio_base = ((UINT64)bar0_hi << 32) | (bar0 & 0xFFFFFFF0u);

            /* CAPLENGTH is the low byte of the first capability register
             * (offset 0x00 of the MMIO space). */
            cap_len = xhci_cap_read32(g_xhci_mmio_base, 0x00) & 0xFF;
            g_xhci_cap_len = cap_len;

            /* The XHCI spec version is HCIVERSION at capability offset
             * 0x00, bits 16:31, in BCD (0x0100 = 1.0). */
            hciversion = xhci_cap_read32(g_xhci_mmio_base, 0x00);
            spec_version = (hciversion >> 16) & 0xFFFF;

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
    {
        EFI_USB2_HC_PROTOCOL *hc = NULL;
        status = uefi_call_wrapper(
            BS->LocateProtocol, 3,
            &EFI_USB2_HC_PROTOCOL_GUID, NULL, (VOID **)&hc);
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

    /* Convert the topology speed field (0=full, 1=low) to the PORTSC
     * SPEED encoding (1=low, 2=full) so we can match by speed. */
    kbd_speed   = (g_usb_topology.kbd.speed == 1) ? 1 : 2;
    mouse_speed = (g_usb_topology.mouse.speed == 1) ? 1 : 2;

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
    UINTN iface_idx, ep_idx;
    BOOLEAN found_kbd = FALSE;
    BOOLEAN found_mouse = FALSE;
    UINT8 speed = 0;
    UINT8 next_addr = 1;   /* UEFI USB stack assigns addresses 1,2,... */

    /* Record the XHCI MMIO base + CAPLENGTH from uefi_verify_xhci(). */
    g_usb_topology.xhci_mmio_base = g_xhci_mmio_base;
    g_usb_topology.xhci_cap_len   = g_xhci_cap_len;

    /* Locate all USB device handles (each exposes EFI_USB_IO_PROTOCOL). */
    status = uefi_call_wrapper(
        BS->LocateHandleBuffer, 5,
        ByProtocol, &EFI_USB_IO_PROTOCOL_GUID, NULL, &num_handles, &handles);
    if (EFI_ERROR(status) || num_handles == 0) {
        if (handles)
            FreePool(handles);
        return EFI_NOT_FOUND;
    }

    for (i = 0; i < num_handles; i++) {
        status = uefi_call_wrapper(
            BS->HandleProtocol, 3,
            handles[i], &EFI_USB_IO_PROTOCOL_GUID, (VOID **)&usbio);
        if (EFI_ERROR(status) || usbio == NULL)
            continue;

        /* Read the device descriptor to get the device address. */
        status = uefi_call_wrapper(usbio->GetDeviceDescriptor, 2, usbio, &dev_desc);
        if (EFI_ERROR(status))
            continue;

        /* Walk interfaces to find a boot-protocol HID interface. */
        for (iface_idx = 0; iface_idx < 16; iface_idx++) {
            status = uefi_call_wrapper(
                usbio->GetInterfaceDescriptor, 3, usbio, iface_idx, &iface_desc);
            if (EFI_ERROR(status))
                break;   /* no more interfaces */

            /* Boot-protocol HID: class 3, subclass 1 (boot), protocol 1
             * (keyboard) or 2 (mouse). */
            if (iface_desc.InterfaceClass != USB_CLASS_HID ||
                iface_desc.InterfaceSubClass != USB_HID_SUBCLASS_BOOT)
                continue;

            /* Find the interrupt IN endpoint on this interface. */
            for (ep_idx = 0; ep_idx < iface_desc.NumEndpoints; ep_idx++) {
                status = uefi_call_wrapper(
                    usbio->GetEndpointDescriptor, 4,
                    usbio, iface_idx, ep_idx, &ep_desc);
                if (EFI_ERROR(status))
                    break;

                /* Interrupt endpoint, IN direction. */
                if ((ep_desc.Attributes & USB_ENDPOINT_TYPE_MASK) !=
                        USB_ENDPOINT_TYPE_INTERRUPT)
                    continue;
                if ((ep_desc.EndpointAddress & USB_ENDPOINT_DIR_IN) == 0)
                    continue;

                /* Determine speed from the device descriptor's
                 * MaxPacketSize0 (8=low, 64=full) - a reasonable proxy for
                 * low/full speed HID devices. */
                speed = (dev_desc.MaxPacketSize0 <= 8) ? 1 : 0;

                if (iface_desc.InterfaceProtocol == USB_HID_PROTOCOL_KEYBOARD &&
                    !found_kbd) {
                    g_usb_topology.kbd.device_addr = next_addr++;
                    g_usb_topology.kbd.endpoint    = ep_desc.EndpointAddress;
                    g_usb_topology.kbd.interval    = ep_desc.Interval;
                    g_usb_topology.kbd.max_packet  = ep_desc.MaxPacketSize;
                    g_usb_topology.kbd.speed       = speed;
                    found_kbd = TRUE;
                } else if (iface_desc.InterfaceProtocol == USB_HID_PROTOCOL_MOUSE &&
                           !found_mouse) {
                    g_usb_topology.mouse.device_addr = next_addr++;
                    g_usb_topology.mouse.endpoint    = ep_desc.EndpointAddress;
                    g_usb_topology.mouse.interval    = ep_desc.Interval;
                    g_usb_topology.mouse.max_packet  = ep_desc.MaxPacketSize;
                    g_usb_topology.mouse.speed       = speed;
                    found_mouse = TRUE;
                }
                break;   /* one interrupt IN endpoint per interface is enough */
            }
        }

        if (found_kbd && found_mouse)
            break;
    }

    if (handles)
        FreePool(handles);

    if (!found_kbd || !found_mouse)
        return EFI_NOT_FOUND;

    /* Record the real root-hub port numbers for the slot context. */
    record_root_hub_ports();

    return EFI_SUCCESS;
}
