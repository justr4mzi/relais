// Relais - fullscreen Direct3D 11 viewer
// SPDX-License-Identifier: MIT
#include "app.h"
#include "shaders.h"
#include "i18n.h"

#include <d3d11.h>
#include <dxgi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <mfapi.h>
#include <shlobj.h>
// GUIDs for the screenshot encoder and the Pictures folder are defined here.
#include <initguid.h>
#include <wincodec.h>
#include <knownfolders.h>

#include <cmath>
#include <cstring>
#include <vector>

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------
// Frame handed from the capture thread to the render thread (tightly packed rows)
// ---------------------------------------------------------------------------
struct CpuFrame {
    std::vector<BYTE> data;
    PixelLayout layout = PixelLayout::NV12;
    UINT32 texW = 0, texH = 0, w = 0, h = 0;
    UINT32 rowBytes = 0;    // bytes per row of plane 0
    size_t uvOffset = 0;    // NV12 chroma plane offset
};

static UINT32 RowBytes(PixelLayout l, UINT32 w) {
    return l == PixelLayout::NV12 ? w : l == PixelLayout::YUY2 ? w * 2 : w * 4;
}

struct Shared {
    CRITICAL_SECTION lock;
    CpuFrame pending;
    bool hasPending = false;
    HANDLE frameEvent = nullptr;
    HANDLE openedEvent = nullptr;
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> dropDuplicates{true};
    std::atomic<int> dupThreshold{15};
    std::atomic<unsigned long long> captured{0}, duplicates{0};
    std::wstring error;
    bool opened = false;
    // what the capture opened
    bool sourceIsMjpeg = false, typeKnowsRange = false, typeFullRange = false, typeKnowsMatrix = false, typeIs709 = true;
    std::wstring deviceId;
    VideoMode mode;
};

static DWORD WINAPI CaptureThread(LPVOID param) {
    Shared& sh = *static_cast<Shared*>(param);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    VideoCapture cap;
    std::wstring err;
    if (!cap.Open(sh.deviceId, sh.mode, err)) {
        sh.error = err;
        sh.failed = true;
        SetEvent(sh.openedEvent);
        CoUninitialize();
        return 0;
    }
    sh.sourceIsMjpeg = cap.sourceIsMjpeg;
    sh.typeKnowsRange = cap.typeKnowsRange;
    sh.typeFullRange = cap.typeFullRange;
    sh.typeKnowsMatrix = cap.typeKnowsMatrix;
    sh.typeIs709 = cap.typeIs709;
    sh.opened = true;
    SetEvent(sh.openedEvent);

    CpuFrame work;
    const int GX = 96, GY = 54;
    std::vector<BYTE> prevSamples(GX * GY), samples(GX * GY);
    bool havePrev = false;

    while (!sh.stop) {
        FrameView fv;
        bool got = false;
        if (!cap.ReadFrame(fv, got, err)) {
            sh.error = err;
            sh.failed = true;
            SetEvent(sh.frameEvent);
            break;
        }
        if (!got) continue;
        ++sh.captured;

        // Pack the frame rows (handles padding and bottom-up RGB).
        work.layout = cap.layout;
        work.texW = cap.texWidth; work.texH = cap.texHeight;
        work.w = cap.width; work.h = cap.height;
        work.rowBytes = RowBytes(work.layout, work.texW);
        size_t plane0 = (size_t)work.rowBytes * work.texH;
        size_t total = plane0 + (work.layout == PixelLayout::NV12 ? (size_t)work.rowBytes * (work.texH / 2) : 0);
        if (work.data.size() != total) work.data.resize(total);
        for (UINT32 y = 0; y < work.texH; ++y)
            memcpy(&work.data[(size_t)y * work.rowBytes], fv.plane0 + (ptrdiff_t)fv.pitch * y, work.rowBytes);
        work.uvOffset = plane0;
        if (work.layout == PixelLayout::NV12 && fv.plane1) {
            for (UINT32 y = 0; y < work.texH / 2; ++y)
                memcpy(&work.data[plane0 + (size_t)y * work.rowBytes], fv.plane1 + (ptrdiff_t)fv.pitch * y, work.rowBytes);
        }
        cap.ReleaseFrame();

        // Duplicate detection: a 30 fps game captured at 60 fps sends every picture twice.
        // Skipping the copy gives the frame generator a clean 30 fps cadence to work with.
        bool duplicate = false;
        if (sh.dropDuplicates) {
            UINT32 rows = work.h, stepY = rows / GY, stepX = work.rowBytes / GX;
            if (stepY == 0) stepY = 1;
            if (stepX == 0) stepX = 1;
            unsigned long long diff = 0;
            for (int gy = 0; gy < GY; ++gy) {
                const BYTE* row = &work.data[(size_t)((gy * stepY + stepY / 2) % work.texH) * work.rowBytes];
                for (int gx = 0; gx < GX; ++gx) {
                    BYTE v = row[(gx * stepX + stepX / 2) % work.rowBytes];
                    BYTE& slot = samples[gy * GX + gx];
                    slot = v;
                    int d = int(v) - int(prevSamples[gy * GX + gx]);
                    diff += (unsigned long long)(d < 0 ? -d : d);
                }
            }
            double meanTenths = 10.0 * double(diff) / double(GX * GY);
            duplicate = havePrev && meanTenths < double(sh.dupThreshold);
            if (!duplicate) { prevSamples.swap(samples); havePrev = true; }
        } else {
            havePrev = false;
        }
        if (duplicate) { ++sh.duplicates; continue; }

        EnterCriticalSection(&sh.lock);
        std::swap(sh.pending, work);
        sh.hasPending = true;
        LeaveCriticalSection(&sh.lock);
        SetEvent(sh.frameEvent);
    }
    cap.Close();
    CoUninitialize();
    return 0;
}


