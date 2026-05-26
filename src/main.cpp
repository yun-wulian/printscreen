#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <gdiplus.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "resource.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT WM_APP_TRIGGER_CAPTURE = WM_APP + 101;
constexpr int HOTKEY_PRINTSCREEN = 1;

struct CapturedFrame {
    int width = 0;
    int height = 0;
    RECT desktopRect{};
    std::vector<std::uint8_t> pixels;      // BGRA, top-down
    std::vector<std::uint8_t> darkPixels;  // BGRA, top-down
};

struct CroppedImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;  // BGRA, top-down
};

enum class AnnotationTool {
    Select,
    Pen,
    Rectangle,
    Ellipse,
    Arrow,
    Text,
};

enum class OverlayAction {
    None,
    Selecting,
    MovingSelection,
    ResizingSelection,
    DrawingAnnotation,
};

enum class ResizeHandle {
    None,
    Left,
    Top,
    Right,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

struct AnnotationCommand {
    AnnotationTool tool = AnnotationTool::Pen;
    COLORREF color = RGB(255, 64, 64);
    int strokeWidth = 3;
    std::vector<POINT> points;
    std::wstring text;
};

struct ToolbarButton {
    RECT rect{};
    int id = 0;
    AnnotationTool tool = AnnotationTool::Select;
    const wchar_t* label = L"";
    int width = 38;
};

struct OverlayState {
    CapturedFrame* frame = nullptr;
    POINT dragStart{};
    POINT dragCurrent{};
    POINT selectionStartOrigin{};
    RECT resizeStartRect{};
    ResizeHandle resizeHandle = ResizeHandle::None;
    OverlayAction action = OverlayAction::None;
    bool finished = false;
    bool canceled = false;
    bool accepted = false;
    RECT selectedRect{};  // client-space, right/bottom exclusive
    AnnotationTool activeTool = AnnotationTool::Select;
    COLORREF activeColor = RGB(255, 64, 64);
    int strokeWidth = 4;
    std::vector<AnnotationCommand> annotations;
    AnnotationCommand currentAnnotation;
    bool textActive = false;
    POINT textPoint{};
    std::wstring textBuffer;
};

HHOOK g_keyboardHook = nullptr;
DWORD g_mainThreadId = 0;
bool g_captureInProgress = false;
bool g_captureQueued = false;

bool HasArea(const RECT& r) {
    return (r.right - r.left) > 1 && (r.bottom - r.top) > 1;
}

RECT NormalizeRect(POINT a, POINT b) {
    RECT r{};
    r.left = std::min(a.x, b.x);
    r.top = std::min(a.y, b.y);
    r.right = std::max(a.x, b.x);
    r.bottom = std::max(a.y, b.y);
    return r;
}

RECT ClampToBounds(const RECT& r, int width, int height) {
    RECT c = r;
    c.left = std::clamp<LONG>(c.left, 0, static_cast<LONG>(width));
    c.right = std::clamp<LONG>(c.right, 0, static_cast<LONG>(width));
    c.top = std::clamp<LONG>(c.top, 0, static_cast<LONG>(height));
    c.bottom = std::clamp<LONG>(c.bottom, 0, static_cast<LONG>(height));
    return c;
}

RECT OffsetRectCopy(RECT r, int dx, int dy) {
    OffsetRect(&r, dx, dy);
    return r;
}

bool PtInRectInclusive(const RECT& r, POINT p) {
    return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
}

int RectWidth(const RECT& r) {
    return r.right - r.left;
}

int RectHeight(const RECT& r) {
    return r.bottom - r.top;
}

void ClampMoveRectToBounds(RECT& r, int width, int height) {
    const int w = RectWidth(r);
    const int h = RectHeight(r);
    if (r.left < 0) {
        OffsetRect(&r, -r.left, 0);
    }
    if (r.top < 0) {
        OffsetRect(&r, 0, -r.top);
    }
    if (r.right > width) {
        OffsetRect(&r, width - r.right, 0);
    }
    if (r.bottom > height) {
        OffsetRect(&r, 0, height - r.bottom);
    }
    r.right = r.left + w;
    r.bottom = r.top + h;
}

RECT ResizeRectFromHandle(RECT startRect, ResizeHandle handle, POINT p, int width, int height) {
    constexpr int minSize = 8;
    RECT r = startRect;
    switch (handle) {
    case ResizeHandle::Left:
    case ResizeHandle::TopLeft:
    case ResizeHandle::BottomLeft:
        r.left = std::clamp<LONG>(p.x, 0, r.right - minSize);
        break;
    default:
        break;
    }
    switch (handle) {
    case ResizeHandle::Right:
    case ResizeHandle::TopRight:
    case ResizeHandle::BottomRight:
        r.right = std::clamp<LONG>(p.x, r.left + minSize, width);
        break;
    default:
        break;
    }
    switch (handle) {
    case ResizeHandle::Top:
    case ResizeHandle::TopLeft:
    case ResizeHandle::TopRight:
        r.top = std::clamp<LONG>(p.y, 0, r.bottom - minSize);
        break;
    default:
        break;
    }
    switch (handle) {
    case ResizeHandle::Bottom:
    case ResizeHandle::BottomLeft:
    case ResizeHandle::BottomRight:
        r.bottom = std::clamp<LONG>(p.y, r.top + minSize, height);
        break;
    default:
        break;
    }
    return r;
}

void BuildDarkened(const std::vector<std::uint8_t>& src, std::vector<std::uint8_t>& dst) {
    dst = src;
    constexpr float factor = 0.38f;
    for (size_t i = 0; i + 3 < dst.size(); i += 4) {
        dst[i + 0] = static_cast<std::uint8_t>(dst[i + 0] * factor);
        dst[i + 1] = static_cast<std::uint8_t>(dst[i + 1] * factor);
        dst[i + 2] = static_cast<std::uint8_t>(dst[i + 2] * factor);
        dst[i + 3] = 0xFF;
    }
}

bool IsAllBlackFrame(const std::vector<std::uint8_t>& pixels) {
    if (pixels.empty()) {
        return true;
    }

    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        if (pixels[i + 0] != 0 || pixels[i + 1] != 0 || pixels[i + 2] != 0) {
            return false;
        }
    }
    return true;
}

