#include "AmbientWasapi.h"

#include <audioclientactivationparams.h>
#include <mmreg.h>
#include <tlhelp32.h>

#include <cstdio>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")
#endif

namespace {

/*
 * ActivateAudioInterfaceAsync reports completion through a callback interface
 * rather than a wait handle, so this exists purely to turn that back into an
 * event the capture thread can wait on with a timeout.
 *
 * IAgileObject is required: without it the activation is marshalled back to the
 * thread's apartment, and a thread that is blocked waiting for the result is
 * not pumping messages, so the callback would never arrive.
 */
class CActivationHandler : public IActivateAudioInterfaceCompletionHandler,
                           public IAgileObject {
public:
    CActivationHandler() {
        this->m_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~CActivationHandler() {
        if (this->m_event != nullptr) {
            CloseHandle(this->m_event);
        }
    }

    HANDLE event() const { return this->m_event; }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation *) override {
        SetEvent(this->m_event);
        return S_OK;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IUnknown *>(static_cast<IActivateAudioInterfaceCompletionHandler *>(this));
        } else if (riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IAgileObject *>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        this->AddRef();
        return S_OK;
    }

    // Deliberately not reference-counted into destruction: the handler lives on
    // the capture thread's stack for the whole activation, which outlives every
    // reference the audio engine can hold.
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

private:
    HANDLE m_event = nullptr;
};

std::string hresultToString(HRESULT hr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)hr);
    return std::string(buf);
}

// Clamp and convert one float sample, which is what the engine hands us when
// it declines a PCM format and falls back to its own.
inline int16_t floatToPcm(float v) {
    if (v > 1.0f) v = 1.0f;
    else if (v < -1.0f) v = -1.0f;
    return static_cast<int16_t>(v * 32767.0f);
}

}  // namespace

CAmbientWasapiSource::~CAmbientWasapiSource() {
    this->stop();
}

size_t CAmbientWasapiSource::countProcesses(const wchar_t *exeName) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    size_t found = 0;

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, exeName) == 0) {
                ++found;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

DWORD CAmbientWasapiSource::findProcessId(const wchar_t *exeName) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    DWORD best = 0;

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, exeName) == 0) {
                if (best == 0 || entry.th32ProcessID < best) {
                    best = entry.th32ProcessID;
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return best;
}

void CAmbientWasapiSource::fail(const std::string &what, HRESULT hr) {
    {
        std::lock_guard<std::mutex> guard(this->m_errorMutex);
        this->m_error = what;
        if (hr != S_OK) {
            this->m_error += " (" + hresultToString(hr) + ")";
        }
    }
    this->m_failed.store(true, std::memory_order_release);
    this->m_running.store(false, std::memory_order_release);
}

std::string CAmbientWasapiSource::lastError() const {
    std::lock_guard<std::mutex> guard(this->m_errorMutex);
    return this->m_error;
}

std::string CAmbientWasapiSource::describeFormat() const {
    if (this->m_format.nChannels == 0) {
        return "not negotiated";
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "%u Hz, %u ch, %u-bit, tag %u",
             (unsigned)this->m_format.nSamplesPerSec,
             (unsigned)this->m_format.nChannels,
             (unsigned)this->m_format.wBitsPerSample,
             (unsigned)this->m_format.wFormatTag);
    return std::string(buf);
}

bool CAmbientWasapiSource::start(DWORD targetPid, SampleSink sink) {
    if (this->m_running.load(std::memory_order_acquire)) {
        return true;
    }
    if (this->m_thread.joinable()) {
        this->m_thread.join();
    }
    if (targetPid == 0 || !sink) {
        this->fail("no target process", S_OK);
        return false;
    }

    this->m_sink = std::move(sink);
    this->m_failed.store(false, std::memory_order_release);
    this->m_stopRequested.store(false, std::memory_order_release);
    this->m_resamplePos = 0.0;
    this->m_resampleLast = 0.0f;
    this->m_format = WAVEFORMATEX{};

    this->m_thread = std::thread(&CAmbientWasapiSource::captureThread, this, targetPid);
    return true;
}

void CAmbientWasapiSource::stop() {
    this->m_stopRequested.store(true, std::memory_order_release);
    if (this->m_thread.joinable()) {
        this->m_thread.join();
    }
    this->m_running.store(false, std::memory_order_release);
}

bool CAmbientWasapiSource::activateClient(DWORD targetPid, IAudioClient **outClient) {
    *outClient = nullptr;

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = targetPid;
    // Include the tree: Arma's audio could plausibly come from a child process,
    // and including it cannot pull in TeamSpeak, which is not a descendant.
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateParams{};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE *>(&params);

    CActivationHandler handler;
    if (handler.event() == nullptr) {
        this->fail("could not create activation event", S_OK);
        return false;
    }

    IActivateAudioInterfaceAsyncOperation *operation = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
        &activateParams, &handler, &operation);
    if (FAILED(hr)) {
        this->fail("ActivateAudioInterfaceAsync failed", hr);
        return false;
    }

    // Bounded, always. Proton exports this function but has no process-loopback
    // device behind it, so on a Wine host the completion may never arrive -- and
    // an unbounded wait there would be indistinguishable from a hang.
    const DWORD waited = WaitForSingleObject(handler.event(), ACTIVATE_TIMEOUT_MS);
    if (waited != WAIT_OBJECT_0) {
        if (operation != nullptr) {
            operation->Release();
        }
        this->fail("activation did not complete within "
                   + std::to_string(ACTIVATE_TIMEOUT_MS) + " ms", S_OK);
        return false;
    }

    HRESULT activateResult = E_FAIL;
    IUnknown *unknown = nullptr;
    hr = operation->GetActivateResult(&activateResult, &unknown);
    operation->Release();

    if (FAILED(hr)) {
        this->fail("GetActivateResult failed", hr);
        return false;
    }
    if (FAILED(activateResult) || unknown == nullptr) {
        this->fail("process loopback activation rejected", activateResult);
        if (unknown != nullptr) {
            unknown->Release();
        }
        return false;
    }

    hr = unknown->QueryInterface(__uuidof(IAudioClient), (void **)outClient);
    unknown->Release();
    if (FAILED(hr)) {
        this->fail("activated object is not an IAudioClient", hr);
        return false;
    }
    return true;
}

