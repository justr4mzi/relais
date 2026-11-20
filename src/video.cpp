// Relais - Media Foundation capture
// SPDX-License-Identifier: MIT
#include "app.h"

#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    va_end(ap);
    buf[1023] = 0;
    return buf;
}

static std::wstring SubtypeName(const GUID& g) {
    if (g == MFVideoFormat_MJPG) return L"MJPG";
    if (g == MFVideoFormat_NV12) return L"NV12";
    if (g == MFVideoFormat_YUY2) return L"YUY2";
    if (g == MFVideoFormat_RGB32) return L"RGB32";
    if (g == MFVideoFormat_RGB24) return L"RGB24";
    if (g == MFVideoFormat_I420) return L"I420";
    if (g == MFVideoFormat_UYVY) return L"UYVY";
    if (g == MFVideoFormat_H264) return L"H264";
    if (g == MFVideoFormat_P010) return L"P010";
    // FourCC stored in Data1
    wchar_t cc[5] = {0};
    for (int i = 0; i < 4; ++i) {
        wchar_t c = (wchar_t)((g.Data1 >> (8 * i)) & 0xFF);
        cc[i] = (c >= 32 && c < 127) ? c : L'?';
    }
    return cc;
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------
std::vector<DeviceInfo> EnumVideoDevices() {
    std::vector<DeviceInfo> out;
    IMFAttributes* attr = nullptr;
    if (FAILED(MFCreateAttributes(&attr, 1))) return out;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(MFEnumDeviceSources(attr, &devs, &count))) {
        for (UINT32 i = 0; i < count; ++i) {
            WCHAR* name = nullptr; WCHAR* link = nullptr; UINT32 len = 0;
            DeviceInfo d;
            if (SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len))) {
                d.name = name; CoTaskMemFree(name);
            }
            if (SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len))) {
                d.id = link; CoTaskMemFree(link);
            }
            if (!d.id.empty()) out.push_back(d);
            devs[i]->Release();
        }
        CoTaskMemFree(devs);
    }
    attr->Release();
    return out;
}

std::vector<DeviceInfo> EnumAudioDevices() {
    std::vector<DeviceInfo> out;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)))
        return out;
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(col->Item(i, &dev))) continue;
            DeviceInfo d;
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id))) { d.id = id; CoTaskMemFree(id); }
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT v; PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) d.name = v.pwszVal;
                PropVariantClear(&v);
                ps->Release();
            }
            if (!d.id.empty()) out.push_back(d);
            dev->Release();
        }
        col->Release();
    }
    en->Release();
    return out;
}

static HRESULT CreateVideoSource(const std::wstring& id, IMFMediaSource** src) {
    IMFAttributes* attr = nullptr;
    HRESULT hr = MFCreateAttributes(&attr, 2);
    if (FAILED(hr)) return hr;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, id.c_str());
    hr = MFCreateDeviceSource(attr, src);
    attr->Release();
    return hr;
}

static HRESULT GetTypeHandler(IMFMediaSource* src, IMFMediaTypeHandler** handler) {
    IMFPresentationDescriptor* pd = nullptr;
    IMFStreamDescriptor* sd = nullptr;
    BOOL selected = FALSE;
    HRESULT hr = src->CreatePresentationDescriptor(&pd);
    if (SUCCEEDED(hr)) hr = pd->GetStreamDescriptorByIndex(0, &selected, &sd);
    if (SUCCEEDED(hr)) hr = sd->GetMediaTypeHandler(handler);
    SafeRelease(sd);
    SafeRelease(pd);
    return hr;
}

