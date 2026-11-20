// Relais - low-latency capture card viewer for Windows
// SPDX-License-Identifier: MIT
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <atomic>
#include <string>
#include <vector>

#define RELAIS_VERSION L"1.0.0"

// ---------------------------------------------------------------------------
// Settings (saved in %APPDATA%\Relais\relais.ini)
// ---------------------------------------------------------------------------
enum ColorRange { RANGE_AUTO = 0, RANGE_LIMITED = 1, RANGE_FULL = 2 };
enum ColorMatrix { MATRIX_AUTO = 0, MATRIX_601 = 1, MATRIX_709 = 2 };

struct Settings {
    std::wstring videoDevice;   // symbolic link of the capture device
    std::wstring videoMode;     // label of the chosen format, e.g. "1920 x 1080 @ 60 fps (MJPG)"
    std::wstring audioDevice;   // endpoint id of the audio input, empty = no sound
    int range = RANGE_AUTO;
    int matrix = MATRIX_AUTO;
    int brightness = 0;         // -100 .. 100
    int contrast = 100;         // 0 .. 200 (%)
    int saturation = 100;       // 0 .. 200 (%)
    int sharpness = 0;          // 0 .. 100
    bool exclusive = true;      // exclusive fullscreen (needed by AFMF on RX 6000)
    bool vsync = false;
    bool dropDuplicates = true; // skip repeated frames (30 fps games captured at 60 fps)
    int dupThreshold = 15;      // tenths of a grey level of mean difference
};

void LoadSettings(Settings& s);
void SaveSettings(const Settings& s);

// ---------------------------------------------------------------------------
// Devices and formats (video.cpp)
// ---------------------------------------------------------------------------
struct DeviceInfo {
    std::wstring name;
    std::wstring id;            // symbolic link (video) or endpoint id (audio)
};

struct VideoMode {
    UINT32 width = 0, height = 0;
    UINT32 fpsNum = 0, fpsDen = 1;
    GUID subtype{};
    DWORD typeIndex = 0;        // index in the device's native media types
    std::wstring label;
    double fps() const { return fpsDen ? double(fpsNum) / fpsDen : 0.0; }
};

std::vector<DeviceInfo> EnumVideoDevices();
std::vector<DeviceInfo> EnumAudioDevices();
std::vector<VideoMode> EnumVideoModes(const std::wstring& deviceId);

enum class PixelLayout { NV12 = 0, YUY2 = 1, RGB32 = 2 };

// A decoded frame, valid until VideoCapture::ReleaseFrame().
struct FrameView {
    const BYTE* plane0 = nullptr;   // Y (NV12), packed YUY2, or BGRX rows
    const BYTE* plane1 = nullptr;   // interleaved UV for NV12
    LONG pitch = 0;                 // bytes between rows (can be negative for bottom-up RGB)
};

class VideoCapture {
public:
    ~VideoCapture() { Close(); }
    bool Open(const std::wstring& deviceId, const VideoMode& mode, std::wstring& error);
    void Close();
    // Blocks until the next frame. Returns false on a fatal error (device unplugged...).
    // got is false when the call returned without a frame (stream tick).
    bool ReadFrame(FrameView& frame, bool& got, std::wstring& error);
    void ReleaseFrame();

    PixelLayout layout = PixelLayout::NV12;
    UINT32 texWidth = 0, texHeight = 0;     // size of the decoded buffer
    UINT32 width = 0, height = 0;           // visible area
    bool sourceIsMjpeg = false;
    bool typeFullRange = false;             // range reported by the output media type
    bool typeKnowsRange = false;
    bool typeIs709 = true;
    bool typeKnowsMatrix = false;

private:
    bool ReadOutputType();
    struct IMFMediaSource* source_ = nullptr;
    struct IMFSourceReader* reader_ = nullptr;
    struct IMFMediaBuffer* lockedBuffer_ = nullptr;
    struct IMF2DBuffer* locked2D_ = nullptr;
    struct IMFSample* sample_ = nullptr;
};

// ---------------------------------------------------------------------------
// Audio passthrough (audio.cpp)
// ---------------------------------------------------------------------------
class AudioPassthrough {
public:
    ~AudioPassthrough() { Stop(); }
    bool Start(const std::wstring& endpointId, std::wstring& error);
    void Stop();
private:
    static DWORD WINAPI ThreadProc(LPVOID self);
    void Run();
    HANDLE thread_ = nullptr;
    std::atomic<bool> stop_{false};
    std::wstring endpoint_;
    HANDLE ready_ = nullptr;
    std::wstring error_;
    bool ok_ = false;
};

// ---------------------------------------------------------------------------
// Viewer (render.cpp)
// ---------------------------------------------------------------------------
struct SessionStats {
    unsigned long long captured = 0;   // frames received from the card
    unsigned long long presented = 0;  // frames sent to the screen
    unsigned long long duplicates = 0; // frames skipped as duplicates
    double seconds = 0.0;
    std::wstring error;
};

// Runs the fullscreen viewer until Escape is pressed. Settings changed with
// hotkeys inside the viewer are written back into s.
SessionStats RunViewer(HINSTANCE inst, Settings& s, const VideoMode& mode);

// Small helpers
std::wstring Format(const wchar_t* fmt, ...);