RECT GetVirtualDesktopRect() {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return RECT{left, top, left + width, top + height};
}

bool PointInDesktopRect(const RECT& r, const POINT& p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

void CompositeFrame(CapturedFrame& dst, const CapturedFrame& src) {
    RECT overlap{
        std::max(dst.desktopRect.left, src.desktopRect.left),
        std::max(dst.desktopRect.top, src.desktopRect.top),
        std::min(dst.desktopRect.right, src.desktopRect.right),
        std::min(dst.desktopRect.bottom, src.desktopRect.bottom)
    };

    if (!HasArea(overlap)) {
        return;
    }

    const int copyWidth = RectWidth(overlap);
    for (int y = 0; y < RectHeight(overlap); ++y) {
        const int dstX = overlap.left - dst.desktopRect.left;
        const int dstY = overlap.top - dst.desktopRect.top + y;
        const int srcX = overlap.left - src.desktopRect.left;
        const int srcY = overlap.top - src.desktopRect.top + y;
        const auto* srcRow = src.pixels.data() +
            (static_cast<size_t>(srcY) * static_cast<size_t>(src.width) + static_cast<size_t>(srcX)) * 4;
        auto* dstRow = dst.pixels.data() +
            (static_cast<size_t>(dstY) * static_cast<size_t>(dst.width) + static_cast<size_t>(dstX)) * 4;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(copyWidth) * 4);
    }
}

bool CaptureCurrentOutputWithDxgi(const POINT& cursorPos, CapturedFrame& out) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    ComPtr<IDXGIAdapter1> selectedAdapter;
    ComPtr<IDXGIOutput1> selectedOutput1;
    RECT selectedRect{};

    for (UINT ai = 0;; ++ai) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        for (UINT oi = 0;; ++oi) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }

            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || !PointInDesktopRect(desc.DesktopCoordinates, cursorPos)) {
                continue;
            }

            ComPtr<IDXGIOutput1> output1;
            if (FAILED(output.As(&output1))) {
                continue;
            }

            selectedAdapter = adapter;
            selectedOutput1 = output1;
            selectedRect = desc.DesktopCoordinates;
            break;
        }

        if (selectedOutput1) {
            break;
        }
    }

    if (!selectedOutput1) {
        return false;
    }

    UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevel{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT hr = D3D11CreateDevice(
        selectedAdapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        deviceFlags,
        levels,
        ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        &device,
        &featureLevel,
        &context);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IDXGIOutputDuplication> duplication;
    if (FAILED(selectedOutput1->DuplicateOutput(device.Get(), &duplication))) {
        return false;
    }

    ComPtr<IDXGIResource> desktopResource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};

    bool gotFrame = false;
    for (int i = 0; i < 12; ++i) {
        hr = duplication->AcquireNextFrame(60, &frameInfo, &desktopResource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }
        if (FAILED(hr)) {
            return false;
        }
        gotFrame = true;
        break;
    }

    if (!gotFrame) {
        return false;
    }

    ComPtr<ID3D11Texture2D> gpuTexture;
    if (FAILED(desktopResource.As(&gpuTexture))) {
        duplication->ReleaseFrame();
        return false;
    }

    D3D11_TEXTURE2D_DESC texDesc{};
    gpuTexture->GetDesc(&texDesc);

    D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> stagingTexture;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture))) {
        duplication->ReleaseFrame();
        return false;
    }

    context->CopyResource(stagingTexture.Get(), gpuTexture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        duplication->ReleaseFrame();
        return false;
    }

    out.width = static_cast<int>(texDesc.Width);
    out.height = static_cast<int>(texDesc.Height);
    out.desktopRect = selectedRect;
    out.pixels.resize(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4);

    for (int y = 0; y < out.height; ++y) {
        const auto* src = reinterpret_cast<const std::uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        auto* dst = out.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(out.width) * 4;
        std::memcpy(dst, src, static_cast<size_t>(out.width) * 4);
    }

    context->Unmap(stagingTexture.Get(), 0);
    duplication->ReleaseFrame();

    if (IsAllBlackFrame(out.pixels)) {
        return false;
    }

    BuildDarkened(out.pixels, out.darkPixels);
    return true;
}