// ---------------------------------------------------------------------------
// Direct3D 11 renderer with a Direct2D overlay for the on-screen display
// ---------------------------------------------------------------------------
typedef HRESULT(WINAPI* PFN_Compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);

struct MenuItemView { std::wstring name, value; };

// What the on-screen display shows on top of the picture.
struct OverlayContent {
    std::wstring toast;                 // short message at the top, empty = none
    std::vector<std::wstring> stats;    // lines at the top left, empty = hidden
    bool menu = false;
    int menuSel = 0;
    std::vector<MenuItemView> items;
};

struct Renderer {
    HWND hwnd = nullptr;
    bool exclusive = true;
    bool vsync = false;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;
    ID3D11RenderTargetView* backRtv = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* psDecode = nullptr;
    ID3D11PixelShader* psPicture = nullptr;
    ID3D11Buffer* cbDecode = nullptr;
    ID3D11Buffer* cbPicture = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    // input textures
    ID3D11Texture2D* tex0 = nullptr; ID3D11ShaderResourceView* srv0 = nullptr;
    ID3D11Texture2D* tex1 = nullptr; ID3D11ShaderResourceView* srv1 = nullptr;
    PixelLayout layout = PixelLayout::NV12;
    UINT32 texW = 0, texH = 0, w = 0, h = 0;
    // decoded RGB
    ID3D11Texture2D* rgb = nullptr; ID3D11RenderTargetView* rgbRtv = nullptr; ID3D11ShaderResourceView* rgbSrv = nullptr;
    bool haveFrame = false;
    UINT backW = 0, backH = 0;
    // overlay
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dw = nullptr;
    IDWriteTextFormat* fmtToast = nullptr;
    IDWriteTextFormat* fmtBody = nullptr;
    IDWriteTextFormat* fmtTitle = nullptr;
    ID2D1RenderTarget* rt2d = nullptr;
    float uiScale = 1.0f;

    struct DecodeCB { int layout, fullRange, bt709, pad; };
    struct PictureCB { float brightness, contrast, saturation, sharpness, texelX, texelY, pad0, pad1; };

    bool Init(HWND wnd, bool excl, std::wstring& err);
    bool CreateBackbuffer();
    void ResizeBuffers();
    bool EnsureInput(const CpuFrame& f);
    void Upload(const CpuFrame& f);
    void Draw(const DecodeCB& d, const PictureCB& p);
    void DrawOverlay(const OverlayContent& o);
    bool SaveScreenshot(const PictureCB& p, std::wstring& path);
    void Present() { swap->Present(vsync ? 1 : 0, 0); }
    void Shutdown();
    void CreateTextFormats();
};

bool Renderer::Init(HWND wnd, bool excl, std::wstring& err) {
    hwnd = wnd;
    exclusive = excl;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, 4, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { err = Format(T(STR_ERR_DX), hr); return false; }

    IDXGIDevice* dxgiDev = nullptr; IDXGIAdapter* adapter = nullptr; IDXGIFactory* factory = nullptr;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
    dxgiDev->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory);

    RECT rc; GetClientRect(hwnd, &rc);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width = rc.right - rc.left;
    sd.BufferDesc.Height = rc.bottom - rc.top;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.BufferCount = 2;
    if (exclusive) {
        // Classic swap chain: real exclusive fullscreen, what AFMF wants on RX 6000.
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    } else {
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    }
    hr = factory->CreateSwapChain(dev, &sd, &swap);
    if (SUCCEEDED(hr)) factory->MakeWindowAssociation(hwnd, exclusive ? 0 : DXGI_MWA_NO_ALT_ENTER);
    SafeRelease(factory); SafeRelease(adapter); SafeRelease(dxgiDev);
    if (FAILED(hr)) { err = Format(T(STR_ERR_SWAPCHAIN), hr); return false; }

    if (exclusive) {
        // Keep the screen's resolution and refresh rate: the picture is scaled by the GPU.
        DXGI_MODE_DESC md{};
        md.Width = sd.BufferDesc.Width;
        md.Height = sd.BufferDesc.Height;
        md.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        DEVMODEW dm{}; dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
            md.RefreshRate.Numerator = dm.dmDisplayFrequency;
            md.RefreshRate.Denominator = 1;
        }
        swap->ResizeTarget(&md);
        swap->SetFullscreenState(TRUE, nullptr);
    }

    // Shaders
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    PFN_Compile compile = dll ? (PFN_Compile)GetProcAddress(dll, "D3DCompile") : nullptr;
    if (!compile) { err = T(STR_ERR_D3DCOMPILER); return false; }
    auto build = [&](const char* entry, const char* target, const char* define, ID3DBlob** blob) -> bool {
        D3D_SHADER_MACRO macros[] = {{define, "1"}, {nullptr, nullptr}};
        ID3DBlob* errors = nullptr;
        HRESULT h = compile(kShaderSource, sizeof(kShaderSource) - 1, "relais.hlsl", define ? macros : nullptr, nullptr,
                            entry, target, (1u << 15) /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */, 0, blob, &errors);
        if (FAILED(h)) {
            std::string msg = errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : "";
            err = std::wstring(T(STR_ERR_SHADER)) + std::wstring(msg.begin(), msg.end());
        }
        SafeRelease(errors);
        return SUCCEEDED(h);
    };
    ID3DBlob* b = nullptr;
    if (!build("VSMain", "vs_4_0", nullptr, &b)) return false;
    dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs); SafeRelease(b);
    if (!build("PSDecode", "ps_4_0", "PASS_DECODE", &b)) return false;
    dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psDecode); SafeRelease(b);
    if (!build("PSPicture", "ps_4_0", "PASS_PICTURE", &b)) return false;
    dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psPicture); SafeRelease(b);

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = sizeof(DecodeCB);
    dev->CreateBuffer(&bd, nullptr, &cbDecode);
    bd.ByteWidth = sizeof(PictureCB);
    dev->CreateBuffer(&bd, nullptr, &cbPicture);

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&smp, &sampler);

    if (!vs || !psDecode || !psPicture || !cbDecode || !cbPicture || !sampler) { err = T(STR_ERR_DX_INCOMPLETE); return false; }

    // The overlay is optional: if Direct2D is missing, the picture still works.
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void**)&d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw);
    return CreateBackbuffer();
}

