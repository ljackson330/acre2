#pragma once

#include "compat.h"
#include "AmbientSource.h"

#include <atomic>
#include <mutex>
#include <thread>

/*
 * Ambient capture via the Linux helper.
 *
 * The plugin is a Windows DLL running under Wine and cannot call PipeWire, and
 * Proton does not implement AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK, so
 * there is no way for it to capture the game for itself on Linux. ambient-
 * helper.py runs natively, captures Arma's PipeWire node, and serves it over a
 * loopback socket that this class reads with ordinary Winsock.
 *
 * Capture is scoped to Arma's node on the helper side, never the output device,
 * so radio audio this client is receiving -- which plays through the TeamSpeak
 * process -- cannot be picked up and re-transmitted. That is the same
 * process-scoping property the Windows backend gets from PID targeting.
 *
 * The helper keeps its capture stream open and discards samples while nothing
 * is connected, so connecting per transmission is cheap.
 */
class CAmbientSocketSource : public IAmbientSource {
public:
    ~CAmbientSocketSource() override;

    bool start(SampleSink sink) override;
    void stop() override;

    bool isRunning() const override {
        return this->m_running.load(std::memory_order_acquire);
    }

    bool hasFailed() const override {
        return this->m_failed.load(std::memory_order_acquire);
    }

    const char *name() const override { return "helper socket"; }
    std::string lastError() const override;

    uint64_t bytesDelivered() const override {
        return this->m_bytesThisSession.load(std::memory_order_acquire);
    }

private:
    void readLoop();
    bool connectToHelper();
    void setError(const std::string &what);

    static constexpr unsigned short HELPER_PORT = 47806;
    static constexpr int CONNECT_TIMEOUT_MS = 200;

    SOCKET m_socket = INVALID_SOCKET;
    bool m_wsaReady = false;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_failed{false};
    std::atomic<uint64_t> m_bytesThisSession{0};
    SampleSink m_sink;

    mutable std::mutex m_errorMutex;
    std::string m_error;
};
