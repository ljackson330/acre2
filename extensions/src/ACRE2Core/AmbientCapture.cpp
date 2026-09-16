#include "AmbientCapture.h"

#include "AcreSettings.h"
#include "AmbientSocketSource.h"
#include "AmbientWasapi.h"
#include "Log.h"
#include "RadioEffect.h"

#include <cmath>

CAmbientCapture *CAmbientCapture::getInstance() {
    static CAmbientCapture instance;
    return &instance;
}

CAmbientCapture::~CAmbientCapture() {
    this->stop();
}

bool CAmbientCapture::isRunning() const {
    return this->m_source && this->m_source->isRunning();
}

/*
 * Choose a backend, without ever waiting for one.
 *
 * WASAPI process loopback is the real Windows path and is tried first. Under
 * Wine it fails -- Proton exports ActivateAudioInterfaceAsync but has no
 * process-loopback device behind it -- and the helper socket takes over, so the
 * same binary serves both a Windows player and this development setup with no
 * build flag or setting to get wrong.
 *
 * Crucially this does not probe and wait. start() runs on the RPC thread that
 * handles startRadioSpeaking, which is in the player's push-to-talk path, and
 * blocking there for the second or so a doomed activation takes would stall
 * their transmission -- the very thing connectToHelper()'s bounded select()
 * exists to avoid. Instead the WASAPI source is simply started, activation
 * resolves on its own thread, and the *next* key-up sees the verdict. The cost
 * is that the first transmission of a session has no ambience on Linux, which
 * is a far better trade than a stalled PTT.
 */
IAmbientSource *CAmbientCapture::resolveSource() {
    if (!this->m_source) {
        const size_t instances =
            CAmbientWasapiSource::countProcesses(CAmbientWasapiSource::GAME_EXECUTABLE);
        if (instances > 1) {
            // Two instances is a test setup, not normal play. The backend takes
            // the lowest PID, which is a guess, so say so rather than letting it
            // look deliberate.
            LOG("AMBIENT: %zu arma3_x64.exe processes running -- capturing the "
                "lowest PID, which may not be the instance you are playing",
                instances);
        }
        this->m_source.reset(new CAmbientWasapiSource());
        return this->m_source.get();
    }

    // Process loopback gets tried once per session. Once it has definitively
    // failed, fall back permanently rather than paying a failed activation on
    // every single key-up.
    if (!this->m_fellBack && this->m_source->hasFailed()) {
        LOG("AMBIENT: %s unavailable (%s) -- using the helper from now on",
            this->m_source->name(), this->m_source->lastError().c_str());
        this->m_source->stop();
        this->m_source.reset(new CAmbientSocketSource());
        this->m_fellBack = true;
    }

    return this->m_source.get();
}

void CAmbientCapture::start() {
    // Reset the pipeline before the source can deliver into it.
    this->m_ring.reset();
    this->m_gate.reset();
    this->m_gate.setThresholdDb(CAcreSettings::getInstance()->getAmbientGateThreshold());
    this->m_mixedCallbacks.store(0, std::memory_order_release);
    this->m_ambientRmsSum.store(0.0, std::memory_order_release);
    this->m_formatLogged.store(false, std::memory_order_release);

    IAmbientSource *source = this->resolveSource();
    if (source == nullptr) {
        return;
    }

    auto sink = [this](const int16_t *samples, size_t count) {
        this->m_ring.write(samples, count);
    };

    if (!source->start(sink)) {
        return;  // fail soft: no ambient audio, voice path untouched
    }

    this->openDumpFile();
    LOG("AMBIENT: capture started (%s)", source->name());
}