void Renderer::CreateTextFormats() {
    SafeRelease(fmtToast); SafeRelease(fmtBody); SafeRelease(fmtTitle);
    if (!dw) return;
    dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                         DWRITE_FONT_STRETCH_NORMAL, 22.0f * uiScale, L"", &fmtToast);
    dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                         DWRITE_FONT_STRETCH_NORMAL, 18.0f * uiScale, L"", &fmtBody);
    dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                         DWRITE_FONT_STRETCH_NORMAL, 24.0f * uiScale, L"", &fmtTitle);
    if (fmtToast) fmtToast->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (fmtBody) fmtBody->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
}

bool Renderer::CreateBackbuffer() {
    SafeRelease(backRtv);
    SafeRelease(rt2d);
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return false;
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    backW = d.Width; backH = d.Height;
    HRESULT hr = dev->CreateRenderTargetView(bb, nullptr, &backRtv);
    if (d2d) {
        IDXGISurface* surf = nullptr;
        if (SUCCEEDED(bb->QueryInterface(__uuidof(IDXGISurface), (void**)&surf))) {
            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
            d2d->CreateDxgiSurfaceRenderTarget(surf, &props, &rt2d);
            surf->Release();
        }
        float scale = backH / 1080.0f;
        if (scale < 0.75f) scale = 0.75f;
        if (scale != uiScale || !fmtBody) { uiScale = scale; CreateTextFormats(); }
    }
    bb->Release();
    return SUCCEEDED(hr);
}

void Renderer::ResizeBuffers() {
    if (!swap) return;
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(backRtv);
    SafeRelease(rt2d);
    ctx->Flush();
    swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, exclusive ? DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH : 0);
    CreateBackbuffer();
}

static bool MakeTexture(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, ID3D11Texture2D** tex, ID3D11ShaderResourceView** srv) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, tex))) return false;
    return SUCCEEDED(dev->CreateShaderResourceView(*tex, nullptr, srv));
}

bool Renderer::EnsureInput(const CpuFrame& f) {
    if (tex0 && f.layout == layout && f.texW == texW && f.texH == texH && f.w == w && f.h == h) return true;
    SafeRelease(srv0); SafeRelease(tex0); SafeRelease(srv1); SafeRelease(tex1);
    SafeRelease(rgbSrv); SafeRelease(rgbRtv); SafeRelease(rgb);
    layout = f.layout; texW = f.texW; texH = f.texH; w = f.w; h = f.h;
    bool ok = false;
    if (layout == PixelLayout::NV12)
        ok = MakeTexture(dev, texW, texH, DXGI_FORMAT_R8_UNORM, &tex0, &srv0) &&
             MakeTexture(dev, texW / 2, texH / 2, DXGI_FORMAT_R8G8_UNORM, &tex1, &srv1);
    else if (layout == PixelLayout::YUY2)
        ok = MakeTexture(dev, texW / 2, texH, DXGI_FORMAT_R8G8B8A8_UNORM, &tex0, &srv0);
    else
        ok = MakeTexture(dev, texW, texH, DXGI_FORMAT_B8G8R8A8_UNORM, &tex0, &srv0);
    if (!ok) return false;

    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &rgb))) return false;
    dev->CreateRenderTargetView(rgb, nullptr, &rgbRtv);
    dev->CreateShaderResourceView(rgb, nullptr, &rgbSrv);
    return rgbRtv && rgbSrv;
}

