#pragma once

#include <vector>
#include <cstddef>
#include <algorithm>

// Streaming converter for interleaved float audio: channel remap + linear-
// interpolated sample-rate conversion. Interpolation state (fractional
// position + last frame) is carried across chunks so packet boundaries do
// not introduce waveform discontinuities — processing each packet in
// isolation produced an audible crackle every ~10 ms when rates differed.
class StreamResampler {
public:
    void configure(int inChannels, int inRate, int outChannels, int outRate) {
        inCh_ = inChannels;
        inRate_ = inRate;
        outCh_ = outChannels;
        outRate_ = outRate;
        ratio_ = static_cast<double>(inRate) / static_cast<double>(outRate);
        adjustedRatio_ = ratio_;
        pos_ = 0.0;
        hasLast_ = false;
        lastFrame_.assign(outCh_, 0.0f);
    }

    void setSpeedAdjustment(double adj) {
        adjustedRatio_ = ratio_ * adj;
    }

    // channelMode: 0 = stereo passthrough, 1 = left channel only (speaker is
    // the left half of a stereo pair), 2 = right channel only.
    void process(const float* in, size_t inFrames, int channelMode, std::vector<float>& out) {
        out.clear();
        if (inFrames == 0) return;

        remapped_.resize(inFrames * outCh_);
        for (size_t f = 0; f < inFrames; ++f) {
            const float* src = in + f * inCh_;
            float* dst = &remapped_[f * outCh_];
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

        if (inRate_ == outRate_ && adjustedRatio_ == 1.0) {
            out = remapped_;
            // Must still advance the continuity state even on this shortcut:
            // adjustedRatio_ flips back to 1.0 after every drift-correction
            // resync, so skipping this left lastFrame_/pos_ stale from
            // whenever the ratio last wasn't 1.0. The next time it drifts
            // away from 1.0 again, interpolation would blend against that
            // stale frame and click.
            if (!remapped_.empty()) {
                std::copy(remapped_.end() - outCh_, remapped_.end(), lastFrame_.begin());
                hasLast_ = true;
            }
            pos_ = 0.0;
            return;
        }

        // Continuous source timeline: W[0] = last frame of the previous
        // chunk, W[k] = remapped[k-1] for k in 1..n. pos_ is the fractional
        // read position relative to W[0].
        if (!hasLast_) {
            std::copy(remapped_.begin(), remapped_.begin() + outCh_, lastFrame_.begin());
            hasLast_ = true;
        }
        const double n = static_cast<double>(inFrames);
        double p = pos_;
        out.reserve(static_cast<size_t>(n / adjustedRatio_ + 2) * outCh_);
        while (p < n) {
            size_t i0 = static_cast<size_t>(p);
            float frac = static_cast<float>(p - static_cast<double>(i0));
            const float* a = (i0 == 0) ? lastFrame_.data() : &remapped_[(i0 - 1) * outCh_];
            const float* b = &remapped_[i0 * outCh_];
            for (int c = 0; c < outCh_; ++c) {
                out.push_back(a[c] + (b[c] - a[c]) * frac);
            }
            p += adjustedRatio_;
        }
        pos_ = p - n;
        std::copy(remapped_.end() - outCh_, remapped_.end(), lastFrame_.begin());
    }

private:
    int inCh_ = 2, inRate_ = 48000, outCh_ = 2, outRate_ = 48000;
    double ratio_ = 1.0;
    double adjustedRatio_ = 1.0;
    double pos_ = 0.0;
    bool hasLast_ = false;
    std::vector<float> lastFrame_;
    std::vector<float> remapped_;
};