bool CaptureWithGdiVirtualDesktop(CapturedFrame& out) {
    const RECT r = GetVirtualDesktopRect();
    const int width = RectWidth(r);
    const int height = RectHeight(r);
    if (width <= 0 || height <= 0) {
        return false;
    }

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) {
        return false;
    }

    HDC memDc = CreateCompatibleDC(screenDc);
    if (!memDc) {
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    HBITMAP bmp = CreateCompatibleBitmap(screenDc, width, height);
    if (!bmp) {
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    HGDIOBJ old = SelectObject(memDc, bmp);
    const BOOL bltOk = BitBlt(memDc, 0, 0, width, height, screenDc, r.left, r.top, SRCCOPY | CAPTUREBLT);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    out.width = width;
    out.height = height;
    out.desktopRect = r;
    out.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    const int lines = GetDIBits(memDc, bmp, 0, static_cast<UINT>(height), out.pixels.data(), &bmi, DIB_RGB_COLORS);

    SelectObject(memDc, old);
    DeleteObject(bmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);

    if (!bltOk || lines != height || IsAllBlackFrame(out.pixels)) {
        return false;
    }

    BuildDarkened(out.pixels, out.darkPixels);
    return true;
}

bool CaptureDesktopFrame(const POINT& cursorPos, CapturedFrame& out) {
    const bool gdiCaptured = CaptureWithGdiVirtualDesktop(out);

    CapturedFrame dxgiFrame;
    if (CaptureCurrentOutputWithDxgi(cursorPos, dxgiFrame)) {
        if (!gdiCaptured) {
            const RECT virtualRect = GetVirtualDesktopRect();
            out.width = RectWidth(virtualRect);
            out.height = RectHeight(virtualRect);
            out.desktopRect = virtualRect;
            out.pixels.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4, 0);
        }

        CompositeFrame(out, dxgiFrame);
        BuildDarkened(out.pixels, out.darkPixels);
        return !IsAllBlackFrame(out.pixels);
    }

    if (gdiCaptured) {
        return true;
    }

    return false;
}

void DrawFrameFull(HDC hdc, const std::vector<std::uint8_t>& pixels, int frameWidth, int frameHeight) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = frameWidth;
    bmi.bmiHeader.biHeight = -frameHeight;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    const int scanLinesCopied = SetDIBitsToDevice(
        hdc,
        0,
        0,
        frameWidth,
        frameHeight,
        0,
        0,
        0,
        static_cast<UINT>(frameHeight),
        pixels.data(),
        &bmi,
        DIB_RGB_COLORS);

    if (scanLinesCopied == 0) {
        StretchDIBits(
            hdc,
            0,
            0,
            frameWidth,
            frameHeight,
            0,
            0,
            frameWidth,
            frameHeight,
            pixels.data(),
            &bmi,
            DIB_RGB_COLORS,
            SRCCOPY);
    }
}

void DrawFrameClipRect(HDC hdc, const std::vector<std::uint8_t>& pixels, int frameWidth, int frameHeight, const RECT& clipRect) {
    const int saved = SaveDC(hdc);
    IntersectClipRect(hdc, clipRect.left, clipRect.top, clipRect.right, clipRect.bottom);
    DrawFrameFull(hdc, pixels, frameWidth, frameHeight);
    RestoreDC(hdc, saved);
}

void DrawAnnotation(HDC hdc, const AnnotationCommand& command, POINT offset = POINT{0, 0}) {
    if (command.points.empty()) {
        return;
    }

    HPEN pen = CreatePen(PS_SOLID, command.strokeWidth, command.color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, command.color);

    auto tx = [offset](POINT p) {
        p.x -= offset.x;
        p.y -= offset.y;
        return p;
    };

    switch (command.tool) {
    case AnnotationTool::Pen:
        if (command.points.size() == 1) {
            POINT p = tx(command.points[0]);
            Ellipse(hdc, p.x - command.strokeWidth, p.y - command.strokeWidth, p.x + command.strokeWidth, p.y + command.strokeWidth);
        } else {
            POINT first = tx(command.points[0]);
            MoveToEx(hdc, first.x, first.y, nullptr);
            for (size_t i = 1; i < command.points.size(); ++i) {
                POINT p = tx(command.points[i]);
                LineTo(hdc, p.x, p.y);
            }
        }
        break;
    case AnnotationTool::Rectangle:
        if (command.points.size() >= 2) {
            POINT a = tx(command.points[0]);
            POINT b = tx(command.points[1]);
            RECT r = NormalizeRect(a, b);
            Rectangle(hdc, r.left, r.top, r.right, r.bottom);
        }
        break;
    case AnnotationTool::Ellipse:
        if (command.points.size() >= 2) {
            POINT a = tx(command.points[0]);
            POINT b = tx(command.points[1]);
            RECT r = NormalizeRect(a, b);
            Ellipse(hdc, r.left, r.top, r.right, r.bottom);
        }
        break;
    case AnnotationTool::Arrow:
        if (command.points.size() >= 2) {
            POINT a = tx(command.points[0]);
            POINT b = tx(command.points[1]);
            MoveToEx(hdc, a.x, a.y, nullptr);
            LineTo(hdc, b.x, b.y);

            const double dx = static_cast<double>(b.x - a.x);
            const double dy = static_cast<double>(b.y - a.y);
            const double len = std::sqrt(dx * dx + dy * dy);
            if (len > 0.1) {
                const double ux = dx / len;
                const double uy = dy / len;
                const double head = std::max(14, command.strokeWidth * 6);
                const double wing = head * 0.55;
                POINT left{
                    static_cast<LONG>(b.x - ux * head - uy * wing),
                    static_cast<LONG>(b.y - uy * head + ux * wing)
                };
                POINT right{
                    static_cast<LONG>(b.x - ux * head + uy * wing),
                    static_cast<LONG>(b.y - uy * head - ux * wing)
                };
                MoveToEx(hdc, b.x, b.y, nullptr);
                LineTo(hdc, left.x, left.y);
                MoveToEx(hdc, b.x, b.y, nullptr);
                LineTo(hdc, right.x, right.y);
            }
        }
        break;
    case AnnotationTool::Text:
        if (!command.text.empty()) {
            POINT p = tx(command.points[0]);
            HFONT font = CreateFontW(
                std::max(18, command.strokeWidth * 7),
                0,
                0,
                0,
                FW_SEMIBOLD,
                FALSE,
                FALSE,
                FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS,
                L"Microsoft YaHei UI");
            HGDIOBJ oldFont = SelectObject(hdc, font);
            TextOutW(hdc, p.x, p.y, command.text.c_str(), static_cast<int>(command.text.size()));
            SelectObject(hdc, oldFont);
            DeleteObject(font);
        }
        break;
    default:
        break;
    }

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

int PngResourceForButton(const ToolbarButton& button) {
    if (button.id == 0) {
        switch (button.tool) {
        case AnnotationTool::Select:
            return IDB_ICON_MOVE;
        case AnnotationTool::Arrow:
            return IDB_ICON_ARROW;
        case AnnotationTool::Pen:
            return IDB_ICON_PEN;
        case AnnotationTool::Rectangle:
            return IDB_ICON_RECTANGLE;
        case AnnotationTool::Ellipse:
            return IDB_ICON_ELLIPSE;
        case AnnotationTool::Text:
            return IDB_ICON_TEXT;
        default:
            return 0;
        }
    }

    switch (button.id) {
    case 100:
        return IDB_ICON_UNDO;
    case 101:
        return IDB_ICON_COLOR;
    case 102:
        return IDB_ICON_CANCEL;
    case 103:
        return IDB_ICON_OK;
    default:
        return 0;
    }
}

std::unique_ptr<Gdiplus::Bitmap> LoadPngBitmapResource(int resourceId) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) {
        return nullptr;
    }

    HGLOBAL loaded = LoadResource(module, resource);
    const DWORD size = SizeofResource(module, resource);
    const void* data = LockResource(loaded);
    if (!loaded || size == 0 || !data) {
        return nullptr;
    }

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) {
        return nullptr;
    }

    void* target = GlobalLock(memory);
    if (!target) {
        GlobalFree(memory);
        return nullptr;
    }
    std::memcpy(target, data, size);
    GlobalUnlock(memory);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
        GlobalFree(memory);
        return nullptr;
    }

    std::unique_ptr<Gdiplus::Bitmap> source(Gdiplus::Bitmap::FromStream(stream, FALSE));
    if (!source || source->GetLastStatus() != Gdiplus::Ok) {
        stream->Release();
        return nullptr;
    }

    Gdiplus::Rect bounds(0, 0, static_cast<INT>(source->GetWidth()), static_cast<INT>(source->GetHeight()));
    Gdiplus::Bitmap* cloned = source->Clone(bounds, PixelFormat32bppPARGB);
    stream->Release();
    if (!cloned || cloned->GetLastStatus() != Gdiplus::Ok) {
        delete cloned;
        return nullptr;
    }
    return std::unique_ptr<Gdiplus::Bitmap>(cloned);
}

