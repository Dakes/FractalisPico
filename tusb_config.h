#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

// TinyUSB for UsbDevice.cpp: the serial port of the log (stdio), the USB drive and picotool's reset interface

#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE)
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC 1
#define CFG_TUD_MSC 1
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
// The reset interface has its own driver (pico_usb_reset)
#define CFG_TUD_VENDOR 0

#define CFG_TUD_CDC_RX_BUFSIZE 64
#define CFG_TUD_CDC_TX_BUFSIZE 256
#define CFG_TUD_CDC_EP_BUFSIZE 64

// A sector per transfer
#define CFG_TUD_MSC_EP_BUFSIZE 512

#endif // TUSB_CONFIG_H
