#pragma once

#include "AmbientSource.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/*
 * Windows ambient capture: WASAPI process loopback.
 *
 * Captures the rendered audio of one process by PID, which is the whole reason
 * this design is safe -- ACRE2 plays received radio through the TeamSpeak
 * process, so a device-wide loopback would pick up audio this client is already
 * receiving and re-transmit it. Scoping to Arma's PID makes that impossible by
 * construction rather than by filtering.
 *
 * Deliberately free of ACRE2 dependencies. The same translation unit builds
 * into the plugin and into ambient/probe, which is what lets the capture path
 * be exercised standalone -- without TeamSpeak, without the mod installed, and
 * (in a VM) without Arma -- before it ever runs inside ts3client.exe.
 *
 * Delivers signed 16-bit mono at 48 kHz to the sink, matching the wire format
 * the Linux helper already produces, so everything downstream of this class is
 * shared between the two platforms and stays byte-for-byte unchanged.
 *
 * Verified on Windows 11 24H2 via ambient/probe: activation succeeds, the
 * engine accepts the requested 48 kHz mono s16 exactly, and capturing a process
 * that renders nothing yields digital silence while another blares on the same
 * device. What remains untested is Arma's own render stream, which needs a real
 * machine with the game.
 */
class CAmbientWasapiSource : public IAmbientSource {
public:

    static constexpr uint32_t TARGET_RATE = 48000;
    static constexpr uint16_t TARGET_CHANNELS = 1;

    CAmbientWasapiSource() = default;
    ~CAmbientWasapiSource() override;
    CAmbientWasapiSource(const CAmbientWasapiSource &) = delete;
    CAmbientWasapiSource &operator=(const CAmbientWasapiSource &) = delete;

    /*
     * Returns immediately. COM initialisation, activation and the capture loop
     * all run on the spawned thread, so a slow or hanging activation can never
     * stall the caller -- which matters because the caller is on the player's
     * push-to-talk path.
     *
     * Success here means "the thread started", not "capture works". Poll
     * isRunning() / hasFailed() for that.
     */
    bool start(SampleSink sink) override;
    void stop() override;

    // Capture a specific process rather than searching for the game. The probe
    // uses this; the plugin goes through start().
    bool startForPid(DWORD targetPid, SampleSink sink);

    inline bool isRunning() const override { return m_running.load(std::memory_order_acquire); }
    inline bool hasFailed() const override { return m_failed.load(std::memory_order_acquire); }

    const char *name() const override { return "WASAPI process loopback"; }
    std::string lastError() const override;
    uint64_t bytesDelivered() const override {
        return m_bytesThisSession.load(std::memory_order_acquire);
    }

    // Executable the plugin looks for. Arma's 64-bit client; the 32-bit build
    // is long dead and not worth a second name.
    static constexpr const wchar_t *GAME_EXECUTABLE = L"arma3_x64.exe";

    // Populated once activation resolves; safe to read after hasFailed() or
    // isRunning() goes true. Describes what the audio engine actually gave us.
    std::string describeFormat() const;

    // Finds a process by executable name, case-insensitively. Returns 0 if not
    // found, and the lowest PID if several match -- callers that care about
    // ambiguity (two Arma instances on one machine) must disambiguate
    // themselves rather than trusting this.
    static DWORD findProcessId(const wchar_t *exeName);
    static size_t countProcesses(const wchar_t *exeName);

private:
    void captureThread(DWORD targetPid);
    bool activateClient(DWORD targetPid, IAudioClient **outClient);
    void fail(const std::string &what, HRESULT hr);

    // Converts one engine buffer to mono 48 kHz s16 and hands it to the sink.
    void deliver(const BYTE *data, uint32_t frameCount, bool silent);

    SampleSink m_sink;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_failed{false};
    std::atomic<uint64_t> m_bytesThisSession{0};

    // Activation is asynchronous; the capture thread waits on this rather than
    // spinning, and always with a bound -- an activation that never completes
    // must degrade to "no ambience", never to a wedged thread.
    static constexpr DWORD ACTIVATE_TIMEOUT_MS = 3000;
    static constexpr DWORD BUFFER_DURATION_MS = 200;

    // The format asked for and accepted; Initialize() fails rather than
    // substituting one. Written by the capture thread before m_running goes true.
    WAVEFORMATEX m_format{};
    // Written by the capture thread, read by whoever is polling the flags.
    mutable std::mutex m_errorMutex;
    std::string m_error;

    // Reusable zero buffer for silent packets, so the audio path does not
    // allocate once it is warm.
    std::vector<int16_t> m_silence;
};
