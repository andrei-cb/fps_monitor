#include "overlay_window.h"

#include "resource.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;

namespace {

const wchar_t kWindowClass[] = L"FpsMonitorOverlayWindow";
constexpr DWORD kBaseExStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP;
constexpr DWORD kPassiveExStyle = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    wchar_t winDir[MAX_PATH];
    GetWindowsDirectoryW(winDir, MAX_PATH);
    for (const wchar_t* file : {L"seguisb.ttf", L"segoeui.ttf"}) {
        wchar_t wpath[MAX_PATH];
        swprintf_s(wpath, L"%s\\Fonts\\%s", winDir, file);
        if (GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES) continue;
        char path[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, sizeof(path), nullptr, nullptr);
        if (io.Fonts->AddFontFromFileTTF(path, 17.0f)) return;
    }
    io.Fonts->AddFontDefault();
}

} // namespace

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> list;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM user) -> BOOL {
        auto& out = *reinterpret_cast<std::vector<MonitorInfo>*>(user);
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi)) return TRUE;

        MonitorInfo info;
        info.rect = mi.rcMonitor;
        char dev[64];
        WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, dev, sizeof(dev), nullptr, nullptr);
        const char* name = dev;
        if (!strncmp(name, "\\\\.\\", 4)) name += 4;
        char label[160];
        std::snprintf(label, sizeof(label), "%s  %ldx%ld%s", name, mi.rcMonitor.right - mi.rcMonitor.left,
                      mi.rcMonitor.bottom - mi.rcMonitor.top, (mi.dwFlags & MONITORINFOF_PRIMARY) ? "  (primary)" : "");
        info.label = label;

        if (mi.dwFlags & MONITORINFOF_PRIMARY) out.insert(out.begin(), info);
        else out.push_back(info);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&list));
    return list;
}

bool OverlayWindow::Create(HINSTANCE instance, MessageHandler handler) {
    handler_ = std::move(handler);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = &OverlayWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(kBaseExStyle | kPassiveExStyle, kWindowClass, L"FPS Monitor", WS_POPUP, 0, 0, 1, 1,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);

    if (!CreateDevice()) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    LoadFonts();
    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_.Get(), context_.Get());
    return true; // hidden until the first frame is rendered
}

void OverlayWindow::Destroy() {
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    rtv_.Reset();
    dcompVisual_.Reset();
    dcompTarget_.Reset();
    dcompDevice_.Reset();
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

bool OverlayWindow::CreateDevice() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, (UINT)std::size(levels),
                                   D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, (UINT)std::size(levels),
                               D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return false;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(factory->CreateSwapChainForComposition(device_.Get(), &desc, nullptr, &swapChain_))) return false;

    if (FAILED(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcompDevice_))) ||
        FAILED(dcompDevice_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_)) ||
        FAILED(dcompDevice_->CreateVisual(&dcompVisual_)) ||
        FAILED(dcompVisual_->SetContent(swapChain_.Get())) ||
        FAILED(dcompTarget_->SetRoot(dcompVisual_.Get())) ||
        FAILED(dcompDevice_->Commit()))
        return false;

    return CreateRenderTarget();
}

bool OverlayWindow::CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
    return SUCCEEDED(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_));
}

void OverlayWindow::SetBounds(const RECT& r) {
    UINT w = (UINT)std::max<LONG>(1, r.right - r.left);
    UINT h = (UINT)std::max<LONG>(1, r.bottom - r.top);
    bounds_ = {r.left, r.top, r.left + (LONG)w, r.top + (LONG)h};
    SetWindowPos(hwnd_, HWND_TOPMOST, r.left, r.top, (int)w, (int)h, SWP_NOACTIVATE);
    if (w != width_ || h != height_) {
        width_ = w;
        height_ = h;
        rtv_.Reset();
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->Flush();
        swapChain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        CreateRenderTarget();
    }
    UpdateDpi();
}

void OverlayWindow::SetInteractive(bool interactive) {
    interactive_ = interactive;
    passThrough_ = !interactive;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    ex = interactive ? (ex & ~(LONG_PTR)kPassiveExStyle) : (ex | kPassiveExStyle);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    if (interactive) {
        // Allowed because the hotkey / tray click gives us foreground rights.
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
    }
}

void OverlayWindow::SetPassThrough(bool passThrough) {
    if (passThrough == passThrough_) return;
    passThrough_ = passThrough;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    ex = passThrough ? (ex | WS_EX_TRANSPARENT) : (ex & ~(LONG_PTR)WS_EX_TRANSPARENT);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
    // We stop receiving mouse messages, so tell ImGui the cursor left (clears stale hover).
    if (passThrough) ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

bool OverlayWindow::CursorOverImGuiWindow() const {
    POINT pt;
    if (!GetCursorPos(&pt) || !ScreenToClient(hwnd_, &pt)) return false;
    const ImVec2 p((float)pt.x, (float)pt.y);
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) {
        if (!w->Active || w->Hidden || (w->Flags & ImGuiWindowFlags_NoInputs)) continue;
        ImRect r = w->Rect();
        r.Expand(4.0f); // resize grips sit slightly outside
        if (r.Contains(p)) return true;
    }
    return false;
}

void OverlayWindow::UpdateHitTest() {
    if (!interactive_) return;
    // Keep input while ImGui holds mouse capture (e.g. dragging a slider past the panel edge).
    SetPassThrough(!(GetCapture() == hwnd_ || CursorOverImGuiWindow()));
}

void OverlayWindow::SetExcludeFromCapture(bool exclude) {
    SetWindowDisplayAffinity(hwnd_, exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
}

void OverlayWindow::ReassertTopmost() {
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void OverlayWindow::SetVisible(bool visible) {
    if (visible == visible_) return;
    visible_ = visible;
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void OverlayWindow::UpdateDpi() {
    float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
    if (dpi <= 0.0f || dpi == dpiScale_) return;
    dpiScale_ = dpi;

    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowBorderSize = 0.0f;
    style.WindowPadding = ImVec2(10, 8);
    style.ItemSpacing = ImVec2(8, 6);
    style.ScaleAllSizes(dpi);
    style.FontScaleDpi = dpi;
    ImGui::GetStyle() = style;
}

void OverlayWindow::NewFrame(ImVec2 layoutSize) {
    UpdateDpi();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame(); // sets DisplaySize to the client size...
    ImGui::GetIO().DisplaySize = ImVec2(std::max(layoutSize.x, (float)width_), std::max(layoutSize.y, (float)height_));
    ImGui::NewFrame();
}

void OverlayWindow::Render(bool vsync) {
    ImGui::Render();
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ID3D11RenderTargetView* rtv = rtv_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->ClearRenderTargetView(rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain_->Present(vsync ? 1 : 0, 0);
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return TRUE;

    if (self && self->handler_) {
        if (auto r = self->handler_(hwnd, msg, wp, lp)) return *r;
    }

    switch (msg) {
    case WM_MOUSEACTIVATE:
        if (self && !self->interactive_) return MA_NOACTIVATE;
        break;
    case WM_DPICHANGED:
        return 0; // we size ourselves to the monitor
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
