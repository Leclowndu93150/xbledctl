# xbledctl

Control the Xbox button LED brightness on Xbox One and Series X|S controllers from Windows.

This is the first tool to achieve user-mode LED control on Xbox controllers on Windows. Microsoft's driver stack provides no public API for this. xbledctl bypasses the driver entirely using [libusb](https://libusb.info/) + [UsbDk](https://github.com/daynix/UsbDk) to write [GIP](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/e7c90904-5e21-426e-b9ad-d82adeee0dbc) LED packets directly to the USB bulk endpoint.

## Features

- Set LED brightness (0-50)
- LED modes: solid, blink (fast/normal/slow), fade (fast/slow), off
- GPU-accelerated GUI (Dear ImGui + DirectX 11)
- ~400KB executable, no runtime dependencies
- Works alongside the stock Xbox driver. No Zadig, no driver replacement.
- Supports Xbox One, One S, One Elite, Elite Series 2, Series X|S, Adaptive Controller
- Auto-applies saved settings when the controller is plugged in
- Starts with Windows and minimizes to system tray (configurable)

## Install

1. Download the [latest release](https://github.com/Leclowndu93150/xbledctl/releases) and extract the zip
2. Run `xbledctl.exe`
3. If UsbDk is not installed, the app will prompt you to download and install it
4. Reboot after installing UsbDk (the filter driver needs a restart to attach to USB devices)
5. Run `xbledctl.exe` again and plug in your controller via USB

That's it.

## How It Works

Xbox controllers use [GIP (Game Input Protocol)](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/e7c90904-5e21-426e-b9ad-d82adeee0dbc) over USB. The LED is controlled by a 7-byte command:

```
Byte 0: 0x0A        Command ID (LED)
Byte 1: 0x20        Flags (internal)
Byte 2: <seq>       Sequence number
Byte 3: 0x03        Payload length
Byte 4: 0x00        Sub-command
Byte 5: <mode>      LED mode (0x00=off, 0x01=on, 0x02-0x09=animations)
Byte 6: <brightness> 0-50
```

On Linux, the [xone](https://github.com/medusalix/xone) driver sends this natively. On Windows, the `dc1-controller` / `xboxgip` driver stack provides no user-mode API to send arbitrary GIP commands. Every official API either silently drops the data or is read-only.

[UsbDk](https://github.com/daynix/UsbDk) is a USB filter driver that lets libusb access the device *without replacing the stock driver*. XInput, Steam Input, and everything else continues to work normally.

See [docs/RESEARCH.md](docs/RESEARCH.md) for the full technical writeup of the reverse-engineering process.

## Why does it run in the background?

Xbox controllers don't store LED settings in firmware. Every time the controller is unplugged, powered off, or reconnected, the LED resets to its default brightness. There's no way around this at the hardware level.

To keep your preferred brightness without having to re-apply it manually every time, xbledctl can start with Windows and sit in the system tray. When it detects a controller being plugged in, it automatically re-applies your saved LED settings. Both options are enabled by default and can be toggled in the app.

## Supported Controllers

| Controller | USB PID | Tested |
|---|---|---|
| Xbox Series X\|S | `0x0B12` | Yes |
| Xbox One S | `0x02EA` | Should work |
| Xbox One (Model 1537) | `0x02D1` | Should work |
| Xbox One (Model 1697) | `0x02DD` | Should work |
| Xbox One Elite | `0x02E3` | Should work |
| Xbox One Elite Series 2 | `0x0B00` | Should work |
| Xbox Adaptive Controller | `0x0B20` | Should work |

All Xbox controllers that use GIP over USB should work. Bluetooth is not supported (the LED is not controllable over Bluetooth at the firmware level).

## Building from Source

### Requirements

- Windows 10/11 (64-bit)
- Visual Studio 2022 with C++ Desktop workload
- CMake (bundled with VS2022)

### Build

Open a **x64 Native Tools Command Prompt for VS 2022** and run:

```
cd path\to\xbledctl
build.bat
```

The output is `build\xbledctl.exe` + `build\libusb-1.0.dll`.

### Dependencies

All dependencies are vendored in the repository:

- **[Dear ImGui](https://github.com/ocornut/imgui)** v1.91.8 (MIT) - `imgui/`
- **[libusb](https://github.com/libusb/libusb)** v1.0.29 (LGPL-2.1) - `lib/`

DirectX 11 and Win32 APIs are part of the Windows SDK.

## Troubleshooting

**"UsbDk is not installed" dialog on startup**
- Download and install UsbDk from [GitHub releases](https://github.com/daynix/UsbDk/releases). Reboot after installing.

**No controller found**
- Make sure the controller is plugged in via USB (not Bluetooth)
- Try clicking Refresh in the app

**LED commands sent but nothing changes**
- Unplug and replug the USB cable
- Try a different brightness value to confirm the change is visible

## License

MIT

## Acknowledgments

- [medusalix/xone](https://github.com/medusalix/xone) - GIP protocol reference and LED packet format
- [libsdl-org/SDL](https://github.com/libsdl-org/SDL) - Xbox One HIDAPI driver source (LED command constants)
- [TheNathannator](https://github.com/TheNathannator) - GIP protocol notes and Windows driver interface documentation
- [daynix/UsbDk](https://github.com/daynix/UsbDk) - the USB filter driver that makes this possible on Windows
- [libusb](https://libusb.info/) - cross-platform USB access
- [ocornut/imgui](https://github.com/ocornut/imgui) - Dear ImGui