void CAmbientCapture::selfTest() {
    LOG("AMBIENT SELFTEST: starting -- capturing for 8 s");

    this->start();

    // Drain the way the capture callback does, so the ring, the gate and the
    // backend are all exercised rather than just the activation.
    const size_t chunk = 480;                 // 10 ms, one gate window
    int16_t buffer[chunk];
    double sumSquares = 0.0;
    size_t realSamples = 0;
    int16_t peak = 0;

    for (int tick = 0; tick < 800; ++tick) {  // 800 x 10 ms = 8 s
        const size_t produced = this->drain(buffer, chunk);
        realSamples += produced;
        for (size_t i = 0; i < produced; ++i) {
            const double v = buffer[i];
            sumSquares += v * v;
            const int16_t mag = (int16_t)((buffer[i] < 0) ? -(int)buffer[i] : (int)buffer[i]);
            if (mag > peak) {
                peak = mag;
            }
        }
        Sleep(10);
    }

    const char *backend = this->m_source ? this->m_source->name() : "none";
    if (realSamples == 0) {
        LOG("AMBIENT SELFTEST: FAILED -- backend '%s' delivered no samples (%s)",
            backend, this->m_source ? this->m_source->lastError().c_str() : "no source");
    } else {
        const double rms = sqrt(sumSquares / realSamples);
        LOG("AMBIENT SELFTEST: OK -- backend '%s', %zu samples (%.2f s), "
            "peak %.1f dBFS, post-gate RMS %.1f dBFS",
            backend, realSamples, realSamples / (double)SAMPLE_RATE,
            (peak > 0) ? 20.0 * log10(peak / 32768.0) : -999.0,
            (rms > 0.0) ? 20.0 * log10(rms / 32768.0) : -999.0);
        LOG("AMBIENT SELFTEST: post-gate RMS is measured after the noise gate, "
            "so silence here with a loud game means the gate is closing, not "
            "that capture failed");
        // Sleep(10) really sleeps ~15 ms at the default timer resolution, so
        // this loop consumes slower than the backend produces and the ring
        // trims itself. The real consumer is TeamSpeak's capture callback,
        // driven by the audio clock at exactly the right rate, so the skips
        // reported below are an artifact of this test and not a capture fault.
        LOG("AMBIENT SELFTEST: ring skips/underruns on the next line come from "
            "this test's own polling loop, not from the capture path");
    }

    this->stop();
}

void CAmbientCapture::openDumpFile() {
    const std::string dumpPath = CAcreSettings::getInstance()->getAmbientDumpFile();
    if (!dumpPath.empty()) {
        if (this->m_dump.open(dumpPath, (uint32_t)SAMPLE_RATE, 1)) {
            LOG("AMBIENT: dumping outgoing stream to %s", dumpPath.c_str());
        } else {
            LOG("AMBIENT: could not open dump file %s", dumpPath.c_str());
        }
    }

    const std::string splitPath = CAcreSettings::getInstance()->getAmbientDumpSplitFile();
    if (!splitPath.empty()) {
        if (this->m_splitDump.open(splitPath, (uint32_t)SAMPLE_RATE, 2)) {
            LOG("AMBIENT: split dump to %s -- left is ambience before the gate, "
                "right is the microphone before mixing", splitPath.c_str());
        } else {
            LOG("AMBIENT: could not open split dump %s", splitPath.c_str());
        }
    }

    const float quality = CAcreSettings::getInstance()->getAmbientDumpSignalQuality();
    if (quality > 0.0f && this->m_dump.isOpen()) {
        this->m_dumpRadio.reset(new CRadioEffect());
        this->m_dumpRadio->setParam("signalQuality", quality);
        LOG("AMBIENT: dump will carry the receive-side radio effect at signal "
            "quality %.2f -- this is what a listener hears, minus the Opus "
            "round trip", quality);
    } else {
        this->m_dumpRadio.reset();
    }
}

void CAmbientCapture::stop() {
    if (!this->m_source) {
        return;
    }

    const uint64_t bytes = this->m_source->bytesDelivered();
    this->m_source->stop();

    const uint32_t mixed = this->m_mixedCallbacks.load(std::memory_order_acquire);
    if (mixed > 0) {
        const double meanRms = this->m_ambientRmsSum.load(std::memory_order_acquire) / mixed;
        const double db = (meanRms > 0.0) ? 20.0 * log10(meanRms / 32768.0) : -999.0;
        LOG("AMBIENT: mixed into %u callbacks; mean ambient level %.1f dBFS", mixed, db);
    } else {
        LOG("AMBIENT: mixed into 0 callbacks -- nothing was added to the outgoing stream");
    }

    if (this->m_splitDump.isOpen()) {
        const uint32_t split = this->m_splitDump.bytesWritten();
        this->m_splitDump.close();
        LOG("AMBIENT: split dump closed -- %.2f s written", split / 4.0 / SAMPLE_RATE);
    }

    if (this->m_dump.isOpen()) {
        const uint32_t dumped = this->m_dump.bytesWritten();
        this->m_dump.close();
        this->m_dumpRadio.reset();
        LOG("AMBIENT: dump closed -- %.2f s written", dumped / 2.0 / SAMPLE_RATE);
    }

    LOG("AMBIENT: capture stopped -- %llu bytes (%.2f s of audio); "
        "ring: overruns=%u underruns=%u skips=%u residual=%zu samples",
        (unsigned long long)bytes, bytes / 2.0 / SAMPLE_RATE,
        this->m_ring.overruns(), this->m_ring.underruns(),
        this->m_ring.skipped(), this->m_ring.fill());
}

