/*
 * xbledctl - Xbox Controller LED Control
 *
 * Dear ImGui + DirectX 11 GUI for controlling the Xbox button LED
 * brightness and animation mode via libusb + UsbDk.
 */

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "gui_theme.h"

#include <d3d11.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <dbt.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "shlwapi.lib")

extern "C" {
#include "xbox_led.h"
}

/* ------------------------------------------------------------------ */
/* DirectX 11 globals                                                  */
/* ------------------------------------------------------------------ */

static ID3D11Device           *g_pd3dDevice          = nullptr;
static ID3D11DeviceContext    *g_pd3dDeviceContext    = nullptr;
static IDXGISwapChain         *g_pSwapChain          = nullptr;
static bool                    g_SwapChainOccluded   = false;
static UINT                    g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView *g_mainRenderTargetView = nullptr;

static bool CreateDeviceD3D(HWND hWnd);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

/* ------------------------------------------------------------------ */
/* Config file (settings persistence)                                  */
/* ------------------------------------------------------------------ */

static char g_config_path[MAX_PATH] = {};

static void InitConfigPath()
{
    GetModuleFileNameA(nullptr, g_config_path, MAX_PATH);
    PathRemoveFileSpecA(g_config_path);
    strcat_s(g_config_path, "\\xbledctl.ini");
}

static void SaveConfig(int brightness, int mode_idx, bool start_with_windows, bool minimize_to_tray)
{
    char buf[512];
    snprintf(buf, sizeof(buf),
        "[xbledctl]\nbrightness=%d\nmode=%d\nstart_with_windows=%d\nminimize_to_tray=%d\n",
        brightness, mode_idx, start_with_windows ? 1 : 0, minimize_to_tray ? 1 : 0);
    FILE *f = nullptr;
    fopen_s(&f, g_config_path, "w");
    if (f) { fputs(buf, f); fclose(f); }
}