static void CopyRows(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const BYTE* src, UINT32 rowBytes, UINT32 copyBytes, UINT32 rows) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    BYTE* dst = (BYTE*)m.pData;
    for (UINT32 y = 0; y < rows; ++y) memcpy(dst + (size_t)y * m.RowPitch, src + (size_t)y * rowBytes, copyBytes);
    ctx->Unmap(tex, 0);
}

void Renderer::Upload(const CpuFrame& f) {
    if (!EnsureInput(f)) return;
    if (layout == PixelLayout::NV12) {
        CopyRows(ctx, tex0, f.data.data(), f.rowBytes, texW, texH);
        CopyRows(ctx, tex1, f.data.data() + f.uvOffset, f.rowBytes, (texW / 2) * 2, texH / 2);
    } else if (layout == PixelLayout::YUY2) {
        CopyRows(ctx, tex0, f.data.data(), f.rowBytes, (texW / 2) * 4, texH);
    } else {
        CopyRows(ctx, tex0, f.data.data(), f.rowBytes, texW * 4, texH);
    }
    haveFrame = true;
}

void Renderer::Draw(const DecodeCB& d, const PictureCB& p) {
    if (!backRtv) return;
    const float black[4] = {0, 0, 0, 1};
    ctx->ClearRenderTargetView(backRtv, black);
    if (haveFrame && rgbRtv) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(cbDecode, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &d, sizeof(d)); ctx->Unmap(cbDecode, 0); }
        if (SUCCEEDED(ctx->Map(cbPicture, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &p, sizeof(p)); ctx->Unmap(cbPicture, 0); }

        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs, nullptr, 0);

        // Pass 1: decode to RGB at the capture resolution
        D3D11_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &rgbRtv, nullptr);
        ID3D11ShaderResourceView* in[3] = {nullptr, nullptr, nullptr};
        if (layout == PixelLayout::NV12) { in[0] = srv0; in[1] = srv1; }
        else in[2] = srv0;
        ctx->PSSetShader(psDecode, nullptr, 0);
        ctx->PSSetShaderResources(0, 3, in);
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->PSSetConstantBuffers(0, 1, &cbDecode);
        ctx->Draw(3, 0);

        // Pass 2: scale into the screen, keeping the aspect ratio
        ID3D11ShaderResourceView* none[3] = {nullptr, nullptr, nullptr};
        ctx->PSSetShaderResources(0, 3, none);
        float scale = std::fmin((float)backW / w, (float)backH / h);
        float dw_ = std::floor(w * scale), dh_ = std::floor(h * scale);
        D3D11_VIEWPORT out{std::floor((backW - dw_) / 2), std::floor((backH - dh_) / 2), dw_, dh_, 0, 1};
        ctx->RSSetViewports(1, &out);
        ctx->OMSetRenderTargets(1, &backRtv, nullptr);
        ctx->PSSetShader(psPicture, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &rgbSrv);
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->PSSetConstantBuffers(0, 1, &cbPicture);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, none);
    } else {
        ctx->OMSetRenderTargets(1, &backRtv, nullptr);
    }
}

static D2D1_ROUNDED_RECT RoundRect(float l, float t, float r, float b, float radius) {
    return D2D1::RoundedRect(D2D1::RectF(l, t, r, b), radius, radius);
}