size_t CAmbientCapture::drain(int16_t *out, size_t sampleCount, int16_t *preGate) {
    if (!this->isRunning()) {
        memset(out, 0x00, sampleCount * sizeof(int16_t));
        if (preGate != nullptr) {
            memset(preGate, 0x00, sampleCount * sizeof(int16_t));
        }
        return 0;
    }
    const size_t produced = this->m_ring.read(out, sampleCount);
    // Copied before gating so a diagnostic dump can carry ungated ambience and
    // gate settings stay adjustable offline.
    if (preGate != nullptr) {
        memcpy(preGate, out, sampleCount * sizeof(int16_t));
    }
    if (produced > 0) {
        // Gate only the real samples. read() zero-fills any shortfall, and
        // including that tail would drag the window RMS down and close the
        // gate on a buffer that did contain audio.
        this->m_gate.process(out, produced);
    }
    return produced;
}

void CAmbientCapture::logFormatOnce(int sampleCount, int channels) {
    if (!this->m_formatLogged.exchange(true, std::memory_order_acq_rel)) {
        LOG("AMBIENT: capture callback format -- sampleCount=%d channels=%d "
            "(backend supplies mono 48 kHz)", sampleCount, channels);
    }
}

void CAmbientCapture::noteMixed(double ambientRms) {
    this->m_mixedCallbacks.fetch_add(1, std::memory_order_relaxed);
    // fetch_add is not available for double; a relaxed read-modify-write is
    // fine here because only the capture callback thread touches this.
    this->m_ambientRmsSum.store(
        this->m_ambientRmsSum.load(std::memory_order_relaxed) + ambientRms,
        std::memory_order_relaxed);
}

void CAmbientCapture::dumpOutgoing(const short *samples, int sampleCount, int channels) {
    if (!this->m_dump.isOpen() || channels <= 0 || sampleCount <= 0) {
        return;
    }

    // CFilterRadio works into a fixed 4096-sample buffer, so never hand it more
    // than that in one go. TeamSpeak's callback is far smaller in practice, but
    // the dump must not be the thing that overruns it.
    constexpr int MAX_CHUNK = 4096;
    int16_t mono[MAX_CHUNK];

    for (int offset = 0; offset < sampleCount; offset += MAX_CHUNK) {
        const int remaining = sampleCount - offset;
        const int frames = (remaining < MAX_CHUNK) ? remaining : MAX_CHUNK;

        // Downmix so the dump matches its declared mono header, and so the
        // radio effect below gets the single contiguous channel it expects --
        // which is also how the receive path feeds it.
        if (channels == 1) {
            memcpy(mono, samples + offset, frames * sizeof(int16_t));
        } else {
            for (int frame = 0; frame < frames; ++frame) {
                int32_t sum = 0;
                const int base = (offset + frame) * channels;
                for (int channel = 0; channel < channels; ++channel) {
                    sum += samples[base + channel];
                }
                mono[frame] = static_cast<int16_t>(sum / channels);
            }
        }

        if (this->m_dumpRadio) {
            this->m_dumpRadio->process(mono, frames);
        }
        this->m_dump.write(mono, frames);
    }
}

void CAmbientCapture::dumpSplit(const int16_t *ambient, const short *voice,
                                int sampleCount, int channels) {
    if (!this->m_splitDump.isOpen() || channels <= 0 || sampleCount <= 0) {
        return;
    }

    constexpr int MAX_CHUNK = 2048;
    int16_t interleaved[MAX_CHUNK * 2];

    for (int offset = 0; offset < sampleCount; offset += MAX_CHUNK) {
        const int remaining = sampleCount - offset;
        const int frames = (remaining < MAX_CHUNK) ? remaining : MAX_CHUNK;

        for (int frame = 0; frame < frames; ++frame) {
            // Voice is downmixed to match the ambience, which is always mono.
            int32_t sum = 0;
            const int base = (offset + frame) * channels;
            for (int channel = 0; channel < channels; ++channel) {
                sum += voice[base + channel];
            }
            interleaved[(frame * 2)] = ambient[offset + frame];
            interleaved[(frame * 2) + 1] = static_cast<int16_t>(sum / channels);
        }
        this->m_splitDump.write(interleaved, (size_t)frames * 2);
    }
}