static void LoadConfig(int *brightness, int *mode_idx, bool *start_with_windows, bool *minimize_to_tray)
{
    *brightness = LED_BRIGHTNESS_DEFAULT;
    *mode_idx = 1;
    *start_with_windows = true;
    *minimize_to_tray = true;

    FILE *f = nullptr;
    fopen_s(&f, g_config_path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int val;
        if (sscanf_s(line, "brightness=%d", &val) == 1)
            *brightness = (val >= 0 && val <= LED_BRIGHTNESS_MAX) ? val : LED_BRIGHTNESS_DEFAULT;
        else if (sscanf_s(line, "mode=%d", &val) == 1)
            *mode_idx = (val >= 0 && val <= 6) ? val : 1;
        else if (sscanf_s(line, "start_with_windows=%d", &val) == 1)
            *start_with_windows = (val != 0);
        else if (sscanf_s(line, "minimize_to_tray=%d", &val) == 1)
            *minimize_to_tray = (val != 0);
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Auto-start with Windows (registry)                                  */
/* ------------------------------------------------------------------ */

static const char *AUTOSTART_KEY = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char *AUTOSTART_VAL = "xbledctl";

static void SetAutoStart(bool enable)
{
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, AUTOSTART_KEY, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return;

    if (enable) {
        char exe_path[MAX_PATH];
        GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        /* Add --minimized flag so it starts in tray */
        char cmd[MAX_PATH + 32];
        snprintf(cmd, sizeof(cmd), "\"%s\" --minimized", exe_path);
        RegSetValueExA(hKey, AUTOSTART_VAL, 0, REG_SZ, (BYTE *)cmd, (DWORD)strlen(cmd) + 1);
    } else {
        RegDeleteValueA(hKey, AUTOSTART_VAL);
    }
    RegCloseKey(hKey);
}

static bool IsAutoStartEnabled()
{
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, AUTOSTART_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD type, size = 0;
    bool exists = RegQueryValueExA(hKey, AUTOSTART_VAL, nullptr, &type, nullptr, &size) == ERROR_SUCCESS;
    RegCloseKey(hKey);
    return exists;
}

/* ------------------------------------------------------------------ */
/* System tray                                                         */
/* ------------------------------------------------------------------ */

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_SHOW 1001
#define ID_TRAY_QUIT 1002

static NOTIFYICONDATAW g_nid = {};
static HWND g_hwnd = nullptr;
static bool g_minimized_to_tray = false;

static void AddTrayIcon(HWND hwnd)
{
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Xbox LED Control");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void MinimizeToTray(HWND hwnd)
{
    ShowWindow(hwnd, SW_HIDE);
    g_minimized_to_tray = true;
}

static void RestoreFromTray(HWND hwnd)
{
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    g_minimized_to_tray = false;
}

/* ------------------------------------------------------------------ */
/* App state                                                           */
/* ------------------------------------------------------------------ */

static XboxController g_ctrl;
static int            g_brightness = LED_BRIGHTNESS_DEFAULT;
static int            g_mode_idx   = 1; /* Steady */
static char           g_status[128] = "Plug in your controller with a USB cable";
static ImVec4         g_status_color;
static bool           g_need_usbdk = false;
static bool           g_start_with_windows = true;
static bool           g_minimize_to_tray = true;
static bool           g_device_change_pending = false;
static DWORD          g_device_change_tick = 0;  /* GetTickCount() when event fired */
static bool           g_device_removed = false;
static DWORD          g_usb_cooldown_until = 0;  /* ignore device events until this tick */

static const ImVec4 COL_WARN    = ImVec4(0.902f, 0.706f, 0.157f, 1.0f); /* (230,180,40) */

struct ModeEntry {
    const char *label;
    uint8_t     value;
};

static const ModeEntry MODES[] = {
    { "Off",        LED_MODE_OFF        },
    { "Steady",     LED_MODE_ON         },
    { "Blink Fast", LED_MODE_BLINK_FAST },
    { "Blink",      LED_MODE_BLINK      },
    { "Blink Slow", LED_MODE_BLINK_SLOW },
    { "Fade Slow",  LED_MODE_FADE_SLOW  },
    { "Fade Fast",  LED_MODE_FADE_FAST  },
};
static const int MODE_COUNT = sizeof(MODES) / sizeof(MODES[0]);

/* Colors */
static const ImVec4 COL_SUCCESS  = ImVec4(0.157f, 0.784f, 0.314f, 1.0f);
static const ImVec4 COL_ERROR    = ImVec4(0.863f, 0.235f, 0.235f, 1.0f);
static const ImVec4 COL_DIM      = ImVec4(0.549f, 0.549f, 0.588f, 1.0f);
static const ImVec4 COL_TEXT     = ImVec4(0.902f, 0.902f, 0.922f, 1.0f);
static const ImVec4 COL_ACCENT   = ImVec4(0.063f, 0.486f, 0.063f, 1.0f);
static const ImVec4 COL_ACCENT_H = ImVec4(0.078f, 0.627f, 0.078f, 1.0f);
static const ImVec4 COL_ACCENT_A = ImVec4(0.047f, 0.392f, 0.047f, 1.0f);

static void SetStatus(const char *msg, const ImVec4 &col)
{
    snprintf(g_status, sizeof(g_status), "%s", msg);
    g_status_color = col;
}

static void ApplyLed()
{
    int mode_val = MODES[g_mode_idx].value;
    int bright   = g_brightness;

    if (g_mode_idx == 0) /* Off */
        bright = 0;

    if (!g_ctrl.connected) {
        if (!xbox_open(&g_ctrl)) {
            SetStatus("Cannot open controller - try Refresh", COL_ERROR);
            return;
        }
    }

    bool ok = xbox_set_led(&g_ctrl, (uint8_t)mode_val, (uint8_t)bright);
    if (ok) {
        if (bright == 0 || g_mode_idx == 0) {
            SetStatus("LED turned off", COL_SUCCESS);
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf), "LED: %s at brightness %d/%d",
                     MODES[g_mode_idx].label, bright, LED_BRIGHTNESS_MAX);
            SetStatus(buf, COL_SUCCESS);
        }
        SaveConfig(g_brightness, g_mode_idx, g_start_with_windows, g_minimize_to_tray);
    } else {
        xbox_close(&g_ctrl);
        SetStatus("Command failed - try Refresh to reconnect", COL_ERROR);
    }
}

static void RefreshController()
{
    xbox_close(&g_ctrl);
    g_need_usbdk = false;
    if (xbox_open(&g_ctrl)) {
        SetStatus("Ready - drag the slider or pick a mode", COL_SUCCESS);
    } else if (g_ctrl.last_err == XBOX_ERR_NO_USBDK) {
        g_need_usbdk = true;
        SetStatus("UsbDk driver required - see below", COL_WARN);
    } else {
        SetStatus("Plug in your controller with a USB cable", COL_DIM);
    }
}

/* Try to connect and auto-apply saved settings */
static void TryAutoApply()
{
    if (g_ctrl.connected)
        return;
    xbox_close(&g_ctrl);
    if (xbox_open(&g_ctrl)) {
        SetStatus("Controller connected - applying saved settings", COL_SUCCESS);
        ApplyLed();
    }
}

/* ------------------------------------------------------------------ */
/* WndProc                                                             */
/* ------------------------------------------------------------------ */

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            if (g_minimize_to_tray) {
                MinimizeToTray(hWnd);
                return 0;
            }
        }
        g_ResizeWidth  = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        /* Intercept close to minimize to tray instead */
        if ((wParam & 0xfff0) == SC_CLOSE && g_minimize_to_tray) {
            MinimizeToTray(hWnd);
            return 0;
        }
        break;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONDBLCLK) {
            RestoreFromTray(hWnd);
        } else if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"Show");
            AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit");
            SetForegroundWindow(hWnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(menu);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_SHOW) RestoreFromTray(hWnd);
        if (LOWORD(wParam) == ID_TRAY_QUIT) {
            RemoveTrayIcon();
            DestroyWindow(hWnd);
        }
        return 0;

    case WM_DEVICECHANGE: {
        DWORD now = GetTickCount();
        if (now < g_usb_cooldown_until)
            return 0;
        if (wParam == DBT_DEVICEARRIVAL) {
            g_device_change_pending = true;
            g_device_change_tick = now;
        }
        if (wParam == DBT_DEVNODES_CHANGED) {
            if (!g_device_change_pending) {
                g_device_change_pending = true;
                g_device_change_tick = now;
            }
        }
        if (wParam == DBT_DEVICEREMOVECOMPLETE) {
            g_device_removed = true;
        }
        return 0;
    }

    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* GUI rendering                                                       */