void Renderer::DrawOverlay(const OverlayContent& o) {
    if (!rt2d || !fmtBody || !fmtToast || !fmtTitle) return;
    if (o.toast.empty() && o.stats.empty() && !o.menu) return;
    const float s = uiScale;
    ID2D1SolidColorBrush *bg = nullptr, *text = nullptr, *dim = nullptr, *sel = nullptr;
    rt2d->CreateSolidColorBrush(D2D1::ColorF(0.05f, 0.06f, 0.08f, 0.82f), &bg);
    rt2d->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &text);
    rt2d->CreateSolidColorBrush(D2D1::ColorF(0.70f, 0.72f, 0.78f, 1.0f), &dim);
    rt2d->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f), &sel);
    if (!bg || !text || !dim || !sel) { SafeRelease(bg); SafeRelease(text); SafeRelease(dim); SafeRelease(sel); return; }

    rt2d->BeginDraw();
    const float W = (float)backW;

    if (!o.toast.empty()) {
        IDWriteTextLayout* tl = nullptr;
        dw->CreateTextLayout(o.toast.c_str(), (UINT32)o.toast.size(), fmtToast, W, 200.0f * s, &tl);
        if (tl) {
            DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m);
            float padX = 24 * s, padY = 12 * s;
            float left = (W - m.width) / 2 - padX, top = 36 * s;
            rt2d->FillRoundedRectangle(RoundRect(left, top, left + m.width + 2 * padX, top + m.height + 2 * padY, 14 * s), bg);
            rt2d->DrawTextLayout(D2D1::Point2F(left + padX - m.left, top + padY), tl, text);
            tl->Release();
        }
    }

    if (!o.stats.empty()) {
        float lineH = 28 * s, pad = 16 * s, width = 560 * s;
        float height = pad * 2 + lineH * o.stats.size();
        float left = 28 * s, top = 28 * s;
        rt2d->FillRoundedRectangle(RoundRect(left, top, left + width, top + height, 12 * s), bg);
        for (size_t i = 0; i < o.stats.size(); ++i) {
            const std::wstring& line = o.stats[i];
            rt2d->DrawText(line.c_str(), (UINT32)line.size(), fmtBody,
                           D2D1::RectF(left + pad, top + pad + lineH * i, left + width - pad, top + pad + lineH * (i + 1)),
                           i == 0 ? text : dim);
        }
    }

    if (o.menu) {
        float rowH = 40 * s, pad = 22 * s, width = 520 * s;
        float height = pad * 2 + 44 * s + rowH * o.items.size() + 40 * s;
        float left = 48 * s, top = (backH - height) / 2;
        rt2d->FillRoundedRectangle(RoundRect(left, top, left + width, top + height, 16 * s), bg);
        const std::wstring title = T(STR_OSD_TITLE);
        rt2d->DrawText(title.c_str(), (UINT32)title.size(), fmtTitle,
                       D2D1::RectF(left + pad, top + pad, left + width - pad, top + pad + 40 * s), text);
        float y = top + pad + 44 * s;
        for (size_t i = 0; i < o.items.size(); ++i) {
            if ((int)i == o.menuSel)
                rt2d->FillRoundedRectangle(RoundRect(left + pad / 2, y, left + width - pad / 2, y + rowH, 8 * s), sel);
            const auto& it = o.items[i];
            rt2d->DrawText(it.name.c_str(), (UINT32)it.name.size(), fmtBody,
                           D2D1::RectF(left + pad, y + 8 * s, left + width * 0.55f, y + rowH), (int)i == o.menuSel ? text : dim);
            std::wstring value = (int)i == o.menuSel ? L"‹  " + it.value + L"  ›" : it.value;
            rt2d->DrawText(value.c_str(), (UINT32)value.size(), fmtBody,
                           D2D1::RectF(left + width * 0.55f, y + 8 * s, left + width - pad, y + rowH), text);
            y += rowH;
        }
        const std::wstring hint = T(STR_OSD_MENU_HINT);
        rt2d->DrawText(hint.c_str(), (UINT32)hint.size(), fmtBody,
                       D2D1::RectF(left + pad, y + 10 * s, left + width - pad, y + 40 * s), dim);
    }

    if (rt2d->EndDraw() == D2DERR_RECREATE_TARGET) SafeRelease(rt2d);
    SafeRelease(bg); SafeRelease(text); SafeRelease(dim); SafeRelease(sel);
}

// Renders the current picture (with the picture settings, without the overlay)
// at the capture resolution and writes it as a PNG in Pictures\Relais.
bool Renderer::SaveScreenshot(const PictureCB& p, std::wstring& path) {
    if (!haveFrame || !rgbSrv) return false;
    ID3D11Texture2D* target = nullptr; ID3D11RenderTargetView* rtv = nullptr; ID3D11Texture2D* staging = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &target)) && SUCCEEDED(dev->CreateRenderTargetView(target, nullptr, &rtv));
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &staging));
    std::vector<BYTE> pixels;
    if (ok) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(cbPicture, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &p, sizeof(p)); ctx->Unmap(cbPicture, 0); }
        D3D11_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(psPicture, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &rgbSrv);
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->PSSetConstantBuffers(0, 1, &cbPicture);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
        ctx->CopyResource(staging, target);
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            pixels.resize((size_t)w * h * 4);
            for (UINT32 y = 0; y < h; ++y) {
                const BYTE* src = (const BYTE*)m.pData + (size_t)y * m.RowPitch;
                BYTE* dst = &pixels[(size_t)y * w * 4];
                for (UINT32 x = 0; x < w; ++x) {   // RGBA -> BGRA, opaque
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = 255;
                }
            }
            ctx->Unmap(staging, 0);
        } else ok = false;
    }
    SafeRelease(staging); SafeRelease(rtv); SafeRelease(target);
    if (!ok || pixels.empty()) return false;

    PWSTR pictures = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures))) return false;
    std::wstring dir = std::wstring(pictures) + L"\\Relais";
    CoTaskMemFree(pictures);
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t; GetLocalTime(&t);
    path = dir + Format(L"\\Relais_%04d-%02d-%02d_%02d-%02d-%02d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);

    IWICImagingFactory* wic = nullptr; IWICStream* stream = nullptr; IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), (void**)&wic);
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(w, h);
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&pf);
    if (SUCCEEDED(hr)) hr = frame->WritePixels(h, w * 4, (UINT)pixels.size(), pixels.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    SafeRelease(frame); SafeRelease(enc); SafeRelease(stream); SafeRelease(wic);
    return SUCCEEDED(hr);
}