void CAmbientWasapiSource::captureThread(DWORD targetPid) {
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(comInit);

    IAudioClient *client = nullptr;
    IAudioCaptureClient *capture = nullptr;
    HANDLE bufferEvent = nullptr;

    if (!this->activateClient(targetPid, &client)) {
        goto cleanup;
    }

    {
        /*
         * Ask for exactly what the rest of the pipeline wants. A process
         * loopback client is documented to accept a caller-chosen PCM format
         * and convert on its way out, which -- if true -- means no resampler is
         * needed at all. deliver() still handles the general case, because that
         * claim has not been checked on real hardware yet.
         */
        WAVEFORMATEX requested{};
        requested.wFormatTag = WAVE_FORMAT_PCM;
        requested.nChannels = TARGET_CHANNELS;
        requested.nSamplesPerSec = TARGET_RATE;
        requested.wBitsPerSample = 16;
        requested.nBlockAlign = (WORD)(requested.nChannels * requested.wBitsPerSample / 8);
        requested.nAvgBytesPerSec = requested.nSamplesPerSec * requested.nBlockAlign;
        requested.cbSize = 0;

        HRESULT hr = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            (REFERENCE_TIME)BUFFER_DURATION_MS * 10000, 0, &requested, nullptr);
        if (FAILED(hr)) {
            this->fail("IAudioClient::Initialize rejected 48 kHz mono s16", hr);
            goto cleanup;
        }
        this->m_format = requested;

        bufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (bufferEvent == nullptr) {
            this->fail("could not create buffer event", S_OK);
            goto cleanup;
        }

        hr = client->SetEventHandle(bufferEvent);
        if (FAILED(hr)) {
            this->fail("SetEventHandle failed", hr);
            goto cleanup;
        }

        hr = client->GetService(__uuidof(IAudioCaptureClient), (void **)&capture);
        if (FAILED(hr)) {
            this->fail("GetService(IAudioCaptureClient) failed", hr);
            goto cleanup;
        }

        hr = client->Start();
        if (FAILED(hr)) {
            this->fail("IAudioClient::Start failed", hr);
            goto cleanup;
        }
    }

    this->m_running.store(true, std::memory_order_release);

    while (!this->m_stopRequested.load(std::memory_order_acquire)) {
        // A silent target renders nothing, so a timeout here is normal rather
        // than an error -- loop and re-check the stop flag.
        if (WaitForSingleObject(bufferEvent, 500) != WAIT_OBJECT_0) {
            continue;
        }

        for (;;) {
            BYTE *data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;

            const HRESULT hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) {
                break;
            }
            if (FAILED(hr)) {
                this->fail("GetBuffer failed", hr);
                break;
            }

            this->deliver(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
            capture->ReleaseBuffer(frames);

            if (frames == 0) {
                break;
            }
        }
    }

    if (client != nullptr) {
        client->Stop();
    }

