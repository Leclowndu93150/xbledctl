#ifndef XBOX_LED_H
#define XBOX_LED_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GIP_CMD_LED        0x0A
#define GIP_OPT_INTERNAL   0x20

#define LED_MODE_OFF           0x00
#define LED_MODE_ON            0x01
#define LED_MODE_BLINK_FAST    0x02
#define LED_MODE_BLINK_SLOW    0x03
#define LED_MODE_BLINK_CHARGE  0x04
#define LED_MODE_FADE_SLOW     0x08
#define LED_MODE_FADE_FAST     0x09
#define LED_MODE_RAMP_TO_LEVEL 0x0D

#define XBOX_OK              0
#define XBOX_ERR_NO_DEVICE   1
#define XBOX_ERR_OPEN_FAILED 3
#define XBOX_ERR_SEND        5

#define LED_BRIGHTNESS_MIN     0
#define LED_BRIGHTNESS_MAX     47
#define LED_BRIGHTNESS_DEFAULT 20

typedef struct {
    void    *handle;
    void    *read_event;
    uint64_t device_id;
    uint8_t  seq;
    bool     connected;
    int      last_err;
    char     error[128];
} XboxController;

void xbox_init(XboxController *ctrl);
bool xbox_open(XboxController *ctrl);
void xbox_close(XboxController *ctrl);
void xbox_cleanup(XboxController *ctrl);
bool xbox_set_led(XboxController *ctrl, uint8_t mode, uint8_t brightness);
bool xbox_set_brightness(XboxController *ctrl, uint8_t brightness);
bool xbox_led_off(XboxController *ctrl);

#ifdef __cplusplus
}
#endif

#endif
