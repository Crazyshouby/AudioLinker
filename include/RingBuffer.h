#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>
#include <algorithm>

// Lock-free single-producer/single-consumer ring buffer of float samples.
// One instance per (capture -> single output) link: the capture thread is
// the sole writer, the matching render thread is the sole reader.
class RingBuffer {
public:
    explicit RingBuffer(size_t capacityFrames) {
        size_t capacity = 1;
        while (capacity < capacityFrames) capacity <<= 1;
        mask_ = capacity - 1;
        buffer_.resize(capacity, 0.0f);
    }

    size_t capacity() const { return buffer_.size(); }

    size_t availableToRead() const {
        return writeIndex_.load(std::memory_order_acquire) - readIndex_.load(std::memory_order_acquire);
    }

    size_t availableToWrite() const {
        return buffer_.size() - availableToRead();
    }

    // A power-of-two ring wraps at most once, so every transfer is at most
    // two contiguous segments -- bulk memcpy/memset instead of a per-sample
    // masked loop, which the compiler can't vectorize.
    size_t write(const float* data, size_t count) {
        size_t freeSpace = availableToWrite();
        size_t toWrite = std::min(count, freeSpace);
        size_t w = writeIndex_.load(std::memory_order_relaxed);
        size_t idx = w & mask_;
        size_t first = std::min(toWrite, buffer_.size() - idx);
        std::memcpy(buffer_.data() + idx, data, first * sizeof(float));
        std::memcpy(buffer_.data(), data + first, (toWrite - first) * sizeof(float));
        writeIndex_.store(w + toWrite, std::memory_order_release);
        return toWrite;
    }

    // Writes silence (zeros) to advance the write cursor without real data.
    // Used to pre-fill the per-output delay offset for latency alignment.
    size_t writeSilence(size_t count) {
        size_t freeSpace = availableToWrite();
        size_t toWrite = std::min(count, freeSpace);
        size_t w = writeIndex_.load(std::memory_order_relaxed);
        size_t idx = w & mask_;
        size_t first = std::min(toWrite, buffer_.size() - idx);
        std::memset(buffer_.data() + idx, 0, first * sizeof(float));
        std::memset(buffer_.data(), 0, (toWrite - first) * sizeof(float));
        writeIndex_.store(w + toWrite, std::memory_order_release);
        return toWrite;
    }

    size_t read(float* data, size_t count) {
        size_t avail = availableToRead();
        size_t toRead = std::min(count, avail);
        size_t r = readIndex_.load(std::memory_order_relaxed);
        size_t idx = r & mask_;
        size_t first = std::min(toRead, buffer_.size() - idx);
        std::memcpy(data, buffer_.data() + idx, first * sizeof(float));
        std::memcpy(data + first, buffer_.data(), (toRead - first) * sizeof(float));
        readIndex_.store(r + toRead, std::memory_order_release);
        return toRead;
    }

    // Drops `count` frames from the read side without copying (used by
    // drift correction to shrink a buffer that has drifted too full).
    size_t drop(size_t count) {
        size_t avail = availableToRead();
        size_t toDrop = std::min(count, avail);
        readIndex_.fetch_add(toDrop, std::memory_order_release);
        return toDrop;
    }

private:
    std::vector<float> buffer_;
    size_t mask_;
    std::atomic<size_t> writeIndex_{0};
    std::atomic<size_t> readIndex_{0};
};