/* ------------------------------------------------------------------ */

static void RenderGui(ImFont *fontTitle, ImFont *fontSub, ImFont *fontBig)
{
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    /* Title */
    ImGui::PushFont(fontTitle);
    ImGui::Text("Xbox LED Control");
    ImGui::PopFont();
    ImGui::Spacing();

    /* ---- Controller card ---- */
    ImGui::BeginChild("##ctrl_card", ImVec2(-1, 80), ImGuiChildFlags_Borders);
    {
        ImGui::PushFont(fontSub);
        ImGui::TextColored(COL_DIM, "CONTROLLER");
        ImGui::SameLine(0, 10);
        if (g_ctrl.connected)
            ImGui::TextColored(COL_SUCCESS, "  CONNECTED");
        else
            ImGui::TextColored(COL_ERROR, "  DISCONNECTED");
        ImGui::PopFont();

        ImGui::Spacing();
        if (g_ctrl.connected) {
            ImGui::TextColored(COL_TEXT, "%s", g_ctrl.name);
            ImGui::TextColored(COL_DIM, "VID: 0x%04X   PID: 0x%04X", g_ctrl.vid, g_ctrl.pid);
        } else {
            ImGui::TextColored(COL_DIM, "No controller found");
            ImGui::TextColored(COL_DIM, "Connect an Xbox controller via USB");
        }
    }
    ImGui::EndChild();
    ImGui::Spacing();

    /* ---- UsbDk install banner ---- */
    if (g_need_usbdk) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.10f, 0.02f, 1.0f));
        ImGui::BeginChild("##usbdk_banner", ImVec2(-1, 90), ImGuiChildFlags_Borders);
        {
            ImGui::PushFont(fontSub);
            ImGui::TextColored(COL_WARN, "DRIVER REQUIRED");
            ImGui::PopFont();
            ImGui::Spacing();
            ImGui::TextWrapped("UsbDk USB filter driver is not installed. Install it and reboot your PC.");
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Button,       COL_WARN);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.80f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.80f, 0.63f, 0.12f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0,0,0,1));
            if (ImGui::Button("Download UsbDk")) {
                ShellExecuteA(nullptr, "open",
                    "https://github.com/daynix/UsbDk/releases", nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::PopStyleColor(4);

            ImGui::SameLine();
            ImGui::TextColored(COL_DIM, "Reboot after installing");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    /* ---- Brightness card ---- */
    ImGui::BeginChild("##bright_card", ImVec2(-1, 120), ImGuiChildFlags_Borders);
    {
        ImGui::PushFont(fontSub);
        ImGui::TextColored(COL_DIM, "BRIGHTNESS");
        ImGui::PopFont();

        /* Big number on the right */
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60);
        float pct = (float)g_brightness / LED_BRIGHTNESS_MAX;
        ImVec4 numCol = ImVec4(
            0.063f + 0.094f * pct,
            0.486f + (0.863f - 0.486f) * pct,
            0.063f + 0.094f * pct,
            1.0f
        );
        ImGui::PushFont(fontBig);
        ImGui::TextColored(numCol, "%d", g_brightness);
        ImGui::PopFont();

        /* Slider */
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##brightness", &g_brightness, 0, LED_BRIGHTNESS_MAX, "", ImGuiSliderFlags_None)) {
            /* Value changed while dragging */
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            ApplyLed();
        }

        /* Min/max labels */
        ImGui::TextColored(COL_DIM, "0");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
        ImGui::TextColored(COL_DIM, "%d", LED_BRIGHTNESS_MAX);
    }
    ImGui::EndChild();
    ImGui::Spacing();

    /* ---- Mode card ---- */
    ImGui::BeginChild("##mode_card", ImVec2(-1, 80), ImGuiChildFlags_Borders);
    {
        ImGui::PushFont(fontSub);
        ImGui::TextColored(COL_DIM, "LED MODE");
        ImGui::PopFont();
        ImGui::Spacing();

        for (int i = 0; i < MODE_COUNT; i++) {
            if (i > 0) ImGui::SameLine();

            bool is_active = (i == g_mode_idx);
            if (is_active) {
                ImGui::PushStyleColor(ImGuiCol_Button,        COL_ACCENT);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  COL_ACCENT_H);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   COL_ACCENT_A);
                ImGui::PushStyleColor(ImGuiCol_Text,           ImVec4(1,1,1,1));
            }

            if (ImGui::Button(MODES[i].label)) {
                g_mode_idx = i;
                ApplyLed();
            }

            if (is_active)
                ImGui::PopStyleColor(4);
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();

    /* ---- Bottom bar ---- */
    /* Apply button */
    ImGui::PushStyleColor(ImGuiCol_Button,       COL_ACCENT);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_ACCENT_H);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  COL_ACCENT_A);
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1,1,1,1));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(24, 12));
    if (ImGui::Button("Apply"))
        ApplyLed();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);

    ImGui::SameLine();

    /* Refresh button */
    ImGui::PushStyleColor(ImGuiCol_Button,       ImVec4(0.157f, 0.157f, 0.216f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.216f, 0.216f, 0.275f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.255f, 0.255f, 0.314f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16, 12));
    if (ImGui::Button("Refresh"))
        RefreshController();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    ImGui::Spacing();
    ImGui::TextColored(g_status_color, "%s", g_status);

    /* ---- Settings ---- */
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Checkbox("Start with Windows", &g_start_with_windows)) {
        SetAutoStart(g_start_with_windows);
        SaveConfig(g_brightness, g_mode_idx, g_start_with_windows, g_minimize_to_tray);
    }
    ImGui::SameLine(0, 20);
    if (ImGui::Checkbox("Minimize to tray", &g_minimize_to_tray)) {
        SaveConfig(g_brightness, g_mode_idx, g_start_with_windows, g_minimize_to_tray);
    }

    ImGui::End();
}

