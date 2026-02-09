#ifndef XBOX_LED_H
#define XBOX_LED_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Microsoft USB Vendor ID */
#define XBOX_VID 0x045E

/* GIP protocol */
#define GIP_CMD_LED        0x0A
#define GIP_OPT_INTERNAL   0x20

/* LED patterns (Table 42, MS-GIPUSB + undocumented) */
#define LED_MODE_OFF           0x00
#define LED_MODE_ON            0x01
#define LED_MODE_BLINK_FAST    0x02
#define LED_MODE_BLINK_SLOW    0x03
#define LED_MODE_BLINK_CHARGE  0x04
#define LED_MODE_FADE_SLOW     0x08
#define LED_MODE_FADE_FAST     0x09
#define LED_MODE_RAMP_TO_LEVEL 0x0D

/* Error codes */
#define XBOX_OK              0
#define XBOX_ERR_NO_DEVICE   1
#define XBOX_ERR_NO_USBDK    2
#define XBOX_ERR_OPEN_FAILED 3
#define XBOX_ERR_CLAIM       4
#define XBOX_ERR_LIBUSB      5

#define LED_BRIGHTNESS_MIN     0
#define LED_BRIGHTNESS_MAX     47
#define LED_BRIGHTNESS_DEFAULT 20

typedef struct {
    void    *ctx;            /* libusb_context*          */
    void    *handle;         /* libusb_device_handle*    */
    void   **dev_list;       /* libusb_device**          */
    int      claimed_iface;  /* -1 = none                */
    uint8_t  out_ep;         /* endpoint address         */
    int      out_ep_is_int;  /* 1 = interrupt, 0 = bulk  */
    uint8_t  seq;
    uint16_t vid;
    uint16_t pid;
    char     name[64];
    bool     connected;
    int      last_err;       /* XBOX_ERR_* code    */
    char     error[128];
} XboxController;

void xbox_init(XboxController *ctrl);
bool xbox_open(XboxController *ctrl);
void xbox_close(XboxController *ctrl);
void xbox_cleanup(XboxController *ctrl);
bool xbox_set_led(XboxController *ctrl, uint8_t mode, uint8_t brightness);
bool xbox_set_brightness(XboxController *ctrl, uint8_t brightness);
bool xbox_led_off(XboxController *ctrl);
bool xbox_is_usbdk_installed(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_LED_H */
