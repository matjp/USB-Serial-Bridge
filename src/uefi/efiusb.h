/*
 * efiusb.h - Manual UEFI USB protocol definitions (GNU-EFI lacks efiusb.h).
 *
 * GNU-EFI does not ship the USB protocol headers. This internal header
 * defines the subset of EFI_USB2_HC_PROTOCOL and EFI_USB_IO_PROTOCOL (plus
 * the USB descriptor and device-request structs) that U1 (usb_discovery.c)
 * uses to enumerate the single keyboard and mouse via the UEFI USB stack.
 *
 * The struct layouts follow the UEFI 2.x specification. We declare the FULL
 * member lists (not just the fields we touch) so that the members we do
 * access (e.g. EFI_USB2_HC_PROTOCOL.Revision) sit at their correct offsets
 * within the protocol structures.
 *
 * See docs/implementation-spec.md section 7.
 */

#ifndef EFIUSB_H
#define EFIUSB_H

#include <efi.h>

/* ------------------------------------------------------------------ */
/* Protocol GUIDs (from the UEFI 2.x spec).                            */
/*                                                                     */
/* GNU-EFI GUIDs are EFI_GUID variables (not macros), because the      */
/* UEFI boot services take a pointer to the GUID. We define the        */
/* initializer macros and the static variables here. Only              */
/* usb_discovery.c includes this header, so a single static copy per   */
/* translation unit is fine.                                           */
/* ------------------------------------------------------------------ */

#define EFI_USB2_HC_PROTOCOL_GUID_INIT \
    { 0x3e745226, 0x9818, 0x45b6, {0xa2, 0xac, 0xd7, 0xcd, 0x0e, 0x8b, 0xa2, 0xbc} }

#define EFI_USB_IO_PROTOCOL_GUID_INIT \
    { 0x2B2F68D6, 0x0CD2, 0x44e9, {0x8C, 0x0C, 0xBB, 0xF9, 0x19, 0x17, 0xAD, 0x29} }

static EFI_GUID EFI_USB2_HC_PROTOCOL_GUID = EFI_USB2_HC_PROTOCOL_GUID_INIT;
static EFI_GUID EFI_USB_IO_PROTOCOL_GUID  = EFI_USB_IO_PROTOCOL_GUID_INIT;

/* ------------------------------------------------------------------ */
/* USB device request + direction.                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    UINT8  RequestType;
    UINT8  Request;
    UINT16 Value;
    UINT16 Index;
    UINT16 Length;
} EFI_USB_DEVICE_REQUEST;

typedef enum {
    EfiUsbNoDirection,
    EfiUsbDirIn,
    EfiUsbDirOut,
    EfiUsbMaximumDirection
} EFI_USB_DIRECTION;

/* ------------------------------------------------------------------ */
/* USB descriptors (subset of fields we need).                         */
/* ------------------------------------------------------------------ */

typedef struct {
    UINT8  Length;
    UINT8  DescriptorType;
    UINT16 BcdUSB;
    UINT8  DeviceClass;
    UINT8  DeviceSubClass;
    UINT8  DeviceProtocol;
    UINT8  MaxPacketSize0;
    UINT16 IdVendor;
    UINT16 IdProduct;
    UINT16 BcdDevice;
    UINT8  NumConfigurations;
} EFI_USB_DEVICE_DESCRIPTOR;

typedef struct {
    UINT8  Length;
    UINT8  DescriptorType;
    UINT8  InterfaceNumber;
    UINT8  AlternateSetting;
    UINT8  NumEndpoints;
    UINT8  InterfaceClass;
    UINT8  InterfaceSubClass;
    UINT8  InterfaceProtocol;
    UINT8  Interface;
} EFI_USB_INTERFACE_DESCRIPTOR;

typedef struct {
    UINT8  Length;
    UINT8  DescriptorType;
    UINT8  EndpointAddress;
    UINT8  Attributes;
    UINT16 MaxPacketSize;
    UINT8  Interval;
} EFI_USB_ENDPOINT_DESCRIPTOR;

/* EFI_USB_CONFIG_DESCRIPTOR (subset: header + total length). */
typedef struct {
    UINT8  Length;
    UINT8  DescriptorType;
    UINT16 TotalLength;
    UINT8  NumInterfaces;
    UINT8  ConfigurationValue;
    UINT8  Configuration;
    UINT8  Attributes;
    UINT8  MaxPower;
} EFI_USB_CONFIG_DESCRIPTOR;

