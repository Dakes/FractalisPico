// The USB device on the Pico: the serial port of the log (stdio), the drive of UsbDrive.hpp and the interface that
// lets picotool reboot the Pico for flashing. Descriptors and TinyUSB callbacks, the drive itself is in UsbDrive.cpp.
#include "UsbDrive.hpp"
#include "pico/unique_id.h"
#include "pico/usb_reset.h"
#include "tusb.h"
#include <cstring>

namespace {

constexpr uint16_t VID = 0x2E8A;  // Raspberry Pi
constexpr uint16_t PID = 0x0009;  // Pico SDK serial port, as before (picotool finds it)

enum : uint8_t { ITF_CDC = 0, ITF_CDC_DATA, ITF_MSC, ITF_RESET, ITF_COUNT };
static_assert(ITF_RESET == PICO_USB_RESET_MS_OS_20_DESCRIPTOR_ITF, "picotool finds the reset interface by its number");

enum : uint8_t { STR_LANGUAGE = 0, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL, STR_CDC, STR_MSC, STR_RESET };

constexpr uint8_t EP_CDC_NOTIFY = 0x81;
constexpr uint8_t EP_CDC_OUT = 0x02;
constexpr uint8_t EP_CDC_IN = 0x82;
constexpr uint8_t EP_MSC_OUT = 0x03;
constexpr uint8_t EP_MSC_IN = 0x83;

const tusb_desc_device_t DEVICE = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    // 2.1 for the Microsoft OS 2.0 descriptor of the reset interface (Windows needs no driver for it then)
    .bcdUSB = 0x0210,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = VID,
    .idProduct = PID,
    // Another set of interfaces than the plain serial port: computers that remember devices see a new one
    .bcdDevice = 0x0200,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

constexpr int CONFIG_LENGTH = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN + TUD_RPI_RESET_DESC_LEN;
const uint8_t CONFIG[CONFIG_LENGTH] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LENGTH, 0, 250),
    TUD_CDC_DESCRIPTOR(ITF_CDC, STR_CDC, EP_CDC_NOTIFY, 8, EP_CDC_OUT, EP_CDC_IN, 64),
    TUD_MSC_DESCRIPTOR(ITF_MSC, STR_MSC, EP_MSC_OUT, EP_MSC_IN, 64),
    TUD_RPI_RESET_DESCRIPTOR(ITF_RESET, STR_RESET),
};

char serial_text[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

const char* const STRINGS[] = {
    nullptr,  // language, see below
    "Raspberry Pi",
    "FractalisPico",
    serial_text,
    "Fractalis log",
    "Fractalis drive",
    "Reset",
};

}  // namespace

const uint8_t* tud_descriptor_device_cb(void) {
    return reinterpret_cast<const uint8_t*>(&DEVICE);
}

const uint8_t* tud_descriptor_configuration_cb(uint8_t) {
    return CONFIG;
}

const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t) {
    static uint16_t text[32];
    if (!serial_text[0]) pico_get_unique_board_id_string(serial_text, sizeof(serial_text));
    int length;
    if (index == STR_LANGUAGE) {
        text[1] = 0x0409;  // English
        length = 1;
    } else {
        if (index >= sizeof(STRINGS) / sizeof(STRINGS[0])) return nullptr;
        const char* s = STRINGS[index];
        for (length = 0; length < 31 && s[length]; ++length) text[1 + length] = static_cast<uint8_t>(s[length]);
    }
    text[0] = static_cast<uint16_t>((TUSB_DESC_STRING << 8) | (2 * length + 2));
    return text;
}

// ---- The drive ----

void tud_msc_inquiry_cb(uint8_t, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    memcpy(vendor_id, "Pico    ", 8);
    memcpy(product_id, "Fractalis drive ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    switch (usb_drive::status()) {
        case usb_drive::Status::READY:
            return true;
        case usb_drive::Status::CHANGED:
            // Medium may have changed: the computer reads the drive again
            tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00);
            return false;
        default:
            // Medium not present
            tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
            return false;
    }
}

void tud_msc_capacity_cb(uint8_t, uint32_t* block_count, uint16_t* block_size) {
    *block_count = usb_drive::SECTORS;
    *block_size = usb_drive::SECTOR_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t, uint8_t, bool start, bool load_eject) {
    if (load_eject && !start) usb_drive::eject();
    return true;
}

int32_t tud_msc_read10_cb(uint8_t, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    return usb_drive::read(lba, offset, buffer, bufsize);
}

int32_t tud_msc_write10_cb(uint8_t, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    return usb_drive::write(lba, offset, buffer, bufsize);
}

bool tud_msc_is_writable_cb(uint8_t) {
    return true;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void*, uint16_t) {
    switch (scsi_cmd[0]) {
        case 0x35:  // synchronize cache: nothing is cached
            return 0;
        default:
            // Invalid command operation code
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
            return -1;
    }
}