std::vector<VideoMode> EnumVideoModes(const std::wstring& deviceId) {
    std::vector<VideoMode> out;
    IMFMediaSource* src = nullptr;
    if (FAILED(CreateVideoSource(deviceId, &src))) return out;
    IMFMediaTypeHandler* h = nullptr;
    if (SUCCEEDED(GetTypeHandler(src, &h))) {
        DWORD n = 0;
        h->GetMediaTypeCount(&n);
        for (DWORD i = 0; i < n; ++i) {
            IMFMediaType* t = nullptr;
            if (FAILED(h->GetMediaTypeByIndex(i, &t))) continue;
            VideoMode m;
            m.typeIndex = i;
            t->GetGUID(MF_MT_SUBTYPE, &m.subtype);
            MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &m.width, &m.height);
            MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &m.fpsNum, &m.fpsDen);
            t->Release();
            if (!m.width || !m.height || !m.fpsDen) continue;
            m.label = Format(L"%u x %u @ %.0f fps (%s)", m.width, m.height, m.fps(), SubtypeName(m.subtype).c_str());
            bool dup = false;
            for (auto& o : out) if (o.label == m.label) { dup = true; break; }
            if (!dup) out.push_back(m);
        }
        h->Release();
    }
    src->Shutdown();
    src->Release();
    // Biggest picture first, then highest frame rate, then uncompressed before MJPEG.
    std::stable_sort(out.begin(), out.end(), [](const VideoMode& a, const VideoMode& b) {
        UINT64 pa = UINT64(a.width) * a.height, pb = UINT64(b.width) * b.height;
        if (pa != pb) return pa > pb;
        if (a.fps() != b.fps()) return a.fps() > b.fps();
        bool ma = a.subtype == MFVideoFormat_MJPG, mb = b.subtype == MFVideoFormat_MJPG;
        return !ma && mb;
    });
    return out;
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------
bool VideoCapture::Open(const std::wstring& deviceId, const VideoMode& mode, std::wstring& error) {
    Close();
    HRESULT hr = CreateVideoSource(deviceId, &source_);
    if (FAILED(hr)) { error = Format(L"Impossible d'ouvrir la carte de capture (0x%08X). Elle est peut-être utilisée par une autre appli (OBS, Discord...).", hr); return false; }

    // Pick the device format chosen by the user.
    IMFMediaTypeHandler* h = nullptr;
    hr = GetTypeHandler(source_, &h);
    if (SUCCEEDED(hr)) {
        IMFMediaType* t = nullptr;
        hr = h->GetMediaTypeByIndex(mode.typeIndex, &t);
        if (SUCCEEDED(hr)) { hr = h->SetCurrentMediaType(t); t->Release(); }
        h->Release();
    }
    if (FAILED(hr)) { error = Format(L"Le format choisi n'est pas accepté par la carte (0x%08X).", hr); Close(); return false; }
    sourceIsMjpeg = (mode.subtype == MFVideoFormat_MJPG);

    IMFAttributes* attr = nullptr;
    MFCreateAttributes(&attr, 4);
    attr->SetUINT32(MF_LOW_LATENCY, TRUE);
    attr->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    attr->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);
    hr = MFCreateSourceReaderFromMediaSource(source_, attr, &reader_);
    attr->Release();
    if (FAILED(hr)) { error = Format(L"Impossible de créer le lecteur vidéo (0x%08X).", hr); Close(); return false; }

    reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    // Ask for a YUV format the GPU converts itself; the native one first so nothing is converted on the CPU.
    std::vector<GUID> wanted;
    if (mode.subtype == MFVideoFormat_NV12 || mode.subtype == MFVideoFormat_YUY2) wanted.push_back(mode.subtype);
    wanted.push_back(MFVideoFormat_NV12);
    wanted.push_back(MFVideoFormat_YUY2);
    wanted.push_back(MFVideoFormat_RGB32);
    hr = E_FAIL;
    for (const GUID& g : wanted) {
        IMFMediaType* out = nullptr;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, g);
        hr = reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out);
        out->Release();
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr) || !ReadOutputType()) {
        error = Format(L"Aucun format de sortie utilisable pour ce flux (0x%08X).", hr);
        Close();
        return false;
    }
    return true;
}