Gdiplus::Bitmap* CachedPngResource(int resourceId) {
    static std::unordered_map<int, std::unique_ptr<Gdiplus::Bitmap>> cache;
    auto found = cache.find(resourceId);
    if (found != cache.end()) {
        return found->second.get();
    }

    auto bitmap = LoadPngBitmapResource(resourceId);
    Gdiplus::Bitmap* raw = bitmap.get();
    cache.emplace(resourceId, std::move(bitmap));
    return raw;
}

bool DrawPngResourceIcon(HDC hdc, int resourceId, const RECT& r) {
    Gdiplus::Bitmap* bitmap = CachedPngResource(resourceId);
    if (!bitmap) {
        return false;
    }

    const int targetSize = std::min(RectWidth(r), RectHeight(r));
    const int x = r.left + (RectWidth(r) - targetSize) / 2;
    const int y = r.top + (RectHeight(r) - targetSize) / 2;

    Gdiplus::Graphics graphics(hdc);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    return graphics.DrawImage(bitmap, x, y, targetSize, targetSize) == Gdiplus::Ok;
}

std::vector<ToolbarButton> BuildToolbar(const OverlayState& state) {
    std::vector<ToolbarButton> buttons;
    if (!HasArea(state.selectedRect)) {
        return buttons;
    }

    constexpr int iconW = 38;
    constexpr int buttonH = 32;
    constexpr int gap = 4;
    constexpr int totalW = iconW * 10 + gap * 9;
    int x = state.selectedRect.left;
    int y = state.selectedRect.bottom + 8;
    if (x + totalW > state.frame->width) {
        x = state.frame->width - totalW - 8;
    }
    x = std::max(8, x);
    if (y + buttonH > state.frame->height) {
        y = state.selectedRect.top - buttonH - 8;
    }
    y = std::clamp(y, 8, std::max(8, state.frame->height - buttonH - 8));

    auto addTool = [&](AnnotationTool tool, const wchar_t* label, int width) {
        RECT r{x, y, x + width, y + buttonH};
        buttons.push_back(ToolbarButton{r, 0, tool, label, width});
        x += width + gap;
    };
    auto addCommand = [&](int id, const wchar_t* label, int width) {
        RECT r{x, y, x + width, y + buttonH};
        buttons.push_back(ToolbarButton{r, id, AnnotationTool::Select, label, width});
        x += width + gap;
    };

    addTool(AnnotationTool::Select, L"Move", iconW);
    addTool(AnnotationTool::Arrow, L"Arrow", iconW);
    addTool(AnnotationTool::Pen, L"Pen", iconW);
    addTool(AnnotationTool::Rectangle, L"Rect", iconW);
    addTool(AnnotationTool::Ellipse, L"Oval", iconW);
    addTool(AnnotationTool::Text, L"Text", iconW);
    addCommand(100, L"Undo", iconW);
    addCommand(101, L"Color", iconW);
    addCommand(102, L"Cancel", iconW);
    addCommand(103, L"OK", iconW);
    return buttons;
}

