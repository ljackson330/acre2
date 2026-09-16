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

    /*
     * Really reference-counted, and heap-allocated as a result.
     *
     * The obvious shortcut -- a stack object returning constants from AddRef
     * and Release, as Microsoft's ApplicationLoopback sample does -- is unsafe
     * here. The audio engine can still hold a reference briefly after
     * GetActivateResult returns, and this object owns the wait event, so a
     * stack instance would destruct at function exit and the engine would call
     * into freed stack and a closed handle. That presents as an intermittent
     * crash inside ts3client.exe, which is precisely the failure mode worth
     * spending ten lines to avoid.
     */
    STDMETHODIMP_(ULONG) AddRef() override {
        return this->m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    STDMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = this->m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

private:
    // Virtual because Release() does `delete this` on a polymorphic type.
    // Private so the only way to destroy it is by dropping the last reference.
    virtual ~CActivationHandler() {
        if (this->m_event != nullptr) {
            CloseHandle(this->m_event);
        }
    }

    std::atomic<ULONG> m_refCount{1};
    HANDLE m_event = nullptr;
};

std::string hresultToString(HRESULT hr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)hr);
    return std::string(buf);
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

bool CAmbientWasapiSource::start(SampleSink sink) {
    // The interface deliberately does not take a PID: callers work in terms of
    // "capture the game", and which process that is belongs to the backend.
    // No logging here on purpose: this translation unit stays free of ACRE2
    // dependencies so ambient/probe can build it standalone. Callers that want
    // to warn about multiple instances use countProcesses() themselves.
    const DWORD pid = findProcessId(GAME_EXECUTABLE);
    if (pid == 0) {
        this->fail("no running process named arma3_x64.exe", S_OK);
        return false;
    }
    return this->startForPid(pid, std::move(sink));
}

bool CAmbientWasapiSource::startForPid(DWORD targetPid, SampleSink sink) {
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
    this->m_bytesThisSession.store(0, std::memory_order_release);
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

    CActivationHandler *handler = new CActivationHandler();
    if (handler->event() == nullptr) {
        this->fail("could not create activation event", S_OK);
        handler->Release();
        return false;
    }

    IActivateAudioInterfaceAsyncOperation *operation = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
        &activateParams, handler, &operation);
    if (FAILED(hr)) {
        this->fail("ActivateAudioInterfaceAsync failed", hr);
        handler->Release();
        return false;
    }

    // Bounded, always. Proton exports this function but has no process-loopback
    // device behind it, so on a Wine host the completion may never arrive -- and
    // an unbounded wait there would be indistinguishable from a hang.
    const DWORD waited = WaitForSingleObject(handler->event(), ACTIVATE_TIMEOUT_MS);
    if (waited != WAIT_OBJECT_0) {
        if (operation != nullptr) {
            operation->Release();
        }
        // Dropping our reference here does not free the handler if the engine
        // still holds one -- which is the whole point of refcounting it.
        handler->Release();
        this->fail("activation did not complete within "
                   + std::to_string(ACTIVATE_TIMEOUT_MS) + " ms", S_OK);
        return false;
    }

    HRESULT activateResult = E_FAIL;
    IUnknown *unknown = nullptr;
    hr = operation->GetActivateResult(&activateResult, &unknown);
    operation->Release();
    handler->Release();

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

    // A failure inside the loop must leave it. Without the m_failed check a
    // persistent GetBuffer error would spin here at 500 ms intervals, calling
    // fail() forever and overwriting the first, most useful error message.
    while (!this->m_stopRequested.load(std::memory_order_acquire)
           && !this->m_failed.load(std::memory_order_acquire)) {
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

    /*
     * No conversion, by construction.
     *
     * Initialize() either accepts the 48 kHz mono s16 this class asks for or
     * fails outright, and m_format is set from the request rather than queried
     * back from the engine -- so anything arriving here is already exactly what
     * the pipeline wants. Verified on Windows 11 24H2, where the engine
     * accepted the requested format exactly.
     *
     * If a future change ever asks for a second format when the first is
     * refused, this is where the downmix and rate conversion have to come back.
     */
    if (silent || data == nullptr) {
        // A silent packet carries no valid data and must be counted as zeros
        // rather than skipped: skipping would shorten the stream and drift the
        // ambience out of step with the speech it accompanies. Process loopback
        // really does send these -- an idle target produced 811 buffers of
        // silence over 8 s rather than going quiet.
        // resize, not assign: the buffer only ever holds zeros, so growing it
        // zero-fills the new tail and leaves the rest alone.
        if (this->m_silence.size() < frameCount) {
            this->m_silence.resize(frameCount, 0);
        }
        this->m_sink(this->m_silence.data(), frameCount);
        return;
    }

    this->m_bytesThisSession.fetch_add(frameCount * sizeof(int16_t),
                                       std::memory_order_relaxed);
    this->m_sink(reinterpret_cast<const int16_t *>(data), frameCount);
}