void Renderer::Shutdown() {
    if (swap) swap->SetFullscreenState(FALSE, nullptr);
    if (ctx) { ctx->ClearState(); ctx->Flush(); }
    SafeRelease(rt2d);
    SafeRelease(fmtToast); SafeRelease(fmtBody); SafeRelease(fmtTitle);
    SafeRelease(dw); SafeRelease(d2d);
    SafeRelease(srv0); SafeRelease(tex0); SafeRelease(srv1); SafeRelease(tex1);
    SafeRelease(rgbSrv); SafeRelease(rgbRtv); SafeRelease(rgb);
    SafeRelease(sampler); SafeRelease(cbPicture); SafeRelease(cbDecode);
    SafeRelease(psPicture); SafeRelease(psDecode); SafeRelease(vs);
    SafeRelease(backRtv); SafeRelease(swap); SafeRelease(ctx); SafeRelease(dev);
}

// ---------------------------------------------------------------------------
// Viewer window, hotkeys and the settings menu
// ---------------------------------------------------------------------------
enum MenuItem { MI_RANGE, MI_MATRIX, MI_BRIGHT, MI_CONTRAST, MI_SAT, MI_SHARP, MI_DUP, MI_THRESH, MI_VSYNC, MI_VOLUME, MI_COUNT };

struct ViewerState {
    Settings* s = nullptr;
    Shared* sh = nullptr;
    Renderer* r = nullptr;
    AudioPassthrough* audio = nullptr;
    bool quit = false;
    bool resize = false;
    bool redraw = false;
    bool menu = false;
    int menuSel = 0;
    bool stats = false;
    bool screenshot = false;
    std::wstring toast;
    ULONGLONG toastUntil = 0;
    // colour settings actually used (Auto resolved)
    int fullRange = 1, bt709 = 1;
};

static void Toast(ViewerState* st, const std::wstring& text, DWORD ms = 1600) {
    st->toast = text;
    st->toastUntil = GetTickCount64() + ms;
    st->redraw = true;
}

static std::wstring RangeName(const ViewerState* st) {
    std::wstring resolved = st->fullRange ? T(STR_SHORT_FULL) : T(STR_SHORT_LIMITED);
    switch (st->s->range) {
    case RANGE_LIMITED: return T(STR_SHORT_LIMITED);
    case RANGE_FULL: return T(STR_SHORT_FULL);
    default: return std::wstring(T(STR_RANGE_AUTO)) + L" (" + resolved + L")";
    }
}

static std::wstring MatrixName(const ViewerState* st) {
    std::wstring resolved = st->bt709 ? L"BT.709" : L"BT.601";
    switch (st->s->matrix) {
    case MATRIX_601: return L"BT.601";
    case MATRIX_709: return L"BT.709";
    default: return std::wstring(T(STR_MATRIX_AUTO)) + L" (" + resolved + L")";
    }
}

static MenuItemView MenuView(const ViewerState* st, int item) {
    const Settings& s = *st->s;
    switch (item) {
    case MI_RANGE: return {T(STR_RANGE), RangeName(st)};
    case MI_MATRIX: return {T(STR_MATRIX), MatrixName(st)};
    case MI_BRIGHT: return {T(STR_BRIGHTNESS), Format(L"%+d", s.brightness)};
    case MI_CONTRAST: return {T(STR_CONTRAST), Format(L"%d %%", s.contrast)};
    case MI_SAT: return {T(STR_SATURATION), Format(L"%d %%", s.saturation)};
    case MI_SHARP: return {T(STR_SHARPNESS), Format(L"%d", s.sharpness)};
    case MI_DUP: return {T(STR_SKIP_SHORT), s.dropDuplicates ? T(STR_ON) : T(STR_OFF)};
    case MI_THRESH: return {T(STR_THRESHOLD), Format(L"%.1f", s.dupThreshold / 10.0)};
    case MI_VSYNC: return {L"VSync", s.vsync ? T(STR_ON) : T(STR_OFF)};
    case MI_VOLUME: return {T(STR_VOLUME), Format(L"%d %%", s.volume)};
    }
    return {};
}

// Changes one setting by one step (dir = -1 or +1) and applies it right away.
static void Adjust(ViewerState* st, int item, int dir) {
    Settings& s = *st->s;
    auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; };
    switch (item) {
    case MI_RANGE: s.range = (s.range + 3 + dir) % 3; break;
    case MI_MATRIX: s.matrix = (s.matrix + 3 + dir) % 3; break;
    case MI_BRIGHT: s.brightness = clamp(s.brightness + 5 * dir, -100, 100); break;
    case MI_CONTRAST: s.contrast = clamp(s.contrast + 5 * dir, 0, 200); break;
    case MI_SAT: s.saturation = clamp(s.saturation + 5 * dir, 0, 200); break;
    case MI_SHARP: s.sharpness = clamp(s.sharpness + 5 * dir, 0, 100); break;
    case MI_DUP: s.dropDuplicates = !s.dropDuplicates; st->sh->dropDuplicates = s.dropDuplicates; break;
    case MI_THRESH: s.dupThreshold = clamp(s.dupThreshold + dir, 1, 60); st->sh->dupThreshold = s.dupThreshold; break;
    case MI_VSYNC: s.vsync = !s.vsync; st->r->vsync = s.vsync; break;
    case MI_VOLUME: s.volume = clamp(s.volume + 5 * dir, 0, 100); st->audio->SetVolume(s.volume / 100.0f); break;
    }
    st->redraw = true;
}