void DrawToolbarIcon(HDC hdc, const ToolbarButton& button, const OverlayState& state) {
    (void)state;

    const int pngResource = PngResourceForButton(button);
    if (pngResource != 0 && DrawPngResourceIcon(hdc, pngResource, button.rect)) {
        return;
    }

    const RECT r = button.rect;
    const int cx = (r.left + r.right) / 2;
    const int cy = (r.top + r.bottom) / 2;
    const COLORREF iconColor = RGB(245, 245, 245);

    HPEN iconPen = CreatePen(PS_SOLID, 2, iconColor);
    HGDIOBJ oldPen = SelectObject(hdc, iconPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

    if (button.id == 100) {
        Arc(hdc, cx - 10, cy - 9, cx + 10, cy + 9, cx - 8, cy - 5, cx + 8, cy - 5);
        MoveToEx(hdc, cx - 9, cy - 5, nullptr);
        LineTo(hdc, cx - 4, cy - 10);
        MoveToEx(hdc, cx - 9, cy - 5, nullptr);
        LineTo(hdc, cx - 3, cy - 2);
    }

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(iconPen);
}

void DrawToolbar(HDC hdc, const OverlayState& state) {
    const auto buttons = BuildToolbar(state);
    if (buttons.empty()) {
        return;
    }

    for (const auto& button : buttons) {
        const bool active = button.id == 0 && button.tool == state.activeTool;
        const bool colorButton = button.id == 101;
        const COLORREF fillColor = colorButton ? state.activeColor : (active ? RGB(42, 142, 255) : RGB(48, 48, 48));
        const int luminance = (GetRValue(fillColor) * 299 + GetGValue(fillColor) * 587 + GetBValue(fillColor) * 114) / 1000;
        const COLORREF borderColor = colorButton && luminance > 190 ? RGB(112, 112, 112) : RGB(75, 75, 75);

        HBRUSH brush = CreateSolidBrush(fillColor);
        HPEN pen = CreatePen(PS_SOLID, 1, borderColor);
        HGDIOBJ oldBrush = SelectObject(hdc, brush);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        RoundRect(hdc, button.rect.left, button.rect.top, button.rect.right, button.rect.bottom, 6, 6);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);

        DrawToolbarIcon(hdc, button, state);
    }
}

ResizeHandle HitTestResizeHandle(const RECT& r, POINT p) {
    constexpr int s = 7;
    const POINT centers[] = {
        {r.left, r.top},
        {(r.left + r.right) / 2, r.top},
        {r.right, r.top},
        {r.left, (r.top + r.bottom) / 2},
        {r.right, (r.top + r.bottom) / 2},
        {r.left, r.bottom},
        {(r.left + r.right) / 2, r.bottom},
        {r.right, r.bottom},
    };
    const ResizeHandle handles[] = {
        ResizeHandle::TopLeft,
        ResizeHandle::Top,
        ResizeHandle::TopRight,
        ResizeHandle::Left,
        ResizeHandle::Right,
        ResizeHandle::BottomLeft,
        ResizeHandle::Bottom,
        ResizeHandle::BottomRight,
    };

    for (int i = 0; i < 8; ++i) {
        RECT hit{centers[i].x - s, centers[i].y - s, centers[i].x + s, centers[i].y + s};
        if (PtInRectInclusive(hit, p)) {
            return handles[i];
        }
    }
    return ResizeHandle::None;
}

void DrawSelectionFrame(HDC hdc, const RECT& r) {
    HPEN border = CreatePen(PS_SOLID, 2, RGB(40, 142, 255));
    HGDIOBJ oldPen = SelectObject(hdc, border);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, r.left, r.top, r.right, r.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(border);

    HBRUSH handleBrush = CreateSolidBrush(RGB(245, 245, 245));
    HPEN handlePen = CreatePen(PS_SOLID, 1, RGB(40, 142, 255));
    oldBrush = SelectObject(hdc, handleBrush);
    oldPen = SelectObject(hdc, handlePen);
    constexpr int s = 4;
    const POINT handles[] = {
        {r.left, r.top}, {(r.left + r.right) / 2, r.top}, {r.right, r.top},
        {r.left, (r.top + r.bottom) / 2}, {r.right, (r.top + r.bottom) / 2},
        {r.left, r.bottom}, {(r.left + r.right) / 2, r.bottom}, {r.right, r.bottom},
    };
    for (POINT p : handles) {
        Rectangle(hdc, p.x - s, p.y - s, p.x + s + 1, p.y + s + 1);
    }
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(handlePen);
    DeleteObject(handleBrush);
}

