/*
 * Native tests for the parts of the ambient pipeline that are shared between
 * the Linux and Windows backends.
 *
 * The ring buffer and the gate are backend-agnostic: whether samples arrive
 * from the PipeWire helper over a socket or from WASAPI process loopback, they
 * pass through these two classes unchanged. That makes them the highest-value
 * thing to test and the worst thing to get wrong -- a race here would surface
 * as intermittent audio corruption on someone else's machine, which is close to
 * undebuggable remotely.
 *
 * Both headers are free of Windows dependencies, so this builds and runs
 * natively on Linux under ASan/UBSan and ThreadSanitizer. See run-tests.sh.
 */

#include "AmbientGate.h"
#include "AmbientRingBuffer.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;
const char *g_currentTest = "";

void check(bool condition, const char *expression, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        printf("  FAIL %s:%d  %s\n", g_currentTest, line, expression);
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

void beginTest(const char *name) {
    g_currentTest = name;
    printf("- %s\n", name);
}

// ---------------------------------------------------------------- ring buffer

void testRingBasicRoundTrip() {
    beginTest("ring: writes come back in order");

    CAmbientRingBuffer ring(1024, 512);
    std::vector<int16_t> in(100);
    for (size_t i = 0; i < in.size(); ++i) {
        in[i] = (int16_t)(i + 1);
    }

    ring.write(in.data(), in.size());
    CHECK(ring.fill() == in.size());

    std::vector<int16_t> out(100, 0);
    const size_t got = ring.read(out.data(), out.size());

    CHECK(got == in.size());
    CHECK(out == in);
    CHECK(ring.fill() == 0);
    CHECK(ring.overruns() == 0);
    CHECK(ring.underruns() == 0);
}

void testRingUnderrunZeroFills() {
    beginTest("ring: reading more than available zero-fills the tail");

    CAmbientRingBuffer ring(1024, 512);
    const int16_t in[4] = {7, 7, 7, 7};
    ring.write(in, 4);

    std::vector<int16_t> out(16, 0x7FFF);
    const size_t got = ring.read(out.data(), out.size());

    CHECK(got == 4);
    CHECK(ring.underruns() == 1);
    for (size_t i = 0; i < 4; ++i) {
        CHECK(out[i] == 7);
    }
    // The zero-filled tail is what lets the mix site add unconditionally.
    for (size_t i = 4; i < out.size(); ++i) {
        CHECK(out[i] == 0);
    }
}

void testRingEmptyReadIsSilent() {
    beginTest("ring: reading an empty buffer yields silence, not garbage");

    CAmbientRingBuffer ring(1024, 512);
    std::vector<int16_t> out(32, 0x1234);
    const size_t got = ring.read(out.data(), out.size());

    CHECK(got == 0);
    for (const int16_t s : out) {
        CHECK(s == 0);
    }
}

void testRingWrapAround() {
    beginTest("ring: data survives repeated wrap-around");

    const size_t capacity = 64;
    CAmbientRingBuffer ring(capacity, 32);

    int16_t next = 1;
    for (int cycle = 0; cycle < 50; ++cycle) {
        int16_t in[20];
        for (int i = 0; i < 20; ++i) {
            in[i] = next++;
        }
        ring.write(in, 20);

        int16_t out[20] = {0};
        const size_t got = ring.read(out, 20);
        CHECK(got == 20);
        for (int i = 0; i < 20; ++i) {
            CHECK(out[i] == in[i]);
        }
    }
    CHECK(ring.overruns() == 0);
}

void testRingOverrunIsCountedNotCorrupting() {
    beginTest("ring: overflowing the buffer drops samples and counts it");

    const size_t capacity = 64;
    CAmbientRingBuffer ring(capacity, 32);

    std::vector<int16_t> in(200, 5);
    ring.write(in.data(), in.size());

    CHECK(ring.overruns() == 1);
    // One slot is reserved to distinguish full from empty.
    CHECK(ring.fill() == capacity - 1);

    std::vector<int16_t> out(capacity, 0);
    const size_t got = ring.read(out.data(), out.size());
    CHECK(got == capacity - 1);
    for (size_t i = 0; i < got; ++i) {
        CHECK(out[i] == 5);
    }
}

void testRingBacklogSkipKeepsNewest() {
    beginTest("ring: a backlog is skipped forward, keeping the newest audio");

    const size_t capacity = 1000;
    const size_t maxBacklog = 100;
    CAmbientRingBuffer ring(capacity, maxBacklog);

    // Write far more than the consumer is allowed to fall behind.
    std::vector<int16_t> in(500);
    for (size_t i = 0; i < in.size(); ++i) {
        in[i] = (int16_t)i;
    }
    ring.write(in.data(), in.size());

    std::vector<int16_t> out(50, 0);
    const size_t got = ring.read(out.data(), out.size());

    CHECK(got == 50);
    CHECK(ring.skipped() == 1);

    // Ambience is only useful while current, so the stale head must be the part
    // that was dropped -- the read should land near the end of what was written.
    CHECK(out[0] >= (int16_t)(in.size() - maxBacklog - 1));
    for (size_t i = 1; i < out.size(); ++i) {
        CHECK(out[i] == (int16_t)(out[0] + (int16_t)i));
    }
}

void testRingResetClearsCounters() {
    beginTest("ring: reset clears state and counters");

    CAmbientRingBuffer ring(64, 32);
    std::vector<int16_t> in(200, 1);
    ring.write(in.data(), in.size());
    int16_t out[8];
    ring.read(out, 8);

    CHECK(ring.overruns() > 0);
    ring.reset();

    CHECK(ring.fill() == 0);
    CHECK(ring.overruns() == 0);
    CHECK(ring.underruns() == 0);
    CHECK(ring.skipped() == 0);
}

/*
 * One producer thread and one consumer thread, never synchronised with each
 * other -- the shape of production use, and the configuration a race would
 * appear in. Run under ThreadSanitizer this is the test that matters.
 *
 * Samples carry a counter, so a torn or mis-ordered copy inside a single read
 * shows up as a discontinuity. Two kinds of gap are legitimate and neither is
 * detectable this way: a backlog skip happens before any copying, and an
 * overrun drops samples at the producer. The overrun case *is* visible, since
 * it leaves a hole in the middle of the ring's contents, so it bounds what the
 * assertions below can demand.
 */
struct ConcurrencyResult {
    size_t discontinuities = 0;
    size_t samplesRead = 0;
    uint32_t overruns = 0;
};

ConcurrencyResult runProducerConsumer(size_t capacity, size_t maxBacklog,
                                      int producerPaceUs, int runMs) {
    CAmbientRingBuffer ring(capacity, maxBacklog);
    std::atomic<bool> stop{false};
    std::atomic<size_t> discontinuities{0};
    std::atomic<size_t> samplesRead{0};

    std::thread producer([&] {
        int16_t counter = 0;
        int16_t chunk[240];
        while (!stop.load(std::memory_order_acquire)) {
            for (int i = 0; i < 240; ++i) {
                chunk[i] = counter++;
            }
            ring.write(chunk, 240);
            if (producerPaceUs > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(producerPaceUs));
            } else {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        int16_t out[480];
        while (!stop.load(std::memory_order_acquire)) {
            const size_t got = ring.read(out, 480);
            samplesRead.fetch_add(got, std::memory_order_relaxed);
            for (size_t i = 1; i < got; ++i) {
                if ((int16_t)(out[i - 1] + 1) != out[i]) {
                    discontinuities.fetch_add(1, std::memory_order_relaxed);
                }
            }
            std::this_thread::yield();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(runMs));
    stop.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    ConcurrencyResult result;
    result.discontinuities = discontinuities.load();
    result.samplesRead = samplesRead.load();
    result.overruns = ring.overruns();
    return result;
}

void testRingConcurrentAtAudioRate() {
    beginTest("ring: producer and consumer stay coherent at real audio rate");

    // 240 samples every 5 ms is 48 kHz, which is what the capture backend
    // actually delivers. At this rate the ring must never overflow, and the
    // consumer must see an unbroken stream.
    const ConcurrencyResult result = runProducerConsumer(4800, 960, 5000, 400);

    CHECK(result.samplesRead > 0);
    CHECK(result.overruns == 0);
    CHECK(result.discontinuities == 0);
}

void testRingConcurrentUnderStress() {
    beginTest("ring: an unthrottled producer only loses samples it reports");

    // Deliberately unrealistic: the producer writes as fast as it can, so the
    // ring overflows constantly. This is the high-contention case for
    // ThreadSanitizer. Each overrun can open at most one hole in the stream,
    // so anything beyond that count would mean real corruption.
    const ConcurrencyResult result = runProducerConsumer(4800, 960, 0, 400);

    CHECK(result.samplesRead > 0);
    CHECK(result.discontinuities <= result.overruns);
}

// ----------------------------------------------------------------------- gate

// Fills a buffer with a square wave at a given dBFS, which gives an exact RMS
// and so makes threshold assertions precise rather than approximate.
std::vector<int16_t> signalAtDbfs(double db, size_t samples) {
    const double amplitude = 32768.0 * pow(10.0, db / 20.0);
    std::vector<int16_t> buf(samples);
    for (size_t i = 0; i < samples; ++i) {
        buf[i] = (int16_t)((i % 2 == 0) ? amplitude : -amplitude);
    }
    return buf;
}

void testGateSuppressesQuiet() {
    beginTest("gate: audio below the threshold is silenced");

    CAmbientGate gate;
    gate.setThresholdDb(-35.0f);
    gate.reset();

    // -50 dBFS is about where the Phase 0 foliage/waves take sits.
    std::vector<int16_t> buf = signalAtDbfs(-50.0, CAmbientGate::WINDOW_SAMPLES * 4);
    gate.process(buf.data(), buf.size());

    CHECK(!gate.isOpen());
    for (const int16_t s : buf) {
        CHECK(s == 0);
    }
}

void testGatePassesLoud() {
    beginTest("gate: audio above the threshold passes through untouched");

    CAmbientGate gate;
    gate.setThresholdDb(-35.0f);
    gate.reset();

    std::vector<int16_t> buf = signalAtDbfs(-20.0, CAmbientGate::WINDOW_SAMPLES * 4);
    const std::vector<int16_t> original = buf;
    gate.process(buf.data(), buf.size());

    CHECK(gate.isOpen());
    CHECK(buf == original);
}

void testGateHoldBridgesGaps() {
    beginTest("gate: the hold bridges quiet gaps between loud windows");

    CAmbientGate gate;
    gate.setThresholdDb(-35.0f);
    gate.reset();

    // One loud window opens the gate.
    std::vector<int16_t> loud = signalAtDbfs(-20.0, CAmbientGate::WINDOW_SAMPLES);
    gate.process(loud.data(), loud.size());
    CHECK(gate.isOpen());

    /*
     * The hold is not a refinement. Without it, combat produced 116 open/close
     * transitions in 30 s -- audible chattering -- because the gaps between
     * shots fall below the threshold. These next windows are quiet and must
     * still pass.
     */
    for (int window = 0; window < CAmbientGate::HOLD_WINDOWS - 1; ++window) {
        std::vector<int16_t> quiet = signalAtDbfs(-60.0, CAmbientGate::WINDOW_SAMPLES);
        const std::vector<int16_t> before = quiet;
        gate.process(quiet.data(), quiet.size());
        CHECK(gate.isOpen());
        CHECK(quiet == before);
    }

    // Once the hold is spent, quiet closes it again.
    for (int window = 0; window < 3; ++window) {
        std::vector<int16_t> quiet = signalAtDbfs(-60.0, CAmbientGate::WINDOW_SAMPLES);
        gate.process(quiet.data(), quiet.size());
    }
    CHECK(!gate.isOpen());
}

void testGateThresholdBoundary() {
    beginTest("gate: the threshold lands where it is set");

    CAmbientGate gate;
    gate.setThresholdDb(-35.0f);

    gate.reset();
    std::vector<int16_t> below = signalAtDbfs(-36.0, CAmbientGate::WINDOW_SAMPLES);
    gate.process(below.data(), below.size());
    CHECK(!gate.isOpen());

    gate.reset();
    std::vector<int16_t> above = signalAtDbfs(-34.0, CAmbientGate::WINDOW_SAMPLES);
    gate.process(above.data(), above.size());
    CHECK(gate.isOpen());
}

void testGatePartialWindowInheritsState() {
    beginTest("gate: a trailing partial window inherits the current state");

    CAmbientGate gate;
    gate.setThresholdDb(-35.0f);
    gate.reset();

    // Closed: a short buffer that is too small to judge must be silenced
    // rather than passed on too few samples.
    std::vector<int16_t> shortQuiet = signalAtDbfs(-20.0, 100);
    gate.process(shortQuiet.data(), shortQuiet.size());
    CHECK(!gate.isOpen());
    for (const int16_t s : shortQuiet) {
        CHECK(s == 0);
    }

    // Open: the same short buffer now passes.
    std::vector<int16_t> loud = signalAtDbfs(-20.0, CAmbientGate::WINDOW_SAMPLES);
    gate.process(loud.data(), loud.size());
    CHECK(gate.isOpen());

    std::vector<int16_t> shortLoud = signalAtDbfs(-20.0, 100);
    const std::vector<int16_t> before = shortLoud;
    gate.process(shortLoud.data(), shortLoud.size());
    CHECK(shortLoud == before);
}

void testGateResetCloses() {
    beginTest("gate: reset closes the gate and clears the hold");

    CAmbientGate gate;
    gate.setThresholdDb(-35.0f);
    gate.reset();

    std::vector<int16_t> loud = signalAtDbfs(-20.0, CAmbientGate::WINDOW_SAMPLES);
    gate.process(loud.data(), loud.size());
    CHECK(gate.isOpen());

    // Each transmission starts fresh; a hold left over from the last one would
    // leak the first moments of ambience through before the gate has judged it.
    gate.reset();
    CHECK(!gate.isOpen());

    std::vector<int16_t> quiet = signalAtDbfs(-60.0, CAmbientGate::WINDOW_SAMPLES);
    gate.process(quiet.data(), quiet.size());
    CHECK(!gate.isOpen());
}

}  // namespace

int main() {
    printf("ambient pipeline tests\n\n");

    testRingBasicRoundTrip();
    testRingUnderrunZeroFills();
    testRingEmptyReadIsSilent();
    testRingWrapAround();
    testRingOverrunIsCountedNotCorrupting();
    testRingBacklogSkipKeepsNewest();
    testRingResetClearsCounters();
    testRingConcurrentAtAudioRate();
    testRingConcurrentUnderStress();

    testGateSuppressesQuiet();
    testGatePassesLoud();
    testGateHoldBridgesGaps();
    testGateThresholdBoundary();
    testGatePartialWindowInheritsState();
    testGateResetCloses();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
