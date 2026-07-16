#pragma once

#include <cmath>
#include <cstdlib>
#include <vector>

// Biquad filter (Audio EQ Cookbook) over interleaved float audio, with
// independent state per channel. One instance per band per output, owned by
// that output's render thread.
class BiquadFilter {
public:
    // BandPass is only used for analysis/metering (see AudioEngine's per-band
    // level meter) -- constant 0dB peak gain, dBgain is ignored for it.
    enum class Type { LowShelf, HighShelf, Peaking, BandPass };

    void configure(int channels, int sampleRate, Type type, double f0, double dBgain) {
        // Only clear the filter's memory when the channel layout actually
        // changes (checked via the state vectors' own size, since ch_'s
        // default of 2 would otherwise mask the very first call for a
        // stereo output and leave x1_/x2_/y1_/y2_ empty). Wiping it on every
        // dB tweak -- this is called on every EQ slider move -- forced a
        // cold-start transient into a signal that was flowing continuously,
        // an audible click on every adjustment.
        if (channels != static_cast<int>(x1_.size())) {
            ch_ = channels;
            x1_.assign(ch_, 0.0f);
            x2_.assign(ch_, 0.0f);
            y1_.assign(ch_, 0.0f);
            y2_.assign(ch_, 0.0f);
        }
        // BandPass is never a no-op passthrough -- it's the whole point of
        // using it (frequency-selective metering), unlike a 0dB shelf/peak.
        bypass_ = type != Type::BandPass && std::abs(dBgain) < 0.01;
        if (bypass_) return;

        const double pi = 3.14159265358979323846;
        double A = std::pow(10.0, dBgain / 40.0);
        double w0 = 2.0 * pi * f0 / sampleRate;
        double cw = std::cos(w0), sw = std::sin(w0);
        double a0;
        if (type == Type::Peaking) {
            // Q ~1.0: roughly one octave wide, a reasonable spacing for a
            // 5-band graphic EQ without adjacent bands fighting each other.
            constexpr double Q = 1.0;
            double alpha = sw / (2.0 * Q);
            b0_ = 1.0 + alpha * A;
            b1_ = -2.0 * cw;
            b2_ = 1.0 - alpha * A;
            a0  = 1.0 + alpha / A;
            a1_ = -2.0 * cw;
            a2_ = 1.0 - alpha / A;
        } else if (type == Type::BandPass) {
            // RBJ cookbook BPF, constant 0dB peak gain, same Q as the peaking
            // bands above so the analysis bands line up with the EQ's own.
            constexpr double Q = 1.0;
            double alpha = sw / (2.0 * Q);
            b0_ = alpha;
            b1_ = 0.0;
            b2_ = -alpha;
            a0  = 1.0 + alpha;
            a1_ = -2.0 * cw;
            a2_ = 1.0 - alpha;
        } else {
            double S = 0.9; // gentle slope
            double alpha = sw / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
            double k = 2.0 * std::sqrt(A) * alpha;
            if (type == Type::LowShelf) {
                b0_ =       A * ((A + 1) - (A - 1) * cw + k);
                b1_ = 2.0 * A * ((A - 1) - (A + 1) * cw);
                b2_ =       A * ((A + 1) - (A - 1) * cw - k);
                a0  =            (A + 1) + (A - 1) * cw + k;
                a1_ =     -2.0 * ((A - 1) + (A + 1) * cw);
                a2_ =            (A + 1) + (A - 1) * cw - k;
            } else { // HighShelf
                b0_ =        A * ((A + 1) + (A - 1) * cw + k);
                b1_ = -2.0 * A * ((A - 1) + (A + 1) * cw);
                b2_ =        A * ((A + 1) + (A - 1) * cw - k);
                a0  =             (A + 1) - (A - 1) * cw + k;
                a1_ =       2.0 * ((A - 1) - (A + 1) * cw);
                a2_ =             (A + 1) - (A - 1) * cw - k;
            }
        }
        b0_ /= a0; b1_ /= a0; b2_ /= a0; a1_ /= a0; a2_ /= a0;
    }

    void process(float* data, size_t frames) {
        if (bypass_) return;
        for (size_t f = 0; f < frames; ++f) {
            for (int c = 0; c < ch_; ++c) {
                float x = data[f * ch_ + c];
                float y = static_cast<float>(b0_ * x + b1_ * x1_[c] + b2_ * x2_[c]
                                             - a1_ * y1_[c] - a2_ * y2_[c]);
                x2_[c] = x1_[c];
                x1_[c] = x;
                y2_[c] = y1_[c];
                y1_[c] = y;
                data[f * ch_ + c] = y;
            }
        }
    }

private:
    int ch_ = 2;
    bool bypass_ = true;
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    std::vector<float> x1_, x2_, y1_, y2_;
};