void PaintOverlay(HWND hwnd, OverlayState& state) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);

    const int width = state.frame->width;
    const int height = state.frame->height;

    HDC drawDc = hdc;
    HDC backDc = CreateCompatibleDC(hdc);
    HBITMAP backBmp = nullptr;
    HGDIOBJ oldBackObj = nullptr;
    if (backDc) {
        backBmp = CreateCompatibleBitmap(hdc, width, height);
        if (backBmp) {
            oldBackObj = SelectObject(backDc, backBmp);
            drawDc = backDc;
        }
    }

    DrawFrameFull(drawDc, state.frame->darkPixels, width, height);

    RECT activeRect = state.selectedRect;
    if (state.action == OverlayAction::Selecting) {
        activeRect = NormalizeRect(state.dragStart, state.dragCurrent);
        activeRect = ClampToBounds(activeRect, width, height);
    }

    if (HasArea(activeRect)) {
        DrawFrameClipRect(drawDc, state.frame->pixels, width, height, activeRect);
        const int saved = SaveDC(drawDc);
        IntersectClipRect(drawDc, activeRect.left, activeRect.top, activeRect.right, activeRect.bottom);
        for (const auto& command : state.annotations) {
            DrawAnnotation(drawDc, command);
        }
        if (state.action == OverlayAction::DrawingAnnotation) {
            DrawAnnotation(drawDc, state.currentAnnotation);
        }
        if (state.textActive) {
            AnnotationCommand preview;
            preview.tool = AnnotationTool::Text;
            preview.color = state.activeColor;
            preview.strokeWidth = state.strokeWidth;
            preview.points.push_back(state.textPoint);
            preview.text = state.textBuffer.empty() ? L"|" : state.textBuffer + L"|";
            DrawAnnotation(drawDc, preview);
        }
        RestoreDC(drawDc, saved);
        DrawSelectionFrame(drawDc, activeRect);

        if (state.action != OverlayAction::Selecting) {
            wchar_t info[64]{};
            swprintf_s(info, L"%d x %d", RectWidth(activeRect), RectHeight(activeRect));
            RECT label{activeRect.left, activeRect.top - 28, activeRect.left + 100, activeRect.top - 4};
            if (label.top < 4) {
                label.top = activeRect.bottom + 8;
                label.bottom = label.top + 24;
            }
            HBRUSH brush = CreateSolidBrush(RGB(48, 48, 48));
            FillRect(drawDc, &label, brush);
            DeleteObject(brush);
            SetBkMode(drawDc, TRANSPARENT);
            SetTextColor(drawDc, RGB(245, 245, 245));
            DrawTextW(drawDc, info, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawToolbar(drawDc, state);
        }
    }

    if (drawDc != hdc) {
        BitBlt(hdc, 0, 0, width, height, drawDc, 0, 0, SRCCOPY);
    }

    if (oldBackObj) {
        SelectObject(backDc, oldBackObj);
    }
    if (backBmp) {
        DeleteObject(backBmp);
    }
    if (backDc) {
        DeleteDC(backDc);
    }

    EndPaint(hwnd, &ps);
}

void CommitTextIfNeeded(OverlayState& state) {
    if (!state.textActive) {
        return;
    }
    if (!state.textBuffer.empty()) {
        AnnotationCommand command;
        command.tool = AnnotationTool::Text;
        command.color = state.activeColor;
        command.strokeWidth = state.strokeWidth;
        command.points.push_back(state.textPoint);
        command.text = state.textBuffer;
        state.annotations.push_back(std::move(command));
    }
    state.textActive = false;
    state.textBuffer.clear();
}

bool IsDrawingTool(AnnotationTool tool) {
    return tool == AnnotationTool::Pen ||
        tool == AnnotationTool::Rectangle ||
        tool == AnnotationTool::Ellipse ||
        tool == AnnotationTool::Arrow ||
        tool == AnnotationTool::Text;
}

void AcceptOverlay(HWND hwnd, OverlayState& state) {
    CommitTextIfNeeded(state);
    state.action = OverlayAction::None;
    state.resizeHandle = ResizeHandle::None;
    state.accepted = true;
    state.finished = true;
    if (GetCapture() == hwnd) {
        ReleaseCapture();
    }
    DestroyWindow(hwnd);
}

bool HandleToolbarClick(OverlayState& state, POINT p) {
    for (const auto& button : BuildToolbar(state)) {
        if (!PtInRectInclusive(button.rect, p)) {
            continue;
        }

        CommitTextIfNeeded(state);
        if (button.id == 0) {
            state.activeTool = button.tool;
        } else if (button.id == 100) {
            if (!state.annotations.empty()) {
                state.annotations.pop_back();
            }
        } else if (button.id == 101) {
            const COLORREF colors[] = {RGB(255, 64, 64), RGB(255, 208, 64), RGB(64, 220, 120), RGB(64, 170, 255), RGB(255, 255, 255)};
            for (int i = 0; i < 5; ++i) {
                if (state.activeColor == colors[i]) {
                    state.activeColor = colors[(i + 1) % 5];
                    break;
                }
            }
        } else if (button.id == 102) {
            state.canceled = true;
            state.finished = true;
        } else if (button.id == 103) {
            state.accepted = true;
            state.finished = true;
        }
        return true;
    }
    return false;
}

LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* incoming = reinterpret_cast<OverlayState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(incoming));
        return TRUE;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
        if (state && state->frame) {
            PaintOverlay(hwnd, *state);
            return 0;
        }
        break;
    case WM_LBUTTONDBLCLK:
        if (state && state->frame && HasArea(state->selectedRect)) {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (HandleToolbarClick(*state, p)) {
                if (state->finished) {
                    DestroyWindow(hwnd);
                } else {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            const bool outsideSelection = !PtInRectInclusive(state->selectedRect, p);
            const bool nonDrawingTool = !IsDrawingTool(state->activeTool);
            if (outsideSelection || nonDrawingTool) {
                AcceptOverlay(hwnd, *state);
                return 0;
            }
        }
        break;
    case WM_LBUTTONDOWN:
        if (state && state->frame) {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            SetFocus(hwnd);
            SetCapture(hwnd);

            if (HasArea(state->selectedRect) && HandleToolbarClick(*state, p)) {
                ReleaseCapture();
                if (state->finished) {
                    DestroyWindow(hwnd);
                } else {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            if (!HasArea(state->selectedRect)) {
                CommitTextIfNeeded(*state);
                state->action = OverlayAction::Selecting;
                state->dragStart = p;
                state->dragCurrent = p;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (state->activeTool == AnnotationTool::Text && PtInRectInclusive(state->selectedRect, p)) {
                CommitTextIfNeeded(*state);
                state->textActive = true;
                state->textPoint = p;
                state->textBuffer.clear();
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            CommitTextIfNeeded(*state);
            const ResizeHandle handle = HitTestResizeHandle(state->selectedRect, p);
            if (handle != ResizeHandle::None) {
                state->action = OverlayAction::ResizingSelection;
                state->resizeHandle = handle;
                state->resizeStartRect = state->selectedRect;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (state->activeTool == AnnotationTool::Select && PtInRectInclusive(state->selectedRect, p)) {
                state->action = OverlayAction::MovingSelection;
                state->dragStart = p;
                state->selectionStartOrigin = POINT{state->selectedRect.left, state->selectedRect.top};
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (state->activeTool != AnnotationTool::Select && PtInRectInclusive(state->selectedRect, p)) {
                state->action = OverlayAction::DrawingAnnotation;
                state->currentAnnotation = AnnotationCommand{};
                state->currentAnnotation.tool = state->activeTool;
                state->currentAnnotation.color = state->activeColor;
                state->currentAnnotation.strokeWidth = state->strokeWidth;
                state->currentAnnotation.points.push_back(p);
                if (state->activeTool != AnnotationTool::Pen) {
                    state->currentAnnotation.points.push_back(p);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            ReleaseCapture();
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (state && state->frame) {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            p.x = std::clamp<LONG>(p.x, 0, state->frame->width);
            p.y = std::clamp<LONG>(p.y, 0, state->frame->height);

            if (state->action == OverlayAction::Selecting) {
                state->dragCurrent = p;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (state->action == OverlayAction::MovingSelection) {
                const int dx = p.x - state->dragStart.x;
                const int dy = p.y - state->dragStart.y;
                RECT moved{
                    state->selectionStartOrigin.x + dx,
                    state->selectionStartOrigin.y + dy,
                    state->selectionStartOrigin.x + dx + RectWidth(state->selectedRect),
                    state->selectionStartOrigin.y + dy + RectHeight(state->selectedRect)
                };
                ClampMoveRectToBounds(moved, state->frame->width, state->frame->height);
                state->selectedRect = moved;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (state->action == OverlayAction::ResizingSelection) {
                state->selectedRect = ResizeRectFromHandle(state->resizeStartRect, state->resizeHandle, p, state->frame->width, state->frame->height);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (state->action == OverlayAction::DrawingAnnotation) {
                p.x = std::clamp<LONG>(p.x, state->selectedRect.left, state->selectedRect.right);
                p.y = std::clamp<LONG>(p.y, state->selectedRect.top, state->selectedRect.bottom);
                if (state->currentAnnotation.tool == AnnotationTool::Pen) {
                    state->currentAnnotation.points.push_back(p);
                } else if (state->currentAnnotation.points.size() >= 2) {
                    state->currentAnnotation.points[1] = p;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        break;
    case WM_LBUTTONUP:
        if (state && state->frame) {
            if (state->action == OverlayAction::Selecting) {
                POINT end{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                RECT r = NormalizeRect(state->dragStart, end);
                r = ClampToBounds(r, state->frame->width, state->frame->height);
                if (HasArea(r)) {
                    state->selectedRect = r;
                }
            } else if (state->action == OverlayAction::DrawingAnnotation) {
                if (!state->currentAnnotation.points.empty()) {
                    state->annotations.push_back(state->currentAnnotation);
                }
            }
            state->action = OverlayAction::None;
            state->resizeHandle = ResizeHandle::None;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CHAR:
        if (state && state->textActive) {
            if (wParam == VK_RETURN) {
                CommitTextIfNeeded(*state);
            } else if (wParam == VK_BACK) {
                if (!state->textBuffer.empty()) {
                    state->textBuffer.pop_back();
                }
            } else if (wParam >= 32) {
                state->textBuffer.push_back(static_cast<wchar_t>(wParam));
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (state) {
            if (wParam == VK_ESCAPE) {
                state->canceled = true;
                state->finished = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'Z') {
                CommitTextIfNeeded(*state);
                if (!state->annotations.empty()) {
                    state->annotations.pop_back();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (wParam == VK_RETURN && HasArea(state->selectedRect)) {
                AcceptOverlay(hwnd, *state);
                return 0;
            }
        }
        break;
    case WM_RBUTTONDOWN:
        if (state) {
            state->canceled = true;
            state->finished = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (state) {
            state->canceled = true;
            state->finished = true;
        }
        DestroyWindow(hwnd);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::optional<OverlayState> ShowOverlayAndEdit(CapturedFrame& frame) {
    static const wchar_t* kClassName = L"PrintScreenRegionSnipOverlay";
    static bool classRegistered = false;

    if (!classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OverlayWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
        wc.hIconSm = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        if (!RegisterClassExW(&wc)) {
            return std::nullopt;
        }
        classRegistered = true;
    }

    OverlayState state{};
    state.frame = &frame;

    const int width = frame.width;
    const int height = frame.height;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kClassName,
        L"",
        WS_POPUP,
        frame.desktopRect.left,
        frame.desktopRect.top,
        width,
        height,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        &state);

    if (!hwnd) {
        return std::nullopt;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    MSG msg{};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);

        if (state.finished) {
            break;
        }
    }

    CommitTextIfNeeded(state);
    if (!state.canceled && state.accepted && HasArea(state.selectedRect)) {
        state.frame = nullptr;
        return state;
    }

    return std::nullopt;
}

CroppedImage CropFrame(const CapturedFrame& frame, RECT rect) {
    rect = ClampToBounds(rect, frame.width, frame.height);

    CroppedImage out;
    out.width = rect.right - rect.left;
    out.height = rect.bottom - rect.top;

    if (out.width <= 0 || out.height <= 0) {
        return out;
    }

    out.pixels.resize(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4);

    for (int y = 0; y < out.height; ++y) {
        const auto* src = frame.pixels.data() +
            (static_cast<size_t>(rect.top + y) * static_cast<size_t>(frame.width) + static_cast<size_t>(rect.left)) * 4;
        auto* dst = out.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(out.width) * 4;
        std::memcpy(dst, src, static_cast<size_t>(out.width) * 4);
    }

    return out;
}

void BakeAnnotations(CroppedImage& image, const std::vector<AnnotationCommand>& annotations, POINT cropOrigin) {
    if (image.width <= 0 || image.height <= 0 || image.pixels.empty() || annotations.empty()) {
        return;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = image.width;
    bmi.bmiHeader.biHeight = -image.height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) {
        return;
    }

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) {
            DeleteObject(dib);
        }
        ReleaseDC(nullptr, screenDc);
        return;
    }

    std::memcpy(bits, image.pixels.data(), image.pixels.size());
    HDC memDc = CreateCompatibleDC(screenDc);
    if (!memDc) {
        DeleteObject(dib);
        ReleaseDC(nullptr, screenDc);
        return;
    }

    HGDIOBJ old = SelectObject(memDc, dib);
    for (const auto& command : annotations) {
        DrawAnnotation(memDc, command, cropOrigin);
    }
    GdiFlush();
    std::memcpy(image.pixels.data(), bits, image.pixels.size());

    SelectObject(memDc, old);
    DeleteDC(memDc);
    DeleteObject(dib);
    ReleaseDC(nullptr, screenDc);
}

bool CopyToClipboardDib(const CroppedImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.pixels.empty()) {
        return false;
    }

    const size_t headerSize = sizeof(BITMAPINFOHEADER);
    const size_t imageSize = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4;

    HGLOBAL hMem = GlobalAlloc(GHND, headerSize + imageSize);
    if (!hMem) {
        return false;
    }

    auto* mem = static_cast<std::uint8_t*>(GlobalLock(hMem));
    if (!mem) {
        GlobalFree(hMem);
        return false;
    }

    auto* bih = reinterpret_cast<BITMAPINFOHEADER*>(mem);
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = image.width;
    bih->biHeight = image.height;  // bottom-up storage
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = static_cast<DWORD>(imageSize);
    bih->biXPelsPerMeter = 0;
    bih->biYPelsPerMeter = 0;
    bih->biClrUsed = 0;
    bih->biClrImportant = 0;

    auto* dstPixels = mem + headerSize;
    for (int y = 0; y < image.height; ++y) {
        const auto* srcRow = image.pixels.data() + static_cast<size_t>(image.height - 1 - y) * static_cast<size_t>(image.width) * 4;
        auto* dstRow = dstPixels + static_cast<size_t>(y) * static_cast<size_t>(image.width) * 4;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(image.width) * 4);
    }

    GlobalUnlock(hMem);

    if (!OpenClipboard(nullptr)) {
        GlobalFree(hMem);
        return false;
    }

    EmptyClipboard();
    if (!SetClipboardData(CF_DIB, hMem)) {
        CloseClipboard();
        GlobalFree(hMem);
        return false;
    }

    CloseClipboard();
    return true;
}

void RequestCapture() {
    if (g_captureInProgress || g_captureQueued) {
        return;
    }

    g_captureQueued = true;
    PostThreadMessageW(g_mainThreadId, WM_APP_TRIGGER_CAPTURE, 0, 0);
}

void HandleCaptureRequest() {
    POINT cursorPos{};
    GetCursorPos(&cursorPos);

    CapturedFrame frame;
    const bool captured = CaptureDesktopFrame(cursorPos, frame);

    if (!captured) {
        MessageBeep(MB_ICONHAND);
        return;
    }

    auto editResult = ShowOverlayAndEdit(frame);
    if (!editResult.has_value()) {
        return;
    }

    CroppedImage crop = CropFrame(frame, editResult->selectedRect);
    POINT cropOrigin{editResult->selectedRect.left, editResult->selectedRect.top};
    BakeAnnotations(crop, editResult->annotations, cropOrigin);
    if (CopyToClipboardDib(crop)) {
        MessageBeep(MB_OK);
    } else {
        MessageBeep(MB_ICONHAND);
    }
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (kb->vkCode == VK_SNAPSHOT) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                RequestCapture();
            }
            return 1;  // always swallow PrintScreen to avoid system snipper popup/fullscreen path
        }
    }

    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    Gdiplus::GdiplusStartupInput gdiplusInput{};
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"Failed to initialize GDI+.", L"PrintScreenRegionSnip", MB_ICONERROR);
        return 1;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_mainThreadId = GetCurrentThreadId();
    MSG queueInit{};
    PeekMessageW(&queueInit, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr, 0);
    if (!g_keyboardHook) {
        MessageBoxW(nullptr, L"Failed to install keyboard hook.", L"PrintScreenRegionSnip", MB_ICONERROR);
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 1;
    }

    RegisterHotKey(nullptr, HOTKEY_PRINTSCREEN, MOD_NOREPEAT, VK_SNAPSHOT);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_PRINTSCREEN) {
            RequestCapture();
            continue;
        }

        if (msg.message == WM_APP_TRIGGER_CAPTURE) {
            if (!g_captureInProgress) {
                g_captureQueued = false;
                g_captureInProgress = true;
                HandleCaptureRequest();
                g_captureInProgress = false;
            }
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotKey(nullptr, HOTKEY_PRINTSCREEN);
    UnhookWindowsHookEx(g_keyboardHook);
    g_keyboardHook = nullptr;
    Gdiplus::GdiplusShutdown(gdiplusToken);

    return 0;
}


