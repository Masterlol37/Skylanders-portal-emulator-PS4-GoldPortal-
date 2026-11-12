// libSceUsbd is Sony's build of libusb-1.0, so its exports take the same
// arguments and use the same structure layouts as libusb.
// These mirrors are only used for the *fake* portal; real devices are passed
// through to the original functions untouched.
#pragma once
#include <stdint.h>

typedef struct SceUsbdDevice       SceUsbdDevice;        // opaque
typedef struct SceUsbdDeviceHandle SceUsbdDeviceHandle;  // opaque

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} SceUsbdDeviceDescriptor;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
    uint8_t  bRefresh;
    uint8_t  bSynchAddress;
    const unsigned char *extra;
    int      extra_length;
} SceUsbdEndpointDescriptor;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
    const SceUsbdEndpointDescriptor *endpoint;
    const unsigned char *extra;
    int      extra_length;
} SceUsbdInterfaceDescriptor;

typedef struct {
    const SceUsbdInterfaceDescriptor *altsetting;
    int num_altsetting;
} SceUsbdInterface;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  MaxPower;
    const SceUsbdInterface *interface;
    const unsigned char *extra;
    int      extra_length;
} SceUsbdConfigDescriptor;

struct SceUsbdTransfer;
typedef void (*SceUsbdTransferCallback)(struct SceUsbdTransfer *);

typedef struct SceUsbdTransfer {
    SceUsbdDeviceHandle *dev_handle;
    uint8_t  flags;
    uint8_t  endpoint;
    uint8_t  type;
    unsigned int timeout;
    int      status;
    int      length;
    int      actual_length;
    SceUsbdTransferCallback callback;
    void    *user_data;
    unsigned char *buffer;
    int      num_iso_packets;
} SceUsbdTransfer;

#define USBD_TRANSFER_TYPE_CONTROL   0
#define USBD_TRANSFER_TYPE_INTERRUPT 3
#define USBD_TRANSFER_COMPLETED      0
#define USBD_TRANSFER_CANCELLED      3
#define USBD_TRANSFER_FREE_TRANSFER  0x04
#define USBD_CONTROL_SETUP_SIZE      8

#define USBD_ERROR_NOT_FOUND   (-5)
#define USBD_ERROR_NOT_SUPPORTED (-12)
