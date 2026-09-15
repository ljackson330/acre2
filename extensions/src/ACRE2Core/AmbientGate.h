#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

/*
 * Amplitude noise gate for captured ambient audio.
 *
 * Deliberately not a content classifier. It suppresses quiet ambience --
 * foliage, waves, footsteps -- that would otherwise sit under every
 * transmission, and passes anything loud enough to be worth hearing.
 *
 * Defaults come from measurements in ambient/README.md. At -35 dBFS, 30 s of
 * walking through foliage beside waves is suppressed completely while 30 s of
 * firefight still passes 88.5% of its windows.
 *
 * The hold is load-bearing rather than a refinement: gating on the raw window
 * level alone produced 116 open/close transitions in 30 s of combat -- audible
 * chattering. A 200 ms hold cuts that to about 6 while *raising* how much
 * combat passes, because it bridges the gaps between shots.
 *
 * Heavy rain passes this gate, and that is correct. Rain and gunfire genuinely
 * overlap in level (-13.3 vs -12.9 dBFS peak in the Phase 0 takes), so no
 * amplitude threshold separates them -- and someone transmitting in a
 * rainstorm should sound like it.
 */
class CAmbientGate {
public:
    static constexpr int WINDOW_SAMPLES = 480;   // 10 ms at 48 kHz
    static constexpr int HOLD_WINDOWS = 20;      // 200 ms

    void reset() {
        this->m_holdRemaining = 0;
        this->m_open = false;
    }

    void setThresholdDb(float db) {
        this->m_thresholdLinear = 32768.0f * powf(10.0f, db / 20.0f);
    }

    /*
     * Applies the gate in place. Operates on whole windows of the buffer; a
     * trailing partial window inherits the current state rather than being
     * judged on too few samples.
     */
    void process(int16_t *samples, size_t count) {
        size_t offset = 0;
        while (offset < count) {
            const size_t remaining = count - offset;
            const size_t window = (remaining < WINDOW_SAMPLES) ? remaining
                                                              : WINDOW_SAMPLES;

            if (window == WINDOW_SAMPLES) {
                double sumSquares = 0.0;
                for (size_t i = 0; i < window; ++i) {
                    const double v = samples[offset + i];
                    sumSquares += v * v;
                }
                const float rms = (float)sqrt(sumSquares / window);

                if (rms >= this->m_thresholdLinear) {
                    this->m_open = true;
                    this->m_holdRemaining = HOLD_WINDOWS;
                } else if (this->m_holdRemaining > 0) {
                    this->m_holdRemaining--;
                } else {
                    this->m_open = false;
                }
            }

            if (!this->m_open) {
                memset(samples + offset, 0x00, window * sizeof(int16_t));
            }
            offset += window;
        }
    }

    bool isOpen() const { return this->m_open; }

private:
    float m_thresholdLinear = 32768.0f * 0.0177827941f;  // -35 dBFS
    int m_holdRemaining = 0;
    bool m_open = false;
};
