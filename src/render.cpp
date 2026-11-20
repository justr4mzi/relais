// Relais - fullscreen Direct3D 11 viewer
// SPDX-License-Identifier: MIT
#include "app.h"
#include "shaders.h"

#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>

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
// Direct3D 11 renderer
// ---------------------------------------------------------------------------
typedef HRESULT(WINAPI* PFN_Compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);

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

    struct DecodeCB { int layout, fullRange, bt709, pad; };
    struct PictureCB { float brightness, contrast, saturation, sharpness, texelX, texelY, pad0, pad1; };

    bool Init(HWND wnd, bool excl, std::wstring& err);
    bool CreateBackbuffer();
    void ResizeBuffers();
    bool EnsureInput(const CpuFrame& f);
    void Upload(const CpuFrame& f);
    void Draw(const DecodeCB& d, const PictureCB& p);
    void Present() { swap->Present(vsync ? 1 : 0, 0); }
    void Shutdown();
};

bool Renderer::Init(HWND wnd, bool excl, std::wstring& err) {
    hwnd = wnd;
    exclusive = excl;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, 4, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { err = Format(L"DirectX 11 n'a pas pu démarrer (0x%08X).", hr); return false; }

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
    if (exclusive) {
        // Classic swap chain: real exclusive fullscreen, what AFMF wants on RX 6000.
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    } else {
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    }
    hr = factory->CreateSwapChain(dev, &sd, &swap);
    if (SUCCEEDED(hr)) factory->MakeWindowAssociation(hwnd, exclusive ? 0 : DXGI_MWA_NO_ALT_ENTER);
    SafeRelease(factory); SafeRelease(adapter); SafeRelease(dxgiDev);
    if (FAILED(hr)) { err = Format(L"Impossible de créer la chaîne d'affichage (0x%08X).", hr); return false; }

    if (exclusive) {
        // Keep the desktop resolution and refresh rate: the picture is scaled by the GPU.
        DXGI_MODE_DESC md{};
        md.Width = sd.BufferDesc.Width;
        md.Height = sd.BufferDesc.Height;
        md.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        DEVMODEW dm{}; dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
            md.RefreshRate.Numerator = dm.dmDisplayFrequency;
            md.RefreshRate.Denominator = 1;
        }
        swap->ResizeTarget(&md);
        swap->SetFullscreenState(TRUE, nullptr);
    }

    // Shaders
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    PFN_Compile compile = dll ? (PFN_Compile)GetProcAddress(dll, "D3DCompile") : nullptr;
    if (!compile) { err = L"d3dcompiler_47.dll est introuvable (Windows 10 ou 11 requis)."; return false; }
    auto build = [&](const char* entry, const char* target, const char* define, ID3DBlob** blob) -> bool {
        D3D_SHADER_MACRO macros[] = {{define, "1"}, {nullptr, nullptr}};
        ID3DBlob* errors = nullptr;
        HRESULT h = compile(kShaderSource, sizeof(kShaderSource) - 1, "relais.hlsl", define ? macros : nullptr, nullptr,
                            entry, target, (1u << 15) /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */, 0, blob, &errors);
        if (FAILED(h)) {
            std::string msg = errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : "";
            err = L"Erreur de shader : " + std::wstring(msg.begin(), msg.end());
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

    if (!vs || !psDecode || !psPicture || !cbDecode || !cbPicture || !sampler) { err = L"Initialisation DirectX incomplète."; return false; }
    return CreateBackbuffer();
}

bool Renderer::CreateBackbuffer() {
    SafeRelease(backRtv);
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return false;
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    backW = d.Width; backH = d.Height;
    HRESULT hr = dev->CreateRenderTargetView(bb, nullptr, &backRtv);
    bb->Release();
    return SUCCEEDED(hr);
}

void Renderer::ResizeBuffers() {
    if (!swap) return;
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(backRtv);
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
        ctx->PSSetConstantBuffers(0, 1, &cbDecode);
        ctx->Draw(3, 0);

        // Pass 2: scale into the screen, keeping the aspect ratio
        ID3D11ShaderResourceView* none[3] = {nullptr, nullptr, nullptr};
        ctx->PSSetShaderResources(0, 3, none);
        float scale = std::fmin((float)backW / w, (float)backH / h);
        float dw = std::floor(w * scale), dh = std::floor(h * scale);
        D3D11_VIEWPORT out{std::floor((backW - dw) / 2), std::floor((backH - dh) / 2), dw, dh, 0, 1};
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

void Renderer::Shutdown() {
    if (swap) swap->SetFullscreenState(FALSE, nullptr);
    if (ctx) { ctx->ClearState(); ctx->Flush(); }
    SafeRelease(srv0); SafeRelease(tex0); SafeRelease(srv1); SafeRelease(tex1);
    SafeRelease(rgbSrv); SafeRelease(rgbRtv); SafeRelease(rgb);
    SafeRelease(sampler); SafeRelease(cbPicture); SafeRelease(cbDecode);
    SafeRelease(psPicture); SafeRelease(psDecode); SafeRelease(vs);
    SafeRelease(backRtv); SafeRelease(swap); SafeRelease(ctx); SafeRelease(dev);
}

// ---------------------------------------------------------------------------
// Viewer window
// ---------------------------------------------------------------------------
struct ViewerState {
    Settings* s = nullptr;
    Shared* sh = nullptr;
    Renderer* r = nullptr;
    bool quit = false;
    bool resize = false;
    bool redraw = false;
};

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
        switch (wp) {
        case VK_ESCAPE: st->quit = true; break;
        case 'D':
            st->s->dropDuplicates = !st->s->dropDuplicates;
            st->sh->dropDuplicates = st->s->dropDuplicates;
            break;
        case 'R': st->s->range = (st->s->range + 1) % 3; st->redraw = true; break;
        case 'M': st->s->matrix = (st->s->matrix + 1) % 3; st->redraw = true; break;
        case 'V': st->s->vsync = !st->s->vsync; st->r->vsync = st->s->vsync; break;
        case VK_OEM_PLUS: case VK_ADD: st->s->brightness = std::min(100, st->s->brightness + 5); st->redraw = true; break;
        case VK_OEM_MINUS: case VK_SUBTRACT: st->s->brightness = std::max(-100, st->s->brightness - 5); st->redraw = true; break;
        }
        return 0;
    case WM_CLOSE:
        if (st) st->quit = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
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
        stats.error = sh.error.empty() ? L"La carte de capture ne répond pas." : sh.error;
        if (WaitForSingleObject(capThread, 5000) == WAIT_OBJECT_0) {
            CloseHandle(sh.frameEvent); CloseHandle(sh.openedEvent);
            DeleteCriticalSection(&sh.lock);
            delete shp;
        }
        CloseHandle(capThread);
        return stats;
    }

    // Window on the monitor where the launcher is (hidden, but it keeps its position).
    HWND launcher = FindWindowW(L"RelaisMain", nullptr);
    HMONITOR mon = launcher ? MonitorFromWindow(launcher, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    WNDCLASSW wc{};
    wc.lpfnWndProc = ViewerProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"RelaisViewer";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    Renderer r;
    r.vsync = s.vsync;
    ViewerState st;
    st.s = &s; st.sh = &sh; st.r = &r;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Relais", WS_POPUP | WS_VISIBLE,
                                mi.rcMonitor.left, mi.rcMonitor.top,
                                mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                                nullptr, nullptr, inst, &st);
    SetForegroundWindow(hwnd);

    std::wstring err;
    AudioPassthrough audio;
    bool ok = r.Init(hwnd, s.exclusive, err);
    if (ok && !s.audioDevice.empty()) {
        std::wstring aerr;
        if (!audio.Start(s.audioDevice, aerr)) stats.error = L"Son : " + aerr;
    }
    if (!ok) stats.error = err;

    CpuFrame frame;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    while (ok && !st.quit) {
        MsgWaitForMultipleObjects(1, &sh.frameEvent, FALSE, 100, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) st.quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (st.quit) break;
        if (sh.failed) { stats.error = sh.error; break; }
        if (st.resize) { st.resize = false; r.ResizeBuffers(); st.redraw = true; }

        bool fresh = false;
        EnterCriticalSection(&sh.lock);
        if (sh.hasPending) { std::swap(frame, sh.pending); sh.hasPending = false; fresh = true; }
        LeaveCriticalSection(&sh.lock);
        if (fresh) r.Upload(frame);
        if (!fresh && !st.redraw) continue;
        st.redraw = false;

        // Resolve "Auto" colour settings from what the card reports.
        Renderer::DecodeCB d{};
        d.layout = (int)r.layout;
        if (s.range == RANGE_FULL) d.fullRange = 1;
        else if (s.range == RANGE_LIMITED) d.fullRange = 0;
        else if (r.layout == PixelLayout::RGB32) d.fullRange = 1;
        else if (sh.typeKnowsRange) d.fullRange = sh.typeFullRange ? 1 : 0;
        else d.fullRange = sh.sourceIsMjpeg ? 1 : 0;   // JPEG is full range
        if (s.matrix == MATRIX_709) d.bt709 = 1;
        else if (s.matrix == MATRIX_601) d.bt709 = 0;
        else if (sh.typeKnowsMatrix) d.bt709 = sh.typeIs709 ? 1 : 0;
        else d.bt709 = sh.sourceIsMjpeg ? 0 : (r.h >= 720 ? 1 : 0);  // JPEG uses BT.601

        Renderer::PictureCB p{};
        p.brightness = s.brightness / 200.0f;
        p.contrast = s.contrast / 100.0f;
        p.saturation = s.saturation / 100.0f;
        p.sharpness = s.sharpness / 100.0f;
        p.texelX = r.w ? 1.0f / r.w : 0.0f;
        p.texelY = r.h ? 1.0f / r.h : 0.0f;

        r.Draw(d, p);
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
