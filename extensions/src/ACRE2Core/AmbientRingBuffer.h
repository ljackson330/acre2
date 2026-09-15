#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

/*
 * Single-producer single-consumer ring buffer for ambient capture samples.
 *
 * Producer is the socket reader thread; consumer is TeamSpeak's capture
 * callback. Neither takes a lock: the callback runs on the audio path, where
 * blocking on a mutex held by a thread waiting on a socket would risk audible
 * glitches in the player's own outgoing voice.
 *
 * Each cursor has exactly one writer -- the producer owns m_write, the
 * consumer owns m_read -- which is what makes the lock-free access safe.
 *
 * Bounded latency is therefore the consumer's job, not the producer's. Ambient
 * audio is only useful while it is current: if a backlog builds, the
 * transmitter's gunfire drifts out of sync with their speech. So the consumer
 * discards its own backlog down to m_maxBacklog before reading. Having the
 * producer drop old samples instead would mean writing the consumer's cursor
 * from the wrong thread, which races with an in-flight read.
 */
class CAmbientRingBuffer {
public:
    // maxBacklogSamples bounds how far behind live the consumer is allowed to
    // fall before it skips forward. Must be < capacitySamples.
    CAmbientRingBuffer(size_t capacitySamples, size_t maxBacklogSamples)
        : m_buffer(capacitySamples),
          m_capacity(capacitySamples),
          m_maxBacklog(maxBacklogSamples < capacitySamples ? maxBacklogSamples
                                                           : capacitySamples - 1) {}

    void reset() {
        this->m_read.store(0, std::memory_order_relaxed);
        this->m_write.store(0, std::memory_order_relaxed);
        this->m_overruns.store(0, std::memory_order_relaxed);
        this->m_underruns.store(0, std::memory_order_relaxed);
        this->m_skipped.store(0, std::memory_order_relaxed);
    }

    // Producer side. Writes what fits; excess is dropped and counted. The
    // consumer's trim normally keeps this from happening at all.
    void write(const int16_t *samples, size_t count) {
        const size_t write = this->m_write.load(std::memory_order_relaxed);
        const size_t read = this->m_read.load(std::memory_order_acquire);
        const size_t free = this->m_capacity - 1 - this->used(read, write);

        if (count > free) {
            this->m_overruns.fetch_add(1, std::memory_order_relaxed);
            count = free;
        }
        if (count == 0) {
            return;
        }

        const size_t firstChunk = (this->m_capacity - write) < count
                                ? (this->m_capacity - write) : count;
        memcpy(&this->m_buffer[write], samples, firstChunk * sizeof(int16_t));
        if (count > firstChunk) {
            memcpy(&this->m_buffer[0], samples + firstChunk,
                   (count - firstChunk) * sizeof(int16_t));
        }
        this->m_write.store((write + count) % this->m_capacity, std::memory_order_release);
    }

    // Consumer side. Any shortfall is zero-filled so callers can mix
    // unconditionally. Returns how many real samples were produced.
    size_t read(int16_t *out, size_t count) {
        size_t read = this->m_read.load(std::memory_order_relaxed);
        const size_t write = this->m_write.load(std::memory_order_acquire);
        size_t available = this->used(read, write);

        // Drop our own backlog rather than play catch-up through stale audio.
        const size_t allowed = (this->m_maxBacklog > count) ? this->m_maxBacklog : count;
        if (available > allowed) {
            const size_t skip = available - allowed;
            read = (read + skip) % this->m_capacity;
            available = allowed;
            this->m_skipped.fetch_add(1, std::memory_order_relaxed);
        }

        const size_t take = (available < count) ? available : count;
        if (take < count) {
            this->m_underruns.fetch_add(1, std::memory_order_relaxed);
        }

        const size_t firstChunk = (this->m_capacity - read) < take
                                ? (this->m_capacity - read) : take;
        memcpy(out, &this->m_buffer[read], firstChunk * sizeof(int16_t));
        if (take > firstChunk) {
            memcpy(out + firstChunk, &this->m_buffer[0],
                   (take - firstChunk) * sizeof(int16_t));
        }
        if (take < count) {
            memset(out + take, 0x00, (count - take) * sizeof(int16_t));
        }

        this->m_read.store((read + take) % this->m_capacity, std::memory_order_release);
        return take;
    }

    size_t fill() const {
        return this->used(this->m_read.load(std::memory_order_acquire),
                          this->m_write.load(std::memory_order_acquire));
    }

    size_t capacity() const { return this->m_capacity; }
    uint32_t overruns() const { return this->m_overruns.load(std::memory_order_relaxed); }
    uint32_t underruns() const { return this->m_underruns.load(std::memory_order_relaxed); }
    uint32_t skipped() const { return this->m_skipped.load(std::memory_order_relaxed); }

private:
    size_t used(size_t read, size_t write) const {
        return (write >= read) ? (write - read) : (this->m_capacity - read + write);
    }

    std::vector<int16_t> m_buffer;
    size_t m_capacity;
    size_t m_maxBacklog;
    std::atomic<size_t> m_read{0};
    std::atomic<size_t> m_write{0};
    std::atomic<uint32_t> m_overruns{0};
    std::atomic<uint32_t> m_underruns{0};
    std::atomic<uint32_t> m_skipped{0};
};