/* USB descriptor types (bDescriptorType values). */
#define USB_DESC_DEVICE     0x01
#define USB_DESC_CONFIG     0x02
#define USB_DESC_STRING     0x03
#define USB_DESC_INTERFACE  0x04
#define USB_DESC_ENDPOINT   0x05

/* USB endpoint attributes (bAttributes bits 0-1). */
#define USB_ENDPOINT_TYPE_MASK   0x03
#define USB_ENDPOINT_TYPE_CONTROL 0x00
#define USB_ENDPOINT_TYPE_ISO    0x01
#define USB_ENDPOINT_TYPE_BULK   0x02
#define USB_ENDPOINT_TYPE_INTERRUPT 0x03

/* USB endpoint direction (bEndpointAddress bit 7). */
#define USB_ENDPOINT_DIR_IN      0x80

/* USB device classes (bDeviceClass / bInterfaceClass). */
#define USB_CLASS_HID            0x03
#define USB_HID_SUBCLASS_BOOT    0x01
#define USB_HID_PROTOCOL_KEYBOARD 0x01
#define USB_HID_PROTOCOL_MOUSE    0x02

/* ------------------------------------------------------------------ */
/* Supporting types referenced by the protocol typedefs below.         */
/* ------------------------------------------------------------------ */

/* Async USB transfer callback (never used; declared for layout only). */
typedef
VOID
(EFIAPI *EFI_ASYNC_USB_TRANSFER_CALLBACK)(
    IN VOID *Data,
    IN UINTN DataLength,
    IN VOID *Context,
    IN UINT32 Status
    );

/* EFI_USB2_HC_TRANSACTION (isochronous URB; only the header is needed for
 * the struct layout - we never use isochronous transfers). */
typedef struct {
    EFI_USB_DEVICE_REQUEST *Request;
    union {
        UINT32 InTransferLength;
        UINT32 OutTransferLength;
    } TransferLength;
    UINT16 RequestType;
    UINT16 Value;
    UINT16 Index;
    VOID   *Data;
    UINT32 Status;
} EFI_USB2_HC_TRANSACTION;

/* ------------------------------------------------------------------ */
/* EFI_USB2_HC_PROTOCOL (full layout per UEFI 2.x).                    */
/* ------------------------------------------------------------------ */

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