bool VideoCapture::ReadOutputType() {
    IMFMediaType* t = nullptr;
    if (FAILED(reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &t))) return false;
    GUID sub{};
    t->GetGUID(MF_MT_SUBTYPE, &sub);
    if (sub == MFVideoFormat_NV12) layout = PixelLayout::NV12;
    else if (sub == MFVideoFormat_YUY2) layout = PixelLayout::YUY2;
    else if (sub == MFVideoFormat_RGB32) layout = PixelLayout::RGB32;
    else { t->Release(); return false; }

    MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &texWidth, &texHeight);
    width = texWidth; height = texHeight;
    MFVideoArea area{};
    if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof(area), nullptr))) {
        if (area.Area.cx > 0 && area.Area.cy > 0 && (UINT32)area.Area.cx <= texWidth && (UINT32)area.Area.cy <= texHeight) {
            width = area.Area.cx; height = area.Area.cy;
        }
    }
    UINT32 v = 0;
    typeKnowsRange = SUCCEEDED(t->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &v)) && v != MFNominalRange_Unknown;
    typeFullRange = typeKnowsRange && v == MFNominalRange_0_255;
    typeKnowsMatrix = SUCCEEDED(t->GetUINT32(MF_MT_YUV_MATRIX, &v)) && v != MFVideoTransferMatrix_Unknown;
    typeIs709 = typeKnowsMatrix ? (v == MFVideoTransferMatrix_BT709) : (height >= 720);
    t->Release();
    return texWidth > 0 && texHeight > 0;
}

bool VideoCapture::ReadFrame(FrameView& frame, bool& got, std::wstring& error) {
    got = false;
    ReleaseFrame();
    DWORD stream = 0, flags = 0;
    LONGLONG ts = 0;
    HRESULT hr = reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &sample_);
    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
        error = Format(L"La capture s'est arrêtée (0x%08X). La carte a peut-être été débranchée.", hr);
        return false;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { error = L"Fin du flux vidéo."; return false; }
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
        if (!ReadOutputType()) { error = L"Le format du flux a changé et n'est plus lisible."; return false; }
    }
    if (!sample_) return true;

    if (FAILED(sample_->GetBufferByIndex(0, &lockedBuffer_))) { SafeRelease(sample_); return true; }
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    lockedBuffer_->GetCurrentLength(&length);
    if (SUCCEEDED(lockedBuffer_->QueryInterface(__uuidof(IMF2DBuffer), (void**)&locked2D_)) &&
        SUCCEEDED(locked2D_->Lock2D(&scan0, &pitch))) {
        // locked through the 2D interface
    } else {
        SafeRelease(locked2D_);
        BYTE* p = nullptr;
        DWORD maxLen = 0;
        if (FAILED(lockedBuffer_->Lock(&p, &maxLen, &length))) { SafeRelease(lockedBuffer_); SafeRelease(sample_); return true; }
        scan0 = p;
        if (layout == PixelLayout::NV12) pitch = (LONG)texWidth;
        else if (layout == PixelLayout::YUY2) pitch = (LONG)texWidth * 2;
        else {
            // MF RGB32 is bottom-up by default.
            pitch = -(LONG)texWidth * 4;
            scan0 = p + (texHeight - 1) * texWidth * 4;
        }
    }

    frame.plane0 = scan0;
    frame.pitch = pitch;
    frame.plane1 = nullptr;
    if (layout == PixelLayout::NV12) {
        // Some decoders pad the luma plane (1080 -> 1088): find where the chroma starts.
        UINT32 planeRows = texHeight;
        if (pitch > 0 && length > 0) {
            UINT32 rows = (UINT32)((UINT64)length * 2 / 3 / (UINT64)pitch);
            if (rows > planeRows && rows < planeRows + 64) planeRows = rows;
        }
        frame.plane1 = scan0 + (size_t)pitch * planeRows;
    }
    got = true;
    return true;
}

void VideoCapture::ReleaseFrame() {
    if (locked2D_) { locked2D_->Unlock2D(); SafeRelease(locked2D_); }
    else if (lockedBuffer_) lockedBuffer_->Unlock();
    SafeRelease(lockedBuffer_);
    SafeRelease(sample_);
}

void VideoCapture::Close() {
    ReleaseFrame();
    SafeRelease(reader_);
    if (source_) { source_->Shutdown(); SafeRelease(source_); }
}
