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
#include <cstdio>

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

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth  = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
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
static const ImVec4   COL_WARN = ImVec4(0.902f, 0.706f, 0.157f, 1.0f); /* (230,180,40) */

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
static const ImVec4 COL_SUCCESS  = ImVec4(0.157f, 0.784f, 0.314f, 1.0f); /* (40,200,80)   */
static const ImVec4 COL_ERROR    = ImVec4(0.863f, 0.235f, 0.235f, 1.0f); /* (220,60,60)   */
static const ImVec4 COL_DIM      = ImVec4(0.549f, 0.549f, 0.588f, 1.0f); /* (140,140,150) */
static const ImVec4 COL_TEXT     = ImVec4(0.902f, 0.902f, 0.922f, 1.0f); /* (230,230,235) */
static const ImVec4 COL_ACCENT   = ImVec4(0.063f, 0.486f, 0.063f, 1.0f); /* (16,124,16)   */
static const ImVec4 COL_ACCENT_H = ImVec4(0.078f, 0.627f, 0.078f, 1.0f); /* (20,160,20)   */
static const ImVec4 COL_ACCENT_A = ImVec4(0.047f, 0.392f, 0.047f, 1.0f); /* (12,100,12)   */

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
            /* Value changed while dragging - nothing to do */
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

    ImGui::End();
}

/* ------------------------------------------------------------------ */
/* D3D11 helpers (from Dear ImGui example)                             */
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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    /* Init controller */
    xbox_init(&g_ctrl);
    g_status_color = COL_DIM;

    /* Early UsbDk check - show native dialog before the GUI even loads */
    if (!xbox_is_usbdk_installed()) {
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

    /* Calculate window rect for 520x530 client area */
    RECT wr = { 0, 0, 520, 530 };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&wr, style, FALSE);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Xbox LED Control",
        style, CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    /* ImGui setup */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; /* no imgui.ini */

    ApplyXboxTheme();

    ImGui_ImplWin32_Init(hwnd);
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

    /* Initial controller scan */
    RefreshController();

    /* Clear color (dark background) */
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

        HRESULT hr = g_pSwapChain->Present(1, 0); /* vsync */
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    /* Cleanup */
    xbox_cleanup(&g_ctrl);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);

    return 0;
}