typedef
EFI_STATUS
(EFIAPI *EFI_USB2_HC_CONTROL_TRANSFER)(
    IN  EFI_USB2_HC_PROTOCOL *This,
    IN  UINT8  DeviceAddress,
    IN  UINT8  DeviceSpeed,
    IN  UINTN  MaximumPacketLength,
    IN  EFI_USB_DEVICE_REQUEST *Request,
    IN  EFI_USB_DIRECTION TransferDirection,
    IN  OUT VOID *Data,
    IN  OUT UINTN *DataLength,
    IN  UINTN Timeout,
    OUT UINT32 *Status
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB2_HC_BULK_TRANSFER)(
    IN  EFI_USB2_HC_PROTOCOL *This,
    IN  UINT8  DeviceAddress,
    IN  UINT8  EndPointAddress,
    IN  UINT8  DeviceSpeed,
    IN  UINTN  MaximumPacketLength,
    IN  OUT VOID *Data,
    IN  OUT UINTN *DataLength,
    IN  UINTN Timeout,
    OUT UINT32 *Status
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB2_HC_ASYNC_INTERRUPT_TRANSFER)(
    IN  EFI_USB2_HC_PROTOCOL *This,
    IN  UINT8  DeviceAddress,
    IN  UINT8  EndPointAddress,
    IN  UINT8  DeviceSpeed,
    IN  UINTN  MaximumPacketLength,
    IN  BOOLEAN IsNewTransfer,
    IN  OUT UINTN *DataLength,
    IN  OUT VOID *Data OPTIONAL,
    IN  UINTN Timeout,
    IN  EFI_ASYNC_USB_TRANSFER_CALLBACK CallBack OPTIONAL,
    IN  VOID *Context OPTIONAL
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB2_HC_SYNC_INTERRUPT_TRANSFER)(
    IN  EFI_USB2_HC_PROTOCOL *This,
    IN  UINT8  DeviceAddress,
    IN  UINT8  EndPointAddress,
    IN  UINT8  DeviceSpeed,
    IN  UINTN  MaximumPacketLength,
    IN  OUT VOID *Data,
    IN  OUT UINTN *DataLength,
    IN  UINTN Timeout,
    OUT UINT32 *Status
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB2_HC_ISOCHRONOUS_TRANSFER)(
    IN  EFI_USB2_HC_PROTOCOL *This,
    IN  UINT8  DeviceAddress,
    IN  UINT8  EndPointAddress,
    IN  UINT8  DeviceSpeed,
    IN  UINTN  MaximumPacketLength,
    IN  UINT8  NumberOfURBs,
    IN  OUT EFI_USB2_HC_TRANSACTION *Data,
    IN  UINTN Timeout,
    OUT UINT32 *Status
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB2_HC_ASYNC_ISOCHRONOUS_TRANSFER)(
    IN  EFI_USB2_HC_PROTOCOL *This,
    IN  UINT8  DeviceAddress,
    IN  UINT8  EndPointAddress,
    IN  UINT8  DeviceSpeed,
    IN  UINTN  MaximumPacketLength,
    IN  UINT8  NumberOfURBs,
    IN  OUT EFI_USB2_HC_TRANSACTION *Data,
    IN  EFI_ASYNC_USB_TRANSFER_CALLBACK CallBack OPTIONAL,
    IN  VOID *Context OPTIONAL
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB2_HC_GET_STATE)(
    IN  EFI_USB2_HC_PROTOCOL *This,
    OUT UINT32 *State
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB2_HC_START_HC)(
    IN EFI_USB2_HC_PROTOCOL *This
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB2_HC_STOP_HC)(
    IN EFI_USB2_HC_PROTOCOL *This
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB2_HC_SET_STATE)(
    IN EFI_USB2_HC_PROTOCOL *This,
    IN UINT32 State
    );

struct _EFI_USB2_HC_PROTOCOL {
    EFI_USB2_HC_GET_CAPABILITY              GetCapability;
    EFI_USB2_HC_RESET                       Reset;
    EFI_USB2_HC_CONTROL_TRANSFER            ControlTransfer;
    EFI_USB2_HC_BULK_TRANSFER               BulkTransfer;
    EFI_USB2_HC_ASYNC_INTERRUPT_TRANSFER    AsyncInterruptTransfer;
    EFI_USB2_HC_SYNC_INTERRUPT_TRANSFER     SyncInterruptTransfer;
    EFI_USB2_HC_ISOCHRONOUS_TRANSFER        IsochronousTransfer;
    EFI_USB2_HC_ASYNC_ISOCHRONOUS_TRANSFER  AsyncIsochronousTransfer;
    EFI_USB2_HC_GET_STATE                   GetState;
    EFI_USB2_HC_START_HC                    StartHC;
    EFI_USB2_HC_STOP_HC                     StopHC;
    EFI_USB2_HC_SET_STATE                   SetState;
    UINT32                                  Revision;   /* 0x00010000 = USB 2.0 (XHCI 1.0) */
};

