#include "check.h"
#include "Resampler.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(int frames, int channels, double freq, int rate) {
    std::vector<float> s(static_cast<size_t>(frames) * channels);
    for (int f = 0; f < frames; ++f) {
        float v = static_cast<float>(std::sin(2.0 * kPi * freq * f / rate));
        for (int c = 0; c < channels; ++c) s[static_cast<size_t>(f) * channels + c] = v;
    }
    return s;
}
} // namespace

TEST(passthrough_is_bit_exact) {
    StreamResampler r;
    r.configure(2, 48000, 2, 48000);
    auto in = sine(480, 2, 440.0, 48000);
    std::vector<float> out;
    r.process(in.data(), 480, 0, out);
    CHECK_EQ(out.size(), in.size());
    for (size_t i = 0; i < in.size(); ++i) CHECK(out[i] == in[i]);
}

TEST(channel_modes_pick_left_or_right) {
    StreamResampler r;
    r.configure(2, 48000, 2, 48000);
    std::vector<float> in = { 0.25f, -0.75f, 0.5f, -0.5f };
    std::vector<float> out;
    r.process(in.data(), 2, 1, out);
    CHECK_EQ(out.size(), size_t(4));
    CHECK_EQ(out[0], 0.25f); CHECK_EQ(out[1], 0.25f);
    CHECK_EQ(out[2], 0.5f);  CHECK_EQ(out[3], 0.5f);
    r.process(in.data(), 2, 2, out);
    CHECK_EQ(out[0], -0.75f); CHECK_EQ(out[1], -0.75f);
}

TEST(rate_conversion_produces_expected_length) {
    StreamResampler r;
    r.configure(2, 44100, 2, 48000);
    auto in = sine(4410, 2, 1000.0, 44100);
    std::vector<float> out;
    size_t total = 0;
    for (int chunk = 0; chunk < 10; ++chunk) {
        r.process(in.data() + chunk * 441 * 2, 441, 0, out);
        total += out.size() / 2;
    }
    // 4410 frames at 44.1k = 100ms = 4800 frames at 48k (minus the kernel's
    // 2-frame look-ahead that stays queued in the tail).
    CHECK(total >= 4795 && total <= 4800);
}

TEST(chunked_processing_matches_single_pass) {
    // The whole point of the carried tail: splitting the input into packets
    // must not change the output (that used to crackle every ~10ms).
    const int frames = 2400;
    auto in = sine(frames, 1, 997.0, 48000);
    StreamResampler whole, chunked;
    whole.configure(1, 48000, 1, 48000);
    chunked.configure(1, 48000, 1, 48000);
    whole.setSpeedAdjustment(1.001);
    chunked.setSpeedAdjustment(1.001);
    std::vector<float> a, b, tmp;
    whole.process(in.data(), frames, 0, a);
    for (int off = 0; off < frames; off += 160) {
        chunked.process(in.data() + off, 160, 0, tmp);
        b.insert(b.end(), tmp.begin(), tmp.end());
    }
    CHECK_EQ(a.size(), b.size());
    double maxDiff = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(a[i] - b[i])));
    }
    CHECK_NEAR(maxDiff, 0.0, 1e-6);
}

TEST(dc_stays_dc_through_interpolation) {
    StreamResampler r;
    r.configure(2, 48000, 2, 48000);
    r.setSpeedAdjustment(0.9985);
    std::vector<float> in(960, 0.3f);
    std::vector<float> out;
    r.process(in.data(), 480, 0, out);
    r.process(in.data(), 480, 0, out);
    for (float v : out) CHECK_NEAR(v, 0.3, 1e-5);
}

TEST(mono_to_stereo_and_back) {
    StreamResampler up, down;
    up.configure(1, 48000, 2, 48000);
    down.configure(2, 48000, 1, 48000);
    std::vector<float> mono = { 0.1f, 0.2f, 0.3f };
    std::vector<float> out;
    up.process(mono.data(), 3, 0, out);
    CHECK_EQ(out.size(), size_t(6));
    CHECK_EQ(out[2], 0.2f); CHECK_EQ(out[3], 0.2f);
    std::vector<float> stereo = { 0.2f, 0.4f, 1.0f, 0.0f };
    down.process(stereo.data(), 2, 0, out);
    CHECK_EQ(out.size(), size_t(2));
    CHECK_NEAR(out[0], 0.3, 1e-6);
    CHECK_NEAR(out[1], 0.5, 1e-6);
}

TEST_MAIN()
