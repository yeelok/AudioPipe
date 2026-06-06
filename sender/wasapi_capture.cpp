// WASAPI shared-mode loopback capture. Built on Windows only.
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propvarutil.h>

#include "wasapi_capture.h"

namespace audiopipe {
namespace win {
namespace {

std::string narrow(const wchar_t* w) {
    if (w == nullptr) {
        return {};
    }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// True if the mix format is 32-bit IEEE float (what shared-mode loopback yields
// for the AudioPipe device). KSDATAFORMAT_SUBTYPE_IEEE_FLOAT has Data1 == 3,
// matching WAVE_FORMAT_IEEE_FLOAT — checked directly to avoid ksmedia.h.
bool is_float_format(const WAVEFORMATEX* f) {
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        return ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
    }
    return false;
}

} // namespace

struct WasapiLoopbackCapture::Impl {
    IMMDeviceEnumerator* enumr   = nullptr;
    IMMDevice*           dev     = nullptr;
    IAudioClient*        client  = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX*        mix     = nullptr;
    bool                 com_init = false;
    std::uint32_t        rate     = 0;
    std::uint16_t        channels = 0;

    void release() {
        if (capture) { capture->Release(); capture = nullptr; }
        if (client)  { client->Stop(); client->Release(); client = nullptr; }
        if (mix)     { CoTaskMemFree(mix); mix = nullptr; }
        if (dev)     { dev->Release(); dev = nullptr; }
        if (enumr)   { enumr->Release(); enumr = nullptr; }
    }
};

WasapiLoopbackCapture::WasapiLoopbackCapture() : impl_(std::make_unique<Impl>()) {}
WasapiLoopbackCapture::~WasapiLoopbackCapture() { close(); }

std::uint32_t WasapiLoopbackCapture::sample_rate() const { return impl_->rate; }
std::uint16_t WasapiLoopbackCapture::channels() const { return impl_->channels; }

bool WasapiLoopbackCapture::open(const std::string& device_substr, std::string& err) {
    if (!impl_->com_init) {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
            impl_->com_init = true;
        }
    }

    // Clear any partial state from a previous failed open() so the sender's
    // reconnect loop (spec §5.7) can retry without leaking COM objects.
    impl_->release();

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&impl_->enumr));
    if (FAILED(hr)) {
        err = "CoCreateInstance(MMDeviceEnumerator) failed";
        return false;
    }

    if (device_substr.empty()) {
        hr = impl_->enumr->GetDefaultAudioEndpoint(eRender, eConsole, &impl_->dev);
        if (FAILED(hr)) {
            err = "GetDefaultAudioEndpoint(eRender) failed";
            return false;
        }
    } else {
        IMMDeviceCollection* coll = nullptr;
        hr = impl_->enumr->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
        if (FAILED(hr)) {
            err = "EnumAudioEndpoints failed";
            return false;
        }
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count && impl_->dev == nullptr; ++i) {
            IMMDevice*       d     = nullptr;
            IPropertyStore*  props = nullptr;
            if (FAILED(coll->Item(i, &d))) {
                continue;
            }
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT name;
                PropVariantInit(&name);
                props->GetValue(PKEY_Device_FriendlyName, &name);
                const std::string fn = (name.vt == VT_LPWSTR) ? narrow(name.pwszVal) : std::string();
                PropVariantClear(&name);
                props->Release();
                if (!fn.empty() && fn.find(device_substr) != std::string::npos) {
                    impl_->dev = d; // keep
                    break;
                }
            }
            d->Release();
        }
        coll->Release();
        if (impl_->dev == nullptr) {
            err = "render device matching '" + device_substr + "' not found";
            return false;
        }
    }

    hr = impl_->dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&impl_->client));
    if (FAILED(hr)) {
        err = "IMMDevice::Activate(IAudioClient) failed";
        return false;
    }
    hr = impl_->client->GetMixFormat(&impl_->mix);
    if (FAILED(hr)) {
        err = "GetMixFormat failed";
        return false;
    }
    if (!is_float_format(impl_->mix)) {
        err = "device mix format is not 32-bit float";
        return false;
    }
    hr = impl_->client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0,
                                   impl_->mix, nullptr);
    if (FAILED(hr)) {
        err = "IAudioClient::Initialize (shared loopback) failed";
        return false;
    }
    hr = impl_->client->GetService(__uuidof(IAudioCaptureClient),
                                   reinterpret_cast<void**>(&impl_->capture));
    if (FAILED(hr)) {
        err = "GetService(IAudioCaptureClient) failed";
        return false;
    }
    hr = impl_->client->Start();
    if (FAILED(hr)) {
        err = "IAudioClient::Start failed";
        return false;
    }
    impl_->rate     = impl_->mix->nSamplesPerSec;
    impl_->channels = impl_->mix->nChannels;
    return true;
}

bool WasapiLoopbackCapture::capture(std::vector<float>& out, bool& silent, std::string& err) {
    silent = false;
    if (impl_->capture == nullptr) {
        err = "capture not open";
        return false;
    }
    const std::uint16_t ch = impl_->channels;

    UINT32  packet = 0;
    HRESULT hr     = impl_->capture->GetNextPacketSize(&packet);
    if (FAILED(hr)) {
        err = "GetNextPacketSize failed";
        return false;
    }
    while (packet > 0) {
        BYTE*  data   = nullptr;
        UINT32 frames = 0;
        DWORD  flags  = 0;
        hr = impl_->capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) {
            err = "GetBuffer failed";
            return false;
        }
        const std::size_t n = std::size_t(frames) * ch;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            out.insert(out.end(), n, 0.0f);
            silent = true;
        } else {
            const float* f = reinterpret_cast<const float*>(data);
            out.insert(out.end(), f, f + n);
        }
        impl_->capture->ReleaseBuffer(frames);
        hr = impl_->capture->GetNextPacketSize(&packet);
        if (FAILED(hr)) {
            err = "GetNextPacketSize failed";
            return false;
        }
    }
    return true;
}

void WasapiLoopbackCapture::close() {
    if (!impl_) {
        return;
    }
    impl_->release();
    if (impl_->com_init) {
        CoUninitialize();
        impl_->com_init = false;
    }
}

} // namespace win
} // namespace audiopipe