cleanup:
    if (capture != nullptr) {
        capture->Release();
    }
    if (client != nullptr) {
        client->Release();
    }
    if (bufferEvent != nullptr) {
        CloseHandle(bufferEvent);
    }
    if (comOwned) {
        CoUninitialize();
    }
    this->m_running.store(false, std::memory_order_release);
}

void CAmbientWasapiSource::deliver(const BYTE *data, uint32_t frameCount, bool silent) {
    if (frameCount == 0 || !this->m_sink) {
        return;
    }

    const uint16_t channels = this->m_format.nChannels;
    const uint16_t bits = this->m_format.wBitsPerSample;
    const uint32_t rate = this->m_format.nSamplesPerSec;

    // Downmix to mono first, in float, so the rate conversion below has one
    // stream to work on regardless of what the engine handed us.
    std::vector<float> mono(frameCount);

    if (silent || data == nullptr) {
        // A silent packet carries no valid data and must be treated as zeros,
        // not skipped -- skipping would shorten the stream and drift the mix
        // out of step with the voice it accompanies.
        std::fill(mono.begin(), mono.end(), 0.0f);
    } else if (bits == 16) {
        const int16_t *pcm = reinterpret_cast<const int16_t *>(data);
        for (uint32_t frame = 0; frame < frameCount; ++frame) {
            int32_t sum = 0;
            for (uint16_t ch = 0; ch < channels; ++ch) {
                sum += pcm[(frame * channels) + ch];
            }
            mono[frame] = (float)sum / (float)channels / 32768.0f;
        }
    } else if (bits == 32) {
        const float *flt = reinterpret_cast<const float *>(data);
        for (uint32_t frame = 0; frame < frameCount; ++frame) {
            float sum = 0.0f;
            for (uint16_t ch = 0; ch < channels; ++ch) {
                sum += flt[(frame * channels) + ch];
            }
            mono[frame] = sum / (float)channels;
        }
    } else {
        return;  // unexpected width; dropping beats emitting noise
    }

    if (rate == TARGET_RATE) {
        std::vector<int16_t> out(frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            out[i] = floatToPcm(mono[i]);
        }
        this->m_sink(out.data(), out.size());
        return;
    }

    /*
     * Linear interpolation. Crude, but the engine is expected to deliver 48 kHz
     * directly and this path exists only so an unexpected rate degrades to
     * slightly soft ambience rather than to silence or to pitch-shifted audio.
     * If the probe shows this path actually gets used, replace it.
     */
    const double step = (double)rate / (double)TARGET_RATE;
    std::vector<int16_t> out;
    out.reserve((size_t)(frameCount / step) + 2);

    double pos = this->m_resamplePos;
    while (pos < (double)frameCount) {
        const size_t index = (size_t)pos;
        const double frac = pos - (double)index;
        const float a = (index == 0) ? this->m_resampleLast : mono[index - 1];
        const float b = mono[index];
        out.push_back(floatToPcm(a + (float)frac * (b - a)));
        pos += step;
    }
    this->m_resamplePos = pos - (double)frameCount;
    this->m_resampleLast = mono[frameCount - 1];

    if (!out.empty()) {
        this->m_sink(out.data(), out.size());
    }
}
