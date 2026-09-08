/* Composite USB device: CDC0 (DriveWire bridge), CDC1 (console), MSC (Task 4
 * fills in the actual storage; here it always reports "no medium"). */
#include "tusb.h"
#include "pico/unique_id.h"
#include <string.h>

/* PID 0x2E8A:0x1042 is a placeholder pending real VID/PID assignment/policy. */
#define USB_VID 0x2E8A
#define USB_PID 0x1042
#define USB_BCD 0x0200

enum {
    ITF_NUM_CDC0 = 0, ITF_NUM_CDC0_DATA,
    ITF_NUM_CDC1, ITF_NUM_CDC1_DATA,
    ITF_NUM_MSC,
    ITF_NUM_TOTAL
};

#define EPNUM_CDC0_NOTIF 0x81
#define EPNUM_CDC0_OUT   0x02
#define EPNUM_CDC0_IN    0x82
#define EPNUM_CDC1_NOTIF 0x83
#define EPNUM_CDC1_OUT   0x04
#define EPNUM_CDC1_IN    0x84
#define EPNUM_MSC_OUT    0x05
#define EPNUM_MSC_IN     0x85

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

/* ---- Device descriptor ---- */

static tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

/* ---- Configuration descriptor ---- */

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC0, 4, EPNUM_CDC0_NOTIF, 8, EPNUM_CDC0_OUT, EPNUM_CDC0_IN, 64),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC1, 5, EPNUM_CDC1_NOTIF, 8, EPNUM_CDC1_OUT, EPNUM_CDC1_IN, 64),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 6, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

/* ---- String descriptors ---- */

enum {
    STRID_LANGID = 0, STRID_MANUFACTURER, STRID_PRODUCT, STRID_SERIAL,
    STRID_ITF_CDC0, STRID_ITF_CDC1, STRID_ITF_MSC,
};

static char const *string_desc_arr[] = {
    NULL,                      /* 0: langid, handled separately */
    "PiCoCo",                  /* 1: manufacturer */
    "PiCoCo Cartridge",        /* 2: product */
    NULL,                      /* 3: serial, filled from unique board id */
    "PiCoCo DriveWire",        /* 4: CDC0 */
    "PiCoCo Console",          /* 5: CDC1 */
    "PiCoCo Storage",          /* 6: MSC */
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t chr_count;

    if (index == STRID_LANGID) {
        _desc_str[1] = 0x0409; /* English (US) */
        chr_count = 1;
    } else if (index == STRID_SERIAL) {
        char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        pico_get_unique_board_id_string(id, sizeof(id));
        chr_count = strlen(id);
        for (size_t i = 0; i < chr_count; i++) _desc_str[1 + i] = (uint16_t)id[i];
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;
        const char *str = string_desc_arr[index];
        if (!str) return NULL;
        chr_count = strlen(str);
        size_t max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1;
        if (chr_count > max_count) chr_count = max_count;
        for (size_t i = 0; i < chr_count; i++) _desc_str[1 + i] = (uint16_t)str[i];
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

/* ---- MSC stubs: no medium until Task 4 ---- */

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id, "PiCoCo", 6);
    memcpy(product_id, "Flash FS", 8);
    memcpy(product_rev, "1.0", 3);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    return false; /* ponytail: no medium until Task 4 wires FatFS in */
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = 0;
    *block_size = 512;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)lun; (void)lba; (void)offset; (void)buffer; (void)bufsize;
    return -1;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)lun; (void)lba; (void)offset; (void)buffer; (void)bufsize;
    return -1;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)lun; (void)scsi_cmd; (void)buffer; (void)bufsize;
    return -1;
}
