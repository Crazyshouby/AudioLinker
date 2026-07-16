#pragma once

#include <vector>
#include <cstddef>
#include <algorithm>

// Streaming converter for interleaved float audio: channel remap + 4-point
// Hermite (Catmull-Rom) sample-rate conversion. The drift P-controller keeps
// the ratio permanently a hair away from 1.0, so interpolation runs on every
// sample of every output: linear interpolation there cost a slowly-modulated
// treble loss (up to ~-3dB at fs/4 when the fractional phase crosses 0.5).
// The cubic kernel keeps the passband flat for ~4 mul/adds more per sample.
//
// Continuity across chunks: the last 3 input frames are carried in tail_ and
// prepended to each chunk (work_ = [tail][remapped]), so the kernel can look
// one frame back and two frames ahead across packet boundaries — processing
// packets in isolation produced an audible crackle every ~10 ms.
class StreamResampler {
public:
    void configure(int inChannels, int inRate, int outChannels, int outRate) {
        inCh_ = inChannels;
        inRate_ = inRate;
        outCh_ = outChannels;
        outRate_ = outRate;
        ratio_ = static_cast<double>(inRate) / static_cast<double>(outRate);
        adjustedRatio_ = ratio_;
        pos_ = 3.0; // work_ index of the first real (non-history) frame
        hasTail_ = false;
        tail_.assign(3 * static_cast<size_t>(outCh_), 0.0f);
    }

    void setSpeedAdjustment(double adj) {
        adjustedRatio_ = ratio_ * adj;
    }

    // channelMode: 0 = stereo passthrough, 1 = left channel only (speaker is
    // the left half of a stereo pair), 2 = right channel only.
    void process(const float* in, size_t inFrames, int channelMode, std::vector<float>& out) {
        out.clear();
        if (inFrames == 0) return;

        // One continuous timeline per chunk: work_[0..2] = the previous
        // chunk's last 3 frames, work_[3..n+2] = this chunk remapped.
        const size_t oc = static_cast<size_t>(outCh_);
        work_.resize((inFrames + 3) * oc);
        for (size_t f = 0; f < inFrames; ++f) {
            const float* src = in + f * inCh_;
            float* dst = &work_[(f + 3) * oc];
            if (channelMode == 1 || channelMode == 2) {
                float v = src[(channelMode == 2 && inCh_ >= 2) ? 1 : 0];
                for (int c = 0; c < outCh_; ++c) dst[c] = v;
            } else if (inCh_ == outCh_) {
                for (int c = 0; c < outCh_; ++c) dst[c] = src[c];
            } else if (inCh_ == 1) {
                for (int c = 0; c < outCh_; ++c) dst[c] = src[0];
            } else if (outCh_ == 1) {
                float sum = 0.0f;
                for (int c = 0; c < inCh_; ++c) sum += src[c];
                dst[0] = sum / static_cast<float>(inCh_);
            } else {
                for (int c = 0; c < outCh_; ++c) dst[c] = src[c % inCh_];
            }
        }
        if (hasTail_) {
            std::copy(tail_.begin(), tail_.end(), work_.begin());
        } else {
            // No history yet: replicate the first frame backwards so the
            // kernel's look-behind sees something sane on the very first call.
            for (size_t k = 0; k < 3; ++k) {
                std::copy(work_.begin() + 3 * oc, work_.begin() + 4 * oc,
                          work_.begin() + k * oc);
            }
            hasTail_ = true;
            pos_ = 3.0;
        }

        const double n = static_cast<double>(inFrames);
        if (inRate_ == outRate_ && adjustedRatio_ == 1.0) {
            // Exact-passthrough shortcut (pre-start default, and right after
            // a drift resync snaps the ratio back). tail_/pos_ must still be
            // maintained so the next non-1.0 chunk interpolates against
            // fresh history instead of stale frames (that staleness clicked).
            out.assign(work_.begin() + 3 * oc, work_.end());
            saveTail();
            pos_ = 3.0;
            return;
        }

        // Interpolate at fractional work_ positions p, needing frames
        // i0-1 .. i0+2: valid while 1 <= p < n+1 given the 3-frame prefix.
        // The carry below keeps pos_ >= 1 across chunks by construction.
        double p = pos_;
        const double limit = n + 1.0;
        if (p < limit) {
            out.reserve(static_cast<size_t>((limit - p) / adjustedRatio_ + 2) * oc);
        }
        while (p < limit) {
            size_t i0 = static_cast<size_t>(p);
            float t = static_cast<float>(p - static_cast<double>(i0));
            const float* ym1 = &work_[(i0 - 1) * oc];
            const float* y0  = &work_[i0 * oc];
            const float* y1  = &work_[(i0 + 1) * oc];
            const float* y2  = &work_[(i0 + 2) * oc];
            for (int c = 0; c < outCh_; ++c) {
                out.push_back(hermite(ym1[c], y0[c], y1[c], y2[c], t));
            }
            p += adjustedRatio_;
        }
        // Next chunk's work_ drops this chunk's n oldest frames (the new
        // tail is the last 3), so the read position shifts by exactly n.
        pos_ = p - n;
        saveTail();
    }

private:
    // 4-point Catmull-Rom cubic: interpolates between y0 (t=0) and y1 (t=1)
    // using one frame of context on each side.
    static float hermite(float ym1, float y0, float y1, float y2, float t) {
        float c1 = 0.5f * (y1 - ym1);
        float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * t + c2) * t + c1) * t + y0;
    }

    void saveTail() {
        std::copy(work_.end() - 3 * static_cast<ptrdiff_t>(outCh_), work_.end(), tail_.begin());
    }

    int inCh_ = 2, inRate_ = 48000, outCh_ = 2, outRate_ = 48000;
    double ratio_ = 1.0;
    double adjustedRatio_ = 1.0;
    double pos_ = 3.0;
    bool hasTail_ = false;
    std::vector<float> tail_;  // last 3 frames of the previous chunk
    std::vector<float> work_;  // [tail][remapped chunk], one timeline
};
