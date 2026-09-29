#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <imgui.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

struct MonitorInfo {
    RECT rect = {};
    std::string label;
};

// Primary monitor first, then the rest in enumeration order (matches Config::monitor).
std::vector<MonitorInfo> EnumerateMonitors();

// A borderless, topmost, click-through window. Normally it is only as big as the stats
// panel; the owner resizes it (SetBounds) as the layout changes. Content is rendered with
// D3D11 into a premultiplied-alpha DirectComposition swap chain, so everything that
// ImGui does not draw stays fully transparent.
class OverlayWindow {
public:
    using MessageHandler = std::function<std::optional<LRESULT>(HWND, UINT, WPARAM, LPARAM)>;

    bool Create(HINSTANCE instance, MessageHandler handler);
    void Destroy();

    HWND Hwnd() const { return hwnd_; }

    void SetBounds(const RECT& rect);
    const RECT& Bounds() const { return bounds_; }
    void SetInteractive(bool interactive);
    void SetExcludeFromCapture(bool exclude);
    void ReassertTopmost();
    // Shows the last rendered frame, so render before showing.
    void SetVisible(bool visible);
    bool Visible() const { return visible_; }

    // While interactive, only ImGui windows take mouse input; everywhere else
    // clicks fall through to whatever is underneath. Call once per frame after Render().
    void UpdateHitTest();

    // `layoutSize` is the ImGui display size (the monitor), which may be larger than the
    // window: ImGui caps auto-sized windows to the display, so it must not shrink with us.
    void NewFrame(ImVec2 layoutSize);
    void Render(bool vsync);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    bool CreateDevice();
    bool CreateRenderTarget();
    void UpdateDpi();
    void SetPassThrough(bool passThrough);
    bool CursorOverImGuiWindow() const;

    HWND hwnd_ = nullptr;
    MessageHandler handler_;
    bool interactive_ = false;
    bool passThrough_ = true;
    bool visible_ = false;
    UINT width_ = 1, height_ = 1;
    RECT bounds_ = {};
    float dpiScale_ = 0.0f;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> dcompDevice_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcompTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcompVisual_;
};
