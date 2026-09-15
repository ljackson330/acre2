#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

/*
 * Minimal WAV writer for capturing the outgoing voice stream to disk.
 *
 * Exists to answer "is the ambience actually reaching the transmitted audio?"
 * directly, by writing the capture buffer after mixing -- which is exactly
 * what TeamSpeak encodes and sends. TeamSpeak's own capture test is not a
 * substitute: it mutes the microphone and processes audio differently, so it
 * cannot show what a listener would receive.
 *
 * Header is written with placeholder sizes and patched on close.
 */
class CAmbientWavWriter {
public:
    ~CAmbientWavWriter() { this->close(); }

    bool open(const std::string &path, uint32_t sampleRate, uint16_t channels) {
        this->close();
        this->m_file = fopen(path.c_str(), "wb");
        if (this->m_file == nullptr) {
            return false;
        }
        this->m_dataBytes = 0;

        const uint16_t bitsPerSample = 16;
        const uint16_t blockAlign = channels * bitsPerSample / 8;
        const uint32_t byteRate = sampleRate * blockAlign;

        fwrite("RIFF", 1, 4, this->m_file);
        this->writeU32(0);                       // patched on close
        fwrite("WAVEfmt ", 1, 8, this->m_file);
        this->writeU32(16);                      // PCM fmt chunk size
        this->writeU16(1);                       // PCM
        this->writeU16(channels);
        this->writeU32(sampleRate);
        this->writeU32(byteRate);
        this->writeU16(blockAlign);
        this->writeU16(bitsPerSample);
        fwrite("data", 1, 4, this->m_file);
        this->writeU32(0);                       // patched on close
        return true;
    }

    void write(const int16_t *samples, size_t count) {
        if (this->m_file == nullptr) {
            return;
        }
        fwrite(samples, sizeof(int16_t), count, this->m_file);
        this->m_dataBytes += static_cast<uint32_t>(count * sizeof(int16_t));
    }

    void close() {
        if (this->m_file == nullptr) {
            return;
        }
        fseek(this->m_file, 4, SEEK_SET);
        this->writeU32(36 + this->m_dataBytes);
        fseek(this->m_file, 40, SEEK_SET);
        this->writeU32(this->m_dataBytes);
        fclose(this->m_file);
        this->m_file = nullptr;
    }

    bool isOpen() const { return this->m_file != nullptr; }
    uint32_t bytesWritten() const { return this->m_dataBytes; }

private:
    void writeU32(uint32_t v) { fwrite(&v, 4, 1, this->m_file); }
    void writeU16(uint16_t v) { fwrite(&v, 2, 1, this->m_file); }

    FILE *m_file = nullptr;
    uint32_t m_dataBytes = 0;
};
