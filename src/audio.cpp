// Relais - audio passthrough: capture card audio input -> default speakers (WASAPI)
// SPDX-License-Identifier: MIT
#include "app.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>

#include <cstring>

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool AudioPassthrough::Start(const std::wstring& endpointId, std::wstring& error) {
    Stop();
    if (endpointId.empty()) return true;
    endpoint_ = endpointId;
    stop_ = false;
    ok_ = false;
    error_.clear();
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!thread_) { CloseHandle(ready_); ready_ = nullptr; error = L"Impossible de démarrer le son."; return false; }
    WaitForSingleObject(ready_, 5000);
    if (!ok_) { error = error_.empty() ? L"Le son n'a pas pu démarrer." : error_; Stop(); return false; }
    return true;
}

void AudioPassthrough::Stop() {
    stop_ = true;
    if (thread_) {
        WaitForSingleObject(thread_, 3000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (ready_) { CloseHandle(ready_); ready_ = nullptr; }
}

DWORD WINAPI AudioPassthrough::ThreadProc(LPVOID self) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    static_cast<AudioPassthrough*>(self)->Run();
    CoUninitialize();
    return 0;
}

void AudioPassthrough::Run() {
    IMFAttributes* attr = nullptr;
    IMFMediaSource* src = nullptr;
    IMFSourceReader* reader = nullptr;
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* outDev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* wfx = nullptr;
    UINT32 wfxSize = 0;
    UINT32 bufferFrames = 0;
    bool started = false;

    auto fail = [&](const wchar_t* msg, HRESULT hr) {
        error_ = Format(L"%s (0x%08X)", msg, hr);
        SetEvent(ready_);
    };

    // --- capture side: Media Foundation reads the card's audio endpoint
    HRESULT hr = MFCreateAttributes(&attr, 2);
    if (SUCCEEDED(hr)) {
        attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID);
        attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID, endpoint_.c_str());
        hr = MFCreateDeviceSource(attr, &src);
    }
    SafeRelease(attr);
    if (FAILED(hr)) { fail(L"Impossible d'ouvrir l'entrée audio", hr); goto done; }

    MFCreateAttributes(&attr, 1);
    attr->SetUINT32(MF_LOW_LATENCY, TRUE);
    hr = MFCreateSourceReaderFromMediaSource(src, attr, &reader);
    SafeRelease(attr);
    if (FAILED(hr)) { fail(L"Impossible de lire l'entrée audio", hr); goto done; }

    {
        // Ask for 32-bit float, keeping the native rate and channel count.
        IMFMediaType* native = nullptr;
        UINT32 rate = 48000, channels = 2;
        if (SUCCEEDED(reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native))) {
            native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
            native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
            native->Release();
        }
        IMFMediaType* t = nullptr;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 4);
        t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 4);
        hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t);
        t->Release();
        if (FAILED(hr)) { fail(L"Format audio non pris en charge", hr); goto done; }
        IMFMediaType* cur = nullptr;
        hr = reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur);
        if (SUCCEEDED(hr)) { hr = MFCreateWaveFormatExFromMFMediaType(cur, &wfx, &wfxSize, 0); cur->Release(); }
        if (FAILED(hr)) { fail(L"Format audio illisible", hr); goto done; }
    }

    // --- render side: default speakers, shared mode, Windows converts the rate if needed
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &outDev);
    if (SUCCEEDED(hr)) hr = outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (SUCCEEDED(hr)) {
        const REFERENCE_TIME bufferDuration = 400000; // 40 ms
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                bufferDuration, 0, wfx, nullptr);
    }
    if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&render);
    if (FAILED(hr)) { fail(L"Impossible d'ouvrir les haut-parleurs", hr); goto done; }

    ok_ = true;
    SetEvent(ready_);

    {
        const UINT32 frameBytes = wfx->nBlockAlign;
        const UINT32 maxQueued = bufferFrames * 3 / 4;   // keep latency low: never queue more than ~30 ms
        while (!stop_) {
            DWORD stream = 0, flags = 0;
            LONGLONG ts = 0;
            IMFSample* sample = nullptr;
            hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &stream, &flags, &ts, &sample);
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) break;
            if (!sample) continue;
            IMFMediaBuffer* buf = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
                BYTE* data = nullptr; DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                    UINT32 frames = len / frameBytes;
                    UINT32 padding = 0;
                    client->GetCurrentPadding(&padding);
                    UINT32 room = padding < maxQueued ? maxQueued - padding : 0;
                    // When the queue is full, drop the oldest part of this packet instead of adding delay.
                    UINT32 toWrite = frames < room ? frames : room;
                    const BYTE* from = data + (size_t)(frames - toWrite) * frameBytes;
                    BYTE* dst = nullptr;
                    if (toWrite && SUCCEEDED(render->GetBuffer(toWrite, &dst))) {
                        memcpy(dst, from, (size_t)toWrite * frameBytes);
                        render->ReleaseBuffer(toWrite, 0);
                        if (!started) { client->Start(); started = true; }
                    }
                    buf->Unlock();
                }
                buf->Release();
            }
            sample->Release();
        }
    }

done:
    if (client && started) client->Stop();
    SafeRelease(render);
    SafeRelease(client);
    SafeRelease(outDev);
    SafeRelease(en);
    if (wfx) CoTaskMemFree(wfx);
    SafeRelease(reader);
    if (src) { src->Shutdown(); SafeRelease(src); }
    if (!ok_) SetEvent(ready_);
}
