#pragma once

#include "compat.h"
#include "AmbientRingBuffer.h"

#include <atomic>
#include <thread>

/*
 * Ambient battle sound capture client.
 *
 * Reads the local game's audio from the ambient-helper process over a loopback
 * socket and makes it available to the outgoing voice path, so listeners hear
 * the transmitter's combat environment.
 *
 * Capture is scoped to Arma's own process on the helper side, never the output
 * device, so radio audio this client is receiving -- which plays through the
 * TeamSpeak process -- cannot be picked up and re-transmitted.
 *
 * Lifetime is tied to transmission: start() on startRadioSpeaking, stop() on
 * stopRadioSpeaking. The helper keeps its capture stream open and discards
 * samples while nothing is connected, so connecting is cheap.
 *
 * Every failure path is soft. If the helper is not running, or the connection
 * drops, the feature disables itself and the outgoing voice path is left
 * exactly as it would be without this class. A missing helper must never cost
 * the player their microphone.
 */
class CAmbientCapture {
public:
    static CAmbientCapture *getInstance();

    // Both are safe to call repeatedly and in any order.
    void start();
    void stop();

    inline bool isRunning() const { return m_running.load(std::memory_order_acquire); }

    /*
     * Consumer side, called from TeamSpeak's capture callback.
     *
     * Fills out[] with sampleCount mono samples, zero-filled if capture is not
     * running or has not delivered enough yet, so callers can mix
     * unconditionally. Returns the number of real samples produced.
     */
    size_t drain(int16_t *out, size_t sampleCount);

    // Records the real callback geometry once per transmission, to confirm
    // what TeamSpeak actually hands us rather than what the docs promise.
    void logFormatOnce(int sampleCount, int channels);

private:
    CAmbientCapture() = default;
    ~CAmbientCapture();
    CAmbientCapture(const CAmbientCapture &) = delete;
    CAmbientCapture &operator=(const CAmbientCapture &) = delete;

    void readLoop();
    bool connectToHelper();

    static constexpr unsigned short HELPER_PORT = 47806;
    static constexpr int CONNECT_TIMEOUT_MS = 200;
    static constexpr size_t SAMPLE_RATE = 48000;
    // 200 ms of slack for scheduling jitter, but never more than 100 ms behind
    // live -- beyond that the ambience lags the speech it accompanies.
    static constexpr size_t RING_CAPACITY = SAMPLE_RATE / 5;
    static constexpr size_t RING_MAX_BACKLOG = SAMPLE_RATE / 10;

    SOCKET m_socket = INVALID_SOCKET;
    bool m_wsaReady = false;
    std::thread m_thread;
    std::atomic<bool> m_running{false};

    // Step 2 instrumentation: proves start/stop alignment with transmit state.
    std::atomic<uint64_t> m_bytesThisSession{0};

    CAmbientRingBuffer m_ring{RING_CAPACITY, RING_MAX_BACKLOG};
    std::atomic<bool> m_formatLogged{false};
};