// Hotkeys outside the menu show what they changed for a moment.
static void QuickChange(ViewerState* st, int item, int dir) {
    Adjust(st, item, dir);
    MenuItemView v = MenuView(st, item);
    Toast(st, v.name + L" : " + v.value);
}

static LRESULT CALLBACK ViewerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ViewerState* st = (ViewerState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        return 0;
    case WM_SIZE:
        if (st) st->resize = true;
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
        break;
    case WM_KEYDOWN:
        if (!st) break;
        if (st->menu) {
            switch (wp) {
            case VK_UP: st->menuSel = (st->menuSel + MI_COUNT - 1) % MI_COUNT; st->redraw = true; break;
            case VK_DOWN: st->menuSel = (st->menuSel + 1) % MI_COUNT; st->redraw = true; break;
            case VK_LEFT: Adjust(st, st->menuSel, -1); break;
            case VK_RIGHT: case VK_RETURN: case VK_SPACE: Adjust(st, st->menuSel, +1); break;
            case VK_ESCAPE: case VK_F2: case VK_TAB: st->menu = false; st->redraw = true; break;
            case VK_F1: st->stats = !st->stats; st->redraw = true; break;
            case VK_F12: st->screenshot = true; break;
            }
            return 0;
        }
        switch (wp) {
        case VK_ESCAPE: st->quit = true; break;
        case VK_F2: case VK_TAB: st->menu = true; st->toast.clear(); st->redraw = true; break;
        case VK_F1: st->stats = !st->stats; st->redraw = true; break;
        case VK_F12: st->screenshot = true; break;
        case 'D': QuickChange(st, MI_DUP, +1); break;
        case 'R': QuickChange(st, MI_RANGE, +1); break;
        case 'M': QuickChange(st, MI_MATRIX, +1); break;
        case 'V': QuickChange(st, MI_VSYNC, +1); break;
        case VK_OEM_PLUS: case VK_ADD: QuickChange(st, MI_BRIGHT, +1); break;
        case VK_OEM_MINUS: case VK_SUBTRACT: QuickChange(st, MI_BRIGHT, -1); break;
        }
        return 0;
    case WM_CLOSE:
        if (st) st->quit = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct MonitorSearch { const std::wstring* name; HMONITOR found; };
static BOOL CALLBACK FindMonitorProc(HMONITOR m, HDC, LPRECT, LPARAM lp) {
    MonitorSearch* ms = (MonitorSearch*)lp;
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(m, &mi) && *ms->name == mi.szDevice) { ms->found = m; return FALSE; }
    return TRUE;
}

