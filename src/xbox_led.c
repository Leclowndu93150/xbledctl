#include "xbox_led.h"

#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define GIP_REENUMERATE 0x40001CD0

#pragma pack(push, 1)
typedef struct {
    uint64_t deviceId;
    uint8_t  commandId;
    uint8_t  clientFlags;
    uint8_t  sequence;
    uint8_t  unknown1;
    uint32_t length;
    uint32_t unknown2;
} GipHeader;
#pragma pack(pop)

void xbox_init(XboxController *ctrl)
{
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->seq = 1;
}

static bool discover_device(XboxController *ctrl)
{
    HANDLE h = (HANDLE)ctrl->handle;
    DWORD bytes = 0;
    DeviceIoControl(h, GIP_REENUMERATE, NULL, 0, NULL, 0, &bytes, NULL);

    uint8_t buf[4096];
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = (HANDLE)ctrl->read_event;

    for (int i = 0; i < 5; i++) {
        memset(buf, 0, sizeof(buf));
        ResetEvent(ov.hEvent);
        DWORD rd = 0;
        BOOL ok = ReadFile(h, buf, sizeof(buf), &rd, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            DWORD wait = WaitForSingleObject(ov.hEvent, 300);
            if (wait == WAIT_TIMEOUT) {
                CancelIo(h);
                WaitForSingleObject(ov.hEvent, 100);
                continue;
            }
            GetOverlappedResult(h, &ov, &rd, FALSE);
        }
        if (rd < sizeof(GipHeader))
            continue;

        GipHeader *hdr = (GipHeader *)buf;
        if (hdr->commandId == 0x01 || hdr->commandId == 0x02) {
            ctrl->device_id = hdr->deviceId;
            return true;
        }
    }
    return false;
}

bool xbox_open(XboxController *ctrl)
{
    xbox_close(ctrl);

    HANDLE h = CreateFileW(L"\\\\.\\XboxGIP",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        NULL);

    if (h == INVALID_HANDLE_VALUE) {
        snprintf(ctrl->error, sizeof(ctrl->error),
                 "Cannot open XboxGIP driver (error %lu)", GetLastError());
        ctrl->last_err = XBOX_ERR_OPEN_FAILED;
        return false;
    }
    ctrl->handle = h;
    ctrl->read_event = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (!discover_device(ctrl)) {
        snprintf(ctrl->error, sizeof(ctrl->error), "No Xbox controller found");
        ctrl->last_err = XBOX_ERR_NO_DEVICE;
        xbox_close(ctrl);
        return false;
    }

    ctrl->connected = true;
    ctrl->last_err = XBOX_OK;
    ctrl->error[0] = '\0';
    return true;
}

void xbox_close(XboxController *ctrl)
{
    if (ctrl->read_event) {
        CloseHandle((HANDLE)ctrl->read_event);
        ctrl->read_event = NULL;
    }
    if (ctrl->handle) {
        CloseHandle((HANDLE)ctrl->handle);
        ctrl->handle = NULL;
    }
    ctrl->device_id = 0;
    ctrl->connected = false;
}

void xbox_cleanup(XboxController *ctrl)
{
    xbox_close(ctrl);
}

bool xbox_set_led(XboxController *ctrl, uint8_t mode, uint8_t brightness)
{
    if (!ctrl->connected || !ctrl->handle)
        return false;

    if (brightness > LED_BRIGHTNESS_MAX)
        brightness = LED_BRIGHTNESS_MAX;

    uint8_t payload[] = { 0x00, mode, brightness };
    uint8_t pkt[sizeof(GipHeader) + sizeof(payload)];
    memset(pkt, 0, sizeof(pkt));

    GipHeader *hdr = (GipHeader *)pkt;
    hdr->deviceId = ctrl->device_id;
    hdr->commandId = GIP_CMD_LED;
    hdr->clientFlags = GIP_OPT_INTERNAL;
    hdr->sequence = ctrl->seq;
    hdr->length = sizeof(payload);
    memcpy(pkt + sizeof(GipHeader), payload, sizeof(payload));

    ctrl->seq = (ctrl->seq % 255) + 1;

    DWORD written = 0;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    BOOL ok = WriteFile((HANDLE)ctrl->handle, pkt, sizeof(pkt), &written, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(ov.hEvent, 2000);
        GetOverlappedResult((HANDLE)ctrl->handle, &ov, &written, FALSE);
    }
    CloseHandle(ov.hEvent);

    if (written != sizeof(pkt)) {
        snprintf(ctrl->error, sizeof(ctrl->error),
                 "Write failed (error %lu)", GetLastError());
        ctrl->last_err = XBOX_ERR_SEND;
        return false;
    }

    return true;
}

bool xbox_set_brightness(XboxController *ctrl, uint8_t brightness)
{
    if (brightness == 0)
        return xbox_set_led(ctrl, LED_MODE_OFF, 0);
    return xbox_set_led(ctrl, LED_MODE_ON, brightness);
}

bool xbox_led_off(XboxController *ctrl)
{
    return xbox_set_led(ctrl, LED_MODE_OFF, 0);
}
