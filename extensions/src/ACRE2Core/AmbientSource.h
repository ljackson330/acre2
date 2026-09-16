#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

/*
 * Where ambient audio comes from.
 *
 * Two backends exist because the capture API is the only genuinely
 * platform-specific part of this feature:
 *
 *   - CAmbientWasapiSource   Windows, WASAPI process loopback, in-process.
 *   - CAmbientSocketSource   Linux under Wine, reading ambient-helper.py over a
 *                            loopback socket. Proton does not implement
 *                            process loopback, so the plugin cannot capture
 *                            for itself there.
 *
 * Everything downstream of this interface -- the ring buffer, the noise gate,
 * the mix into TeamSpeak's capture buffer, the dump file, the telemetry -- is
 * shared, and was proven end to end on Linux before the Windows backend
 * existed. Keeping the seam here is what lets the Windows port reuse all of it
 * unchanged rather than reimplementing it.
 *
 * Every implementation must fail soft. If capture cannot start, or stops
 * working mid-transmission, the outgoing voice path must be left exactly as it
 * would be without this feature. A broken backend must never cost the player
 * their microphone.
 */
class IAmbientSource {
public:
    /*
     * Called from the source's own thread with signed 16-bit mono samples at
     * 48 kHz. That format is the interface contract: it is what TeamSpeak's
     * capture callback expects, so no implementation may hand up anything else
     * and no consumer has to convert.
     */
    using SampleSink = std::function<void(const int16_t *, size_t)>;

    virtual ~IAmbientSource() = default;

    // Returns false if capture could not be started at all. Implementations
    // that resolve asynchronously may return true and fail later, so this is
    // not a promise that audio will arrive -- poll isRunning().
    virtual bool start(SampleSink sink) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

    // True once this backend has definitively failed for this session, as
    // opposed to merely not running. Backends that resolve asynchronously set
    // it from their own thread, which is what lets the pipeline pick a
    // different one without ever waiting on a result.
    virtual bool hasFailed() const = 0;

    // For logs: which backend this is, and why it failed if it did.
    virtual const char *name() const = 0;
    virtual std::string lastError() const = 0;

    // Bytes delivered since the last start(), for the per-transmission report.
    virtual uint64_t bytesDelivered() const = 0;
};