static HMONITOR PickMonitor(const Settings& s) {
    if (!s.monitor.empty()) {
        MonitorSearch ms{&s.monitor, nullptr};
        EnumDisplayMonitors(nullptr, nullptr, FindMonitorProc, (LPARAM)&ms);
        if (ms.found) return ms.found;
    }
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

SessionStats RunViewer(HINSTANCE inst, Settings& s, const VideoMode& mode) {
    SessionStats stats;
    // On the heap: if the capture thread is stuck inside the driver we leak it instead of crashing.
    Shared* shp = new Shared;
    Shared& sh = *shp;
    InitializeCriticalSection(&sh.lock);
    sh.frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    sh.openedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    sh.deviceId = s.videoDevice;
    sh.mode = mode;
    sh.dropDuplicates = s.dropDuplicates;
    sh.dupThreshold = s.dupThreshold;

    HANDLE capThread = CreateThread(nullptr, 0, CaptureThread, &sh, 0, nullptr);
    WaitForSingleObject(sh.openedEvent, 15000);
    if (!sh.opened) {
        sh.stop = true;
        stats.error = sh.error.empty() ? T(STR_ERR_NO_RESPONSE) : sh.error;
        if (WaitForSingleObject(capThread, 5000) == WAIT_OBJECT_0) {
            CloseHandle(sh.frameEvent); CloseHandle(sh.openedEvent);
            DeleteCriticalSection(&sh.lock);
            delete shp;
        }
        CloseHandle(capThread);
        return stats;
    }

    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(PickMonitor(s), &mi);
    WNDCLASSW wc{};
    wc.lpfnWndProc = ViewerProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"RelaisViewer";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    Renderer r;
    r.vsync = s.vsync;
    AudioPassthrough audio;
    audio.SetVolume(s.volume / 100.0f);
    ViewerState st;
    st.s = &s; st.sh = &sh; st.r = &r; st.audio = &audio;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Relais", WS_POPUP | WS_VISIBLE,
                                mi.rcMonitor.left, mi.rcMonitor.top,
                                mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                                nullptr, nullptr, inst, &st);
    SetForegroundWindow(hwnd);

    std::wstring err;
    bool ok = r.Init(hwnd, s.exclusive, err);
    if (ok && !s.audioDevice.empty() && s.audioDevice != L"none") {
        std::wstring aerr;
        if (!audio.Start(s.audioDevice, aerr)) stats.error = std::wstring(T(STR_ERR_SOUND)) + aerr;
    }
    if (!ok) stats.error = err;
    Toast(&st, T(STR_OSD_HINT), 4000);

    CpuFrame frame;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    ULONGLONG lastFresh = 0, rateTick = GetTickCount64();
    unsigned long long rateCap = 0, rateShown = 0, rateDup = 0;
    double capRate = 0, shownRate = 0, dupRate = 0;

    while (ok && !st.quit) {
        MsgWaitForMultipleObjects(1, &sh.frameEvent, FALSE, 50, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) st.quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (st.quit) break;
        if (sh.failed) { stats.error = sh.error; break; }
        if (st.resize) { st.resize = false; r.ResizeBuffers(); st.redraw = true; }

        ULONGLONG now = GetTickCount64();
        if (now - rateTick >= 1000) {
            double dt = (now - rateTick) / 1000.0;
            unsigned long long c = sh.captured, d = sh.duplicates;
            capRate = (c - rateCap) / dt; dupRate = (d - rateDup) / dt; shownRate = (stats.presented - rateShown) / dt;
            rateCap = c; rateDup = d; rateShown = stats.presented; rateTick = now;
        }
        // The overlay only forces an extra frame when the picture is still, so it never
        // adds repeated frames to a moving picture (that would confuse frame generation).
        bool still = now - lastFresh > 120;
        if (!st.toast.empty() && now >= st.toastUntil) { st.toast.clear(); if (still) st.redraw = true; }
        if (st.stats && still && now - rateTick < 60) st.redraw = true;

        bool fresh = false;
        EnterCriticalSection(&sh.lock);
        if (sh.hasPending) { std::swap(frame, sh.pending); sh.hasPending = false; fresh = true; }
        LeaveCriticalSection(&sh.lock);
        if (fresh) { r.Upload(frame); lastFresh = now; }

        // Resolve "Auto" colour settings from what the card reports.
        if (s.range == RANGE_FULL) st.fullRange = 1;
        else if (s.range == RANGE_LIMITED) st.fullRange = 0;
        else if (r.layout == PixelLayout::RGB32) st.fullRange = 1;
        else if (sh.typeKnowsRange) st.fullRange = sh.typeFullRange ? 1 : 0;
        else st.fullRange = sh.sourceIsMjpeg ? 1 : 0;   // JPEG is full range
        if (s.matrix == MATRIX_709) st.bt709 = 1;
        else if (s.matrix == MATRIX_601) st.bt709 = 0;
        else if (sh.typeKnowsMatrix) st.bt709 = sh.typeIs709 ? 1 : 0;
        else st.bt709 = sh.sourceIsMjpeg ? 0 : (r.h >= 720 ? 1 : 0);  // JPEG uses BT.601

        Renderer::PictureCB p{};
        p.brightness = s.brightness / 200.0f;
        p.contrast = s.contrast / 100.0f;
        p.saturation = s.saturation / 100.0f;
        p.sharpness = s.sharpness / 100.0f;
        p.texelX = r.w ? 1.0f / r.w : 0.0f;
        p.texelY = r.h ? 1.0f / r.h : 0.0f;

        if (st.screenshot) {
            st.screenshot = false;
            std::wstring path;
            Toast(&st, r.SaveScreenshot(p, path) ? T(STR_SCREENSHOT_SAVED) : T(STR_SCREENSHOT_FAILED), 2500);
        }
        if (!fresh && !st.redraw) continue;
        st.redraw = false;

        Renderer::DecodeCB d{};
        d.layout = (int)r.layout;
        d.fullRange = st.fullRange;
        d.bt709 = st.bt709;
        r.Draw(d, p);

        OverlayContent o;
        o.toast = st.toast;
        if (st.stats) {
            const wchar_t* sub = r.layout == PixelLayout::NV12 ? L"NV12" : r.layout == PixelLayout::YUY2 ? L"YUY2" : L"RGB32";
            std::wstring src = mode.subtype == MFVideoFormat_MJPG ? std::wstring(L"MJPG → ") + sub : std::wstring(sub);
            o.stats.push_back(Format(T(STR_STATS_SOURCE), r.w, r.h, src.c_str(), mode.fps()));
            o.stats.push_back(Format(T(STR_STATS_RATES), capRate, shownRate, dupRate));
            o.stats.push_back(Format(T(STR_STATS_COLOUR), RangeName(&st).c_str(), MatrixName(&st).c_str()));
        }
        o.menu = st.menu;
        o.menuSel = st.menuSel;
        if (st.menu) for (int i = 0; i < MI_COUNT; ++i) o.items.push_back(MenuView(&st, i));
        r.DrawOverlay(o);

        r.Present();
        if (fresh) ++stats.presented;
    }

    QueryPerformanceCounter(&t1);
    stats.seconds = double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart);
    audio.Stop();
    r.Shutdown();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);

    sh.stop = true;
    bool ended = WaitForSingleObject(capThread, 5000) == WAIT_OBJECT_0;
    CloseHandle(capThread);
    stats.captured = sh.captured;
    stats.duplicates = sh.duplicates;
    if (stats.error.empty() && sh.failed) stats.error = sh.error;
    if (ended) {
        CloseHandle(sh.frameEvent);
        CloseHandle(sh.openedEvent);
        DeleteCriticalSection(&sh.lock);
        delete shp;
    }
    return stats;
}
