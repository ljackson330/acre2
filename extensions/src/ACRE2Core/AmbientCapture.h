#pragma once

#include "compat.h"
#include "AmbientGate.h"
#include "AmbientRingBuffer.h"
#include "AmbientSource.h"
#include "AmbientWavWriter.h"

#include <atomic>
#include <memory>

/*
 * Ambient battle sound: the pipeline between a capture backend and the
 * outgoing voice stream.
 *
 * Owns everything that is not platform-specific -- the ring buffer, the noise
 * gate, the dump file and the per-transmission telemetry -- and holds an
 * IAmbientSource for the part that is. The source is chosen once and reused:
 * WASAPI process loopback where it works, the Linux helper socket otherwise.
 *
 * Lifetime is tied to transmission: start() on startRadioSpeaking, stop() on
 * stopRadioSpeaking. Capture therefore only runs during actual transmission
 * windows, which are short and infrequent regardless of how much is happening
 * elsewhere on the server.
 *
 * Every failure path is soft. If no backend works, or one drops mid-
 * transmission, the outgoing voice path is left exactly as it would be without
 * this class. Ambient sound is a nicety; a player's microphone is not.
 */
class CAmbientCapture {
public:
    static CAmbientCapture *getInstance();

    // Both are safe to call repeatedly and in any order.
    void start();
    void stop();

    bool isRunning() const;

    /*
     * Consumer side, called from TeamSpeak's capture callback.
     *
     * Fills out[] with sampleCount mono samples, noise-gated and ready to mix,
     * zero-filled if capture is not running or has not delivered enough yet,
     * so callers can mix unconditionally. Returns the number of real samples
     * produced.
     */
    size_t drain(int16_t *out, size_t sampleCount);

    // Records the real callback geometry once per transmission, to confirm
    // what TeamSpeak actually hands us rather than what the docs promise.
    void logFormatOnce(int sampleCount, int channels);

    // Writes the post-mix outgoing buffer to disk when a dump file is
    // configured. This is the transmitted stream, so it is both the proof the
    // feature works and the demo of what a listener hears.
    void dumpOutgoing(const short *samples, int sampleCount, int channels);

    // Per-transmission mix telemetry, reported on stop().
    void noteMixed(double ambientRms);

    /*
     * Diagnostic: capture for a few seconds and report what arrived.
     *
     * Exists because every other way of exercising the capture backend needs
     * the game running and a radio keyed. A tester on a machine we cannot
     * reach can set ambientSelfTest, start TeamSpeak, and send back a log line
     * that says whether capture works at all -- which separates "the backend
     * is broken" from "the transmit hooks never fired" without a debugging
     * session.
     *
     * Blocks for several seconds, so callers run it on their own thread.
     */
    void selfTest();

private:
    CAmbientCapture() = default;
    ~CAmbientCapture();
    CAmbientCapture(const CAmbientCapture &) = delete;
    CAmbientCapture &operator=(const CAmbientCapture &) = delete;

    /*
     * Picks a backend on first use and remembers it.
     *
     * Probing is not free -- a WASAPI activation that is going to fail still
     * costs most of a second -- and start() is on the player's push-to-talk
     * path, so paying it once per TeamSpeak session rather than once per
     * transmission is the difference between an unnoticeable delay and a
     * missing first second of every key-up.
     */
    IAmbientSource *resolveSource();
    void openDumpFile();

    static constexpr size_t SAMPLE_RATE = 48000;
    // 200 ms of slack for scheduling jitter, but never more than 100 ms behind
    // live -- beyond that the ambience lags the speech it accompanies.
    static constexpr size_t RING_CAPACITY = SAMPLE_RATE / 5;
    static constexpr size_t RING_MAX_BACKLOG = SAMPLE_RATE / 10;

    std::unique_ptr<IAmbientSource> m_source;
    bool m_fellBack = false;

    CAmbientRingBuffer m_ring{RING_CAPACITY, RING_MAX_BACKLOG};
    // Owned here rather than at the mix site so its hold state is reset with
    // each transmission and its threshold is resolved once, not per callback.
    CAmbientGate m_gate;
    CAmbientWavWriter m_dump;
    std::atomic<uint32_t> m_mixedCallbacks{0};
    std::atomic<double> m_ambientRmsSum{0.0};
    std::atomic<bool> m_formatLogged{false};
};
