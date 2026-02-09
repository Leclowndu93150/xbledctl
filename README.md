# xbledctl

Control the Xbox button LED brightness on Xbox One and Series X|S controllers from Windows.

This is the first tool to achieve user-mode LED control on Xbox controllers on Windows. Microsoft's driver stack provides no public API for this. xbledctl talks directly to the `xboxgip.sys` kernel driver through its `\\.\XboxGIP` device interface, sending [GIP](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/e7c90904-5e21-426e-b9ad-d82adeee0dbc) LED commands without detaching or replacing any drivers.

## Features

- Set LED brightness (0-47%, per [MS-GIPUSB](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/e7c90904-5e21-426e-b9ad-d82adeee0dbc) spec)
- LED modes: steady, fast blink, slow blink, charging blink, fade (slow/fast), fade in, off
- Supports Xbox One, One S, One Elite, Elite Series 2, Series X|S, Adaptive Controller
- Auto-applies saved settings when the controller is plugged in
- Starts with Windows and minimizes to system tray (configurable)

## Install

1. Download the [latest release](https://github.com/Leclowndu93150/xbledctl/releases) and extract the zip
2. Run `xbledctl.exe`
3. Plug in your controller via USB

## How It Works

Xbox controllers use [GIP (Game Input Protocol)](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/e7c90904-5e21-426e-b9ad-d82adeee0dbc) over USB. The LED is controlled by command `0x0A` with a 3-byte payload.

xbledctl sends commands through the `\\.\XboxGIP` device interface exposed by Microsoft's own `xboxgip.sys` driver. The protocol uses a 20-byte header followed by the payload:

```
GipHeader (20 bytes, packed):
  Offset 0:  uint64  deviceId       (from device announce message)
  Offset 8:  uint8   commandId      (0x0A for LED)
  Offset 9:  uint8   clientFlags    (0x20 = internal)
  Offset 10: uint8   sequence       (1-255)
  Offset 11: uint8   unknown1       (0)
  Offset 12: uint32  length         (payload size = 3)
  Offset 16: uint32  unknown2       (0)

LED Payload (3 bytes):
  Byte 0: 0x00        Sub-command (guide button LED)
  Byte 1: <mode>      0x00=off, 0x01=on, 0x02=fast blink, etc.
  Byte 2: <intensity> 0-47
```

The sequence to send a command:
1. Open `\\.\XboxGIP` with `CreateFileW` (overlapped I/O)
2. Send IOCTL `0x40001CD0` to trigger device re-enumeration
3. `ReadFile` in a loop to receive device announce messages (command `0x01` or `0x02`)
4. Extract `deviceId` from the announce message header
5. `WriteFile` the 23-byte packet (20-byte header + 3-byte payload)

This is fundamentally different from the raw USB GIP protocol (which uses a 4-byte header on the wire). The `\\.\XboxGIP` interface wraps commands in its own 20-byte header that includes a `deviceId` field for routing to the correct controller.

See [docs/RESEARCH.md](docs/RESEARCH.md) for the full technical writeup of every approach we tried, what failed, and why.

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

The output is `build\xbledctl.exe`.

### Dependencies

All dependencies are vendored in the repository:

- **[Dear ImGui](https://github.com/ocornut/imgui)** v1.91.8 (MIT) - `imgui/`

DirectX 11 and Win32 APIs are part of the Windows SDK.

## Troubleshooting

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
- [ocornut/imgui](https://github.com/ocornut/imgui) - Dear ImGui
