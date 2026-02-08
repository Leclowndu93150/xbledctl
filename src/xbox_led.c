/*
 * Xbox controller LED control via libusb + UsbDk.
 *
 * Bypasses the Windows xboxgip/dc1-controller driver stack by using
 * UsbDk as a USB filter driver, giving libusb direct access to the
 * bulk/interrupt OUT endpoint for sending GIP LED commands.
 */

#include "xbox_led.h"
#include "libusb.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* Supported Xbox controller USB Product IDs */
static const struct { uint16_t pid; const char *name; } XBOX_DEVICES[] = {
    { 0x02D1, "Xbox One Controller (1537)"       },
    { 0x02DD, "Xbox One Controller (1697)"       },
    { 0x02E3, "Xbox One Elite Controller"        },
    { 0x02EA, "Xbox One S Controller"            },
    { 0x0B00, "Xbox One Elite Series 2"          },
    { 0x0B05, "Xbox One Elite Series 2 v2"       },
    { 0x0B12, "Xbox Series X|S Controller"       },
    { 0x0B20, "Xbox Adaptive Controller"         },
};
#define XBOX_DEVICE_COUNT (sizeof(XBOX_DEVICES) / sizeof(XBOX_DEVICES[0]))

static const char *find_device_name(uint16_t pid)
{
    for (int i = 0; i < (int)XBOX_DEVICE_COUNT; i++)
        if (XBOX_DEVICES[i].pid == pid)
            return XBOX_DEVICES[i].name;
    return "Unknown Xbox Controller";
}

static bool is_xbox_pid(uint16_t pid)
{
    for (int i = 0; i < (int)XBOX_DEVICE_COUNT; i++)
        if (XBOX_DEVICES[i].pid == pid)
            return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void xbox_init(XboxController *ctrl)
{
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->claimed_iface = -1;
    ctrl->seq = 1;
}

bool xbox_open(XboxController *ctrl)
{
    xbox_close(ctrl);

    /* Init libusb */
    libusb_context *ctx = NULL;
    int rc = libusb_init(&ctx);
    if (rc != 0) {
        snprintf(ctrl->error, sizeof(ctrl->error),
                 "libusb_init failed: %s", libusb_strerror(rc));
        ctrl->last_err = XBOX_ERR_LIBUSB;
        return false;
    }
    ctrl->ctx = ctx;

    /* Enable UsbDk backend */
    rc = libusb_set_option(ctx, LIBUSB_OPTION_USE_USBDK);
    if (rc != 0 && !xbox_is_usbdk_installed()) {
        snprintf(ctrl->error, sizeof(ctrl->error),
                 "UsbDk is not installed");
        ctrl->last_err = XBOX_ERR_NO_USBDK;
        xbox_close(ctrl);
        return false;
    }

    /* Enumerate USB devices */
    libusb_device **dev_list = NULL;
    ssize_t count = libusb_get_device_list(ctx, &dev_list);
    if (count < 0) {
        snprintf(ctrl->error, sizeof(ctrl->error),
                 "libusb_get_device_list failed: %s", libusb_strerror((int)count));
        xbox_close(ctrl);
        return false;
    }
    ctrl->dev_list = (void **)dev_list;

    /* Find an Xbox controller */
    libusb_device *target = NULL;
    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;
        libusb_get_device_descriptor(dev_list[i], &desc);
        if (desc.idVendor == XBOX_VID && is_xbox_pid(desc.idProduct)) {
            target = dev_list[i];
            ctrl->vid = desc.idVendor;
            ctrl->pid = desc.idProduct;
            snprintf(ctrl->name, sizeof(ctrl->name), "%s", find_device_name(desc.idProduct));
            break;
        }
    }

    if (!target) {
        snprintf(ctrl->error, sizeof(ctrl->error), "No Xbox controller found");
        ctrl->last_err = XBOX_ERR_NO_DEVICE;
        return false;
    }

    /* Discover OUT endpoints from config descriptor */
    struct {
        int iface;
        uint8_t addr;
        int is_int;
    } out_eps[16];
    int out_ep_count = 0;

    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_config_descriptor(target, 0, &cfg) == 0) {
        for (int i = 0; i < cfg->bNumInterfaces; i++) {
            const struct libusb_interface *iface = &cfg->interface[i];
            for (int j = 0; j < iface->num_altsetting; j++) {
                const struct libusb_interface_descriptor *alt = &iface->altsetting[j];
                for (int k = 0; k < alt->bNumEndpoints; k++) {
                    const struct libusb_endpoint_descriptor *ep = &alt->endpoint[k];
                    if (!(ep->bEndpointAddress & 0x80) && out_ep_count < 16) {
                        out_eps[out_ep_count].iface = alt->bInterfaceNumber;
                        out_eps[out_ep_count].addr = ep->bEndpointAddress;
                        out_eps[out_ep_count].is_int = (ep->bmAttributes & 3) == 3;
                        out_ep_count++;
                    }
                }
            }
        }
        libusb_free_config_descriptor(cfg);
    }

    /* Open device */
    libusb_device_handle *handle = NULL;
    rc = libusb_open(target, &handle);
    if (rc != 0) {
        if (rc == LIBUSB_ERROR_NOT_SUPPORTED || rc == LIBUSB_ERROR_ACCESS) {
            snprintf(ctrl->error, sizeof(ctrl->error),
                     "UsbDk is not installed or needs a reboot");
            ctrl->last_err = XBOX_ERR_NO_USBDK;
        } else {
            snprintf(ctrl->error, sizeof(ctrl->error),
                     "libusb_open failed: %s", libusb_strerror(rc));
            ctrl->last_err = XBOX_ERR_OPEN_FAILED;
        }
        return false;
    }
    ctrl->handle = handle;

    libusb_set_auto_detach_kernel_driver(handle, 1);

    /* Claim the first interface with an OUT endpoint */
    for (int i = 0; i < out_ep_count; i++) {
        rc = libusb_claim_interface(handle, out_eps[i].iface);
        if (rc != 0) {
            libusb_detach_kernel_driver(handle, out_eps[i].iface);
            rc = libusb_claim_interface(handle, out_eps[i].iface);
        }
        if (rc == 0) {
            ctrl->claimed_iface = out_eps[i].iface;
            ctrl->out_ep = out_eps[i].addr;
            ctrl->out_ep_is_int = out_eps[i].is_int;
            ctrl->connected = true;
            ctrl->last_err = XBOX_OK;
            ctrl->error[0] = '\0';
            return true;
        }
    }

    snprintf(ctrl->error, sizeof(ctrl->error),
             "Could not claim any USB interface");
    ctrl->last_err = XBOX_ERR_CLAIM;
    xbox_close(ctrl);
    return false;
}

