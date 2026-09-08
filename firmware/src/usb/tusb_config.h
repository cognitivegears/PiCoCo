#pragma once
/* The Pico SDK's tinyusb_device/tinyusb_board CMake targets (pico_add_library)
 * do not define CFG_TUSB_MCU/CFG_TUSB_OS themselves -- that only happens via
 * TinyUSB's own family_support.cmake, which we don't use. So we set them here. */
#define CFG_TUSB_MCU          OPT_MCU_RP2040   /* RP2350 uses the same USB IP as RP2040 in TinyUSB's port */
#define CFG_TUSB_OS           OPT_OS_PICO
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define CFG_TUD_ENABLED       1
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_CDC           2
#define CFG_TUD_MSC           1
#define CFG_TUD_CDC_RX_BUFSIZE 512
#define CFG_TUD_CDC_TX_BUFSIZE 512
#define CFG_TUD_MSC_EP_BUFSIZE 512
