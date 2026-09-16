/*
 * Ambient battle sound -- Windows capture probe.
 *
 * Standalone verification of the WASAPI process-loopback capture path. Builds
 * from the same CAmbientWasapiSource the plugin uses, so a green run here is
 * evidence about the shipping code rather than about a reimplementation of it.
 *
 * Two jobs:
 *
 *  1. Development, in a VM. The process-loopback contract -- activation, the
 *     negotiated format, PID scoping -- is provable against any process that
 *     renders audio, so a browser or a media player stands in for Arma. This is
 *     the only place that path can run at all: Proton does not implement
 *     process loopback, and CI runners have no audio endpoint.
 *
 *  2. First contact on a real machine. Someone with Windows and Arma runs one
 *     executable, with no mod install and no TeamSpeak, and sends back a WAV
 *     and this log. That turns a remote debugging session into a single data
 *     drop that can be analysed offline.
 *
 * The gate summary at the end reuses the shipped CAmbientGate against the
 * captured audio, so its output is directly comparable to the Phase 0 table in
 * ambient/README.md.
 */

#include "AmbientGate.h"
#include "AmbientWasapi.h"
#include "AmbientWavWriter.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    DWORD pid = 0;
    std::wstring process = L"arma3_x64.exe";
    int seconds = 20;
    std::string out = "ambient-probe.wav";
};

void usage() {
    printf(
        "usage: ambient-probe [options]\n"
        "\n"
        "  --process <name.exe>  process to capture (default: arma3_x64.exe)\n"
        "  --pid <n>             capture this PID instead of searching by name\n"
        "  --seconds <n>         capture duration (default: 20)\n"
        "  --out <file.wav>      output file (default: ambient-probe.wav)\n"
        "  --help                this message\n"
        "\n"
        "Start the target and make it produce sound, then run this.\n");
}

std::wstring widen(const char *s) {
    const int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(need > 0 ? need - 1 : 0, L'\0');
    if (need > 0) {
        MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], need);
    }
    return w;
}

bool parseArgs(int argc, char **argv, Options &opts) {
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const bool hasNext = (i + 1) < argc;

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            return false;
        } else if (strcmp(a, "--process") == 0 && hasNext) {
            opts.process = widen(argv[++i]);
        } else if (strcmp(a, "--pid") == 0 && hasNext) {
            opts.pid = (DWORD)strtoul(argv[++i], nullptr, 10);
        } else if (strcmp(a, "--seconds") == 0 && hasNext) {
            opts.seconds = atoi(argv[++i]);
        } else if (strcmp(a, "--out") == 0 && hasNext) {
            opts.out = argv[++i];
        } else {
            printf("unrecognised argument: %s\n\n", a);
            return false;
        }
    }
    return opts.seconds > 0;
}

double dbfs(double rms) {
    return (rms > 0.0) ? 20.0 * log10(rms / 32768.0) : -999.0;
}

// Mirrors the Phase 0 analysis: per-window RMS, then what fraction of windows
// the shipped gate leaves open.
void summarise(const std::vector<int16_t> &samples) {
    if (samples.empty()) {
        printf("\nNo audio captured.\n");
        printf("  The target rendered nothing for the whole capture window.\n");
        printf("  Make it produce sound and try again.\n");
        return;
    }

    double sumSquares = 0.0;
    int16_t peak = 0;
    size_t nonZero = 0;
    for (const int16_t s : samples) {
        const double v = s;
        sumSquares += v * v;
        const int16_t mag = (int16_t)((s < 0) ? -(int)s : (int)s);
        if (mag > peak) peak = mag;
        if (s != 0) ++nonZero;
    }

    printf("\n--- captured audio ---\n");
    printf("  samples      : %zu (%.2f s at 48 kHz)\n",
           samples.size(), samples.size() / 48000.0);
    printf("  peak         : %.1f dBFS\n", dbfs(peak));
    printf("  RMS          : %.1f dBFS\n", dbfs(sqrt(sumSquares / samples.size())));
    printf("  non-zero     : %.2f%%\n", 100.0 * nonZero / samples.size());

    if (nonZero == 0) {
        printf("\n  Every sample is exactly zero. Either the target was silent,\n");
        printf("  or process loopback attached to the wrong process.\n");
        return;
    }

    printf("\n--- gate (as shipped) ---\n");
    printf("  %-12s %s\n", "threshold", "windows open");
    for (const float threshold : {-40.0f, -35.0f, -30.0f}) {
        CAmbientGate gate;
        gate.setThresholdDb(threshold);
        gate.reset();

        std::vector<int16_t> copy(samples);
        size_t open = 0, windows = 0;
        for (size_t off = 0; off + CAmbientGate::WINDOW_SAMPLES <= copy.size();
             off += CAmbientGate::WINDOW_SAMPLES) {
            gate.process(copy.data() + off, CAmbientGate::WINDOW_SAMPLES);
            if (gate.isOpen()) ++open;
            ++windows;
        }
        printf("  %-12.1f %.1f%%%s\n", threshold,
               windows ? (100.0 * open / windows) : 0.0,
               (threshold == -35.0f) ? "   <- shipping default" : "");
    }
    printf("\n  Compare against ambient/README.md: a firefight passes ~88%% at\n");
    printf("  -35 dBFS, foliage and waves 0%%, heavy rain ~59%%.\n");
}

}  // namespace