/* ------------------------------------------------------------------ */
/* EFI_USB_IO_PROTOCOL (full layout per UEFI 2.x).                     */
/* ------------------------------------------------------------------ */

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
(EFIAPI *EFI_USB_IO_ASYNC_INTERRUPT_TRANSFER)(
    IN  EFI_USB_IO_PROTOCOL *This,
    IN  UINT8 DeviceEndpoint,
    IN  BOOLEAN IsNewTransfer,
    IN  UINTN PollInterval,
    IN  UINTN DataLength,
    IN  EFI_ASYNC_USB_TRANSFER_CALLBACK CallBack OPTIONAL,
    IN  VOID *Context OPTIONAL
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

typedef
EFI_STATUS
(EFIAPI *EFI_USB_IO_ISOCHRONOUS_TRANSFER)(
    IN  EFI_USB_IO_PROTOCOL *This,
    IN  UINT8 DeviceEndpoint,
    IN  OUT VOID *Data,
    IN  UINTN DataLength,
    OUT UINT32 *Status
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB_IO_ASYNC_ISOCHRONOUS_TRANSFER)(
    IN  EFI_USB_IO_PROTOCOL *This,
    IN  UINT8 DeviceEndpoint,
    IN  OUT VOID *Data,
    IN  UINTN DataLength,
    IN  EFI_ASYNC_USB_TRANSFER_CALLBACK CallBack OPTIONAL,
    IN  VOID *Context OPTIONAL
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB_IO_GET_DEVICE_DESCRIPTOR)(
    IN  EFI_USB_IO_PROTOCOL *This,
    OUT EFI_USB_DEVICE_DESCRIPTOR *DeviceDescriptor
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB_IO_GET_CONFIG_DESCRIPTOR)(
    IN  EFI_USB_IO_PROTOCOL *This,
    OUT UINTN *ConfigDescriptorSize,
    OUT EFI_USB_CONFIG_DESCRIPTOR **ConfigDescriptor
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB_IO_GET_INTERFACE_DESCRIPTOR)(
    IN  EFI_USB_IO_PROTOCOL *This,
    IN  UINTN InterfaceIndex,
    OUT EFI_USB_INTERFACE_DESCRIPTOR *InterfaceDescriptor
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB_IO_GET_ENDPOINT_DESCRIPTOR)(
    IN  EFI_USB_IO_PROTOCOL *This,
    IN  UINTN InterfaceIndex,
    IN  UINTN EndpointIndex,
    OUT EFI_USB_ENDPOINT_DESCRIPTOR *EndpointDescriptor
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB_IO_GET_STRING_DESCRIPTOR)(
    IN  EFI_USB_IO_PROTOCOL *This,
    IN  UINT16 LangID,
    IN  UINT8 StringIndex,
    OUT CHAR16 **String
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB_IO_RESET)(
    IN EFI_USB_IO_PROTOCOL *This
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB_IO_GET_DEVICE)(
    IN  EFI_USB_IO_PROTOCOL *This,
    OUT EFI_USB_DEVICE_DESCRIPTOR *DeviceDescriptor
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB_IO_GET_BUS)(
    IN  EFI_USB_IO_PROTOCOL *This,
    OUT EFI_USB2_HC_PROTOCOL **Bus
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB_IO_GET_PARENT)(
    IN  EFI_USB_IO_PROTOCOL *This,
    OUT EFI_USB_IO_PROTOCOL **Parent
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB_IO_CONFIGURE)(
    IN EFI_USB_IO_PROTOCOL *This,
    IN EFI_USB_CONFIG_DESCRIPTOR *ConfigDescriptor
    );

typedef
EFI_STATUS
(EFIAPI *EFI_USB_IO_PORT_RESET)(
    IN EFI_USB_IO_PROTOCOL *This
    );

struct _EFI_USB_IO_PROTOCOL {
    EFI_USB_IO_CONTROL_TRANSFER            ControlTransfer;
    EFI_USB_IO_BULK_TRANSFER               BulkTransfer;
    EFI_USB_IO_ASYNC_INTERRUPT_TRANSFER    AsyncInterruptTransfer;
    EFI_USB_IO_SYNC_INTERRUPT_TRANSFER     SyncInterruptTransfer;
    EFI_USB_IO_ISOCHRONOUS_TRANSFER        IsochronousTransfer;
    EFI_USB_IO_ASYNC_ISOCHRONOUS_TRANSFER  AsyncIsochronousTransfer;
    EFI_USB_IO_GET_DEVICE_DESCRIPTOR       GetDeviceDescriptor;
    EFI_USB_IO_GET_CONFIG_DESCRIPTOR       GetConfigDescriptor;
    EFI_USB_IO_GET_INTERFACE_DESCRIPTOR    GetInterfaceDescriptor;
    EFI_USB_IO_GET_ENDPOINT_DESCRIPTOR     GetEndpointDescriptor;
    EFI_USB_IO_GET_STRING_DESCRIPTOR       GetStringDescriptor;
    EFI_USB_IO_RESET                       Reset;
    EFI_USB_IO_GET_DEVICE                  GetDevice;
    EFI_USB_IO_GET_BUS                     GetBus;
    EFI_USB_IO_GET_PARENT                  GetParent;
    EFI_USB_IO_CONFIGURE                   Configure;
    EFI_USB_IO_PORT_RESET                  PortReset;
};

#endif /* EFIUSB_H */