void xbox_close(XboxController *ctrl)
{
    libusb_device_handle *handle = (libusb_device_handle *)ctrl->handle;

    if (handle) {
        if (ctrl->claimed_iface >= 0) {
            libusb_release_interface(handle, ctrl->claimed_iface);
            ctrl->claimed_iface = -1;
        }
        libusb_close(handle);
        ctrl->handle = NULL;
    }

    ctrl->out_ep = 0;
    ctrl->connected = false;

    if (ctrl->dev_list) {
        libusb_free_device_list((libusb_device **)ctrl->dev_list, 1);
        ctrl->dev_list = NULL;
    }

    if (ctrl->ctx) {
        libusb_exit((libusb_context *)ctrl->ctx);
        ctrl->ctx = NULL;
    }
}

void xbox_cleanup(XboxController *ctrl)
{
    xbox_close(ctrl);
}

/* ------------------------------------------------------------------ */
/* GIP transport                                                       */
/* ------------------------------------------------------------------ */

static bool send_gip(XboxController *ctrl, uint8_t cmd, uint8_t flags,
                     const uint8_t *payload, int payload_len)
{
    if (!ctrl->connected || !ctrl->handle)
        return false;

    uint8_t packet[64];
    int pkt_len = 4 + payload_len;
    if (pkt_len > (int)sizeof(packet))
        return false;

    packet[0] = cmd;
    packet[1] = flags;
    packet[2] = ctrl->seq;
    packet[3] = (uint8_t)payload_len;
    memcpy(packet + 4, payload, payload_len);

    ctrl->seq = (ctrl->seq % 255) + 1;

    int transferred = 0;
    int rc;
    libusb_device_handle *handle = (libusb_device_handle *)ctrl->handle;

    if (ctrl->out_ep_is_int)
        rc = libusb_interrupt_transfer(handle, ctrl->out_ep, packet, pkt_len, &transferred, 3000);
    else
        rc = libusb_bulk_transfer(handle, ctrl->out_ep, packet, pkt_len, &transferred, 3000);

    if (rc != 0) {
        snprintf(ctrl->error, sizeof(ctrl->error),
                 "Transfer failed: %s", libusb_strerror(rc));
        ctrl->connected = false;
        return false;
    }

    return transferred > 0;
}

/* ------------------------------------------------------------------ */
/* Public commands                                                     */
/* ------------------------------------------------------------------ */

bool xbox_set_led(XboxController *ctrl, uint8_t mode, uint8_t brightness)
{
    if (brightness > LED_BRIGHTNESS_MAX)
        brightness = LED_BRIGHTNESS_MAX;
    uint8_t payload[] = { 0x00, mode, brightness };
    return send_gip(ctrl, GIP_CMD_LED, GIP_OPT_INTERNAL, payload, sizeof(payload));
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

bool xbox_is_usbdk_installed(void)
{
#ifdef _WIN32
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat_s(path, MAX_PATH, "\\UsbDkHelper.dll");
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    return true;
#endif
}