int main(int argc, char **argv) {
    Options opts;
    if (!parseArgs(argc, argv, opts)) {
        usage();
        return 2;
    }

    printf("ACRE2 ambient capture probe\n");
    printf("===========================\n\n");

    DWORD pid = opts.pid;
    if (pid == 0) {
        const size_t matches = CAmbientWasapiSource::countProcesses(opts.process.c_str());
        pid = CAmbientWasapiSource::findProcessId(opts.process.c_str());
        if (pid == 0) {
            printf("FAIL: no process named '%ls' is running.\n", opts.process.c_str());
            printf("      Start it first, or pass --pid.\n");
            return 1;
        }
        printf("target       : %ls (pid %lu)\n", opts.process.c_str(), (unsigned long)pid);
        if (matches > 1) {
            printf("WARNING      : %zu processes match that name; captured the lowest\n"
                   "               PID. Pass --pid to choose.\n", matches);
        }
    } else {
        printf("target       : pid %lu (given explicitly)\n", (unsigned long)pid);
    }
    printf("duration     : %d s\n", opts.seconds);
    printf("output       : %s\n\n", opts.out.c_str());

    std::vector<int16_t> collected;
    std::mutex collectedMutex;
    std::atomic<uint64_t> callbacks{0};

    CAmbientWasapiSource source;
    const bool started = source.start(pid, [&](const int16_t *data, size_t count) {
        callbacks.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> guard(collectedMutex);
        collected.insert(collected.end(), data, data + count);
    });

    if (!started) {
        printf("FAIL: %s\n", source.lastError().c_str());
        return 1;
    }

    // start() only promises the thread launched; activation resolves on it.
    printf("activating process loopback...\n");
    for (int waited = 0; waited < 50; ++waited) {
        if (source.isRunning() || source.hasFailed()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (source.hasFailed()) {
        printf("\nFAIL: %s\n", source.lastError().c_str());
        printf("\nThis is the result that matters most. Send this whole log back.\n");
        return 1;
    }
    if (!source.isRunning()) {
        printf("\nFAIL: activation neither succeeded nor reported an error.\n");
        return 1;
    }

    printf("OK: activated. negotiated format: %s\n", source.describeFormat().c_str());
    printf("    (48000 Hz, 1 ch, 16-bit means no resampling is needed)\n\n");

    printf("capturing for %d s...\n", opts.seconds);
    for (int elapsed = 0; elapsed < opts.seconds; ++elapsed) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        size_t got;
        {
            std::lock_guard<std::mutex> guard(collectedMutex);
            got = collected.size();
        }
        printf("  %2d s  %.2f s of audio\r", elapsed + 1, got / 48000.0);
        fflush(stdout);
        if (source.hasFailed()) {
            printf("\ncapture failed mid-run: %s\n", source.lastError().c_str());
            break;
        }
    }
    printf("\n");

    source.stop();

    std::vector<int16_t> samples;
    {
        std::lock_guard<std::mutex> guard(collectedMutex);
        samples.swap(collected);
    }

    printf("\nbuffers delivered: %llu\n",
           (unsigned long long)callbacks.load(std::memory_order_relaxed));

    if (!samples.empty()) {
        CAmbientWavWriter wav;
        if (wav.open(opts.out, CAmbientWasapiSource::TARGET_RATE, 1)) {
            wav.write(samples.data(), samples.size());
            wav.close();
            printf("wrote %s\n", opts.out.c_str());
        } else {
            printf("could not open %s for writing\n", opts.out.c_str());
        }
    }

    summarise(samples);

    printf("\nSend back this log and %s.\n", opts.out.c_str());
    return samples.empty() ? 1 : 0;
}
