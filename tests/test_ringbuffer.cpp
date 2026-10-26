#include "check.h"
#include "RingBuffer.h"

#include <numeric>

TEST(capacity_rounds_up_to_power_of_two) {
    RingBuffer r(1000);
    CHECK_EQ(r.capacity(), size_t(1024));
    CHECK_EQ(r.availableToRead(), size_t(0));
    CHECK_EQ(r.availableToWrite(), size_t(1024));
}

TEST(roundtrip_across_wrap) {
    RingBuffer r(8);
    std::vector<float> in(6);
    std::iota(in.begin(), in.end(), 1.0f);
    CHECK_EQ(r.write(in.data(), 6), size_t(6));
    std::vector<float> out(4);
    CHECK_EQ(r.read(out.data(), 4), size_t(4));
    CHECK_EQ(out[0], 1.0f);
    CHECK_EQ(out[3], 4.0f);
    // 2 left, 6 free: write 5 more -> wraps around the end of the storage.
    std::vector<float> in2 = { 10, 11, 12, 13, 14 };
    CHECK_EQ(r.write(in2.data(), 5), size_t(5));
    CHECK_EQ(r.availableToRead(), size_t(7));
    std::vector<float> out2(7);
    CHECK_EQ(r.read(out2.data(), 7), size_t(7));
    CHECK_EQ(out2[0], 5.0f);
    CHECK_EQ(out2[1], 6.0f);
    CHECK_EQ(out2[2], 10.0f);
    CHECK_EQ(out2[6], 14.0f);
}

TEST(short_write_is_truncated_to_free_space) {
    RingBuffer r(8);
    std::vector<float> in(10, 1.0f);
    CHECK_EQ(r.write(in.data(), 10), size_t(8));
    CHECK_EQ(r.write(in.data(), 10), size_t(0));
}

TEST(aligned_write_never_splits_a_frame) {
    // Stereo frames = 2 samples. Fill so that 3 samples are free: a plain
    // write would accept 3 (splitting a frame and swapping L/R for every
    // frame read afterwards); the aligned write must take only 2.
    RingBuffer r(8);
    std::vector<float> fill(5, 0.0f);
    r.write(fill.data(), 5);
    CHECK_EQ(r.availableToWrite(), size_t(3));
    std::vector<float> stereo = { 1, 2, 3, 4, 5, 6 };
    CHECK_EQ(r.writeAligned(stereo.data(), 6, 2), size_t(2));
    CHECK_EQ(r.availableToRead(), size_t(7)); // 5 pre-filled + 2
    r.drop(5);
    std::vector<float> out(2);
    CHECK_EQ(r.read(out.data(), 2), size_t(2));
    CHECK_EQ(out[0], 1.0f);
    CHECK_EQ(out[1], 2.0f);
}

TEST(aligned_silence_keeps_frame_alignment) {
    RingBuffer r(8);
    std::vector<float> fill(3, 0.5f);
    r.write(fill.data(), 3);
    CHECK_EQ(r.writeSilenceAligned(100, 2), size_t(4)); // 5 free -> 4
    CHECK_EQ(r.availableToRead(), size_t(7));
}

TEST(drop_discards_oldest) {
    RingBuffer r(8);
    std::vector<float> in = { 1, 2, 3, 4 };
    r.write(in.data(), 4);
    CHECK_EQ(r.drop(2), size_t(2));
    std::vector<float> out(2);
    r.read(out.data(), 2);
    CHECK_EQ(out[0], 3.0f);
    CHECK_EQ(r.drop(10), size_t(0));
}

TEST_MAIN()