/* ------------------------------------------------------------------ */
/* D3D11 helpers                                                       */
/* ------------------------------------------------------------------ */

static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext)  { g_pd3dDeviceContext->Release();  g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)         { g_pd3dDevice->Release();         g_pd3dDevice = nullptr; }
}

static void CreateRenderTarget()
{
    ID3D11Texture2D *pBack;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBack));
    g_pd3dDevice->CreateRenderTargetView(pBack, nullptr, &g_mainRenderTargetView);
    pBack->Release();
}

static void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
    /* Check if launched with --minimized (auto-start) */
    bool start_minimized = (strstr(lpCmdLine, "--minimized") != nullptr);

    /* Init controller */
    xbox_init(&g_ctrl);
    g_status_color = COL_DIM;

    /* Load saved settings */
    InitConfigPath();
    LoadConfig(&g_brightness, &g_mode_idx, &g_start_with_windows, &g_minimize_to_tray);

    /* Early UsbDk check */
    if (!xbox_is_usbdk_installed() && !start_minimized) {
        int choice = MessageBoxW(nullptr,
            L"UsbDk USB filter driver is not installed.\n\n"
            L"xbledctl requires UsbDk to communicate with Xbox controllers.\n"
            L"Click OK to open the download page, then reboot after installing.\n\n"
            L"You can also continue without it, but LED control won't work.",
            L"Xbox LED Control - Driver Required",
            MB_OKCANCEL | MB_ICONWARNING);
        if (choice == IDOK) {
            ShellExecuteA(nullptr, "open",
                "https://github.com/daynix/UsbDk/releases", nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    /* Create window */
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
        nullptr, nullptr, nullptr, nullptr, L"xbledctl", nullptr };
    RegisterClassExW(&wc);

    RECT wr = { 0, 0, 520, 580 };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&wr, style, FALSE);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Xbox LED Control",
        style, CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    /* Register for USB device notifications */
    DEV_BROADCAST_DEVICEINTERFACE dbdi = {};
    dbdi.dbcc_size = sizeof(dbdi);
    dbdi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    RegisterDeviceNotification(g_hwnd, &dbdi, DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);

    /* System tray icon */
    AddTrayIcon(g_hwnd);

    if (start_minimized) {
        MinimizeToTray(g_hwnd);
    } else {
        ShowWindow(g_hwnd, SW_SHOWDEFAULT);
        UpdateWindow(g_hwnd);
    }

    /* ImGui setup */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ApplyXboxTheme();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    /* Load fonts */
    ImFont *fontDefault = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    ImFont *fontTitle   = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 28.0f);
    ImFont *fontSub     = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 14.0f);
    ImFont *fontBig     = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 42.0f);
    if (!fontDefault) fontDefault = io.Fonts->AddFontDefault();
    if (!fontTitle)   fontTitle   = fontDefault;
    if (!fontSub)     fontSub     = fontDefault;
    if (!fontBig)     fontBig     = fontDefault;

    /* Initial controller scan + auto-apply saved settings */
    RefreshController();
    if (g_ctrl.connected) {
        ApplyLed();
        g_usb_cooldown_until = GetTickCount() + 2000;
    }

    /* Sync autostart checkbox with actual registry state */
    g_start_with_windows = IsAutoStartEnabled();

    const float clear[4] = { 0.071f, 0.071f, 0.094f, 1.0f };

    /* Main loop */
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) break;

        /* Handle USB device removal */
        if (g_device_removed) {
            g_device_removed = false;
            if (g_ctrl.connected) {
                xbox_close(&g_ctrl);
                SetStatus("Controller disconnected", COL_DIM);
                g_usb_cooldown_until = GetTickCount() + 2000;
                g_device_change_pending = false;
            }
        }

        /* Handle USB device arrival - only act when not already connected */
        if (g_device_change_pending && !g_ctrl.connected
            && (GetTickCount() - g_device_change_tick) >= 1000) {
            g_device_change_pending = false;
            TryAutoApply();
            if (g_ctrl.connected)
                g_usb_cooldown_until = GetTickCount() + 2000;
        } else if (g_device_change_pending && g_ctrl.connected) {
            g_device_change_pending = false;
        }

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::PushFont(fontDefault);
        RenderGui(fontTitle, fontSub, fontBig);
        ImGui::PopFont();

        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    /* Cleanup */
    xbox_cleanup(&g_ctrl);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);

    return 0;
}
