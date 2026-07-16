#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Lifecycle of one output inside a mic calibration pass.
enum class CalState { Pending, Measuring, Done, Failed };

struct CalOutcome {
    std::wstring deviceId;
    CalState state = CalState::Pending;
    int delayMs = 0; // measured end-to-end delay, valid once state == Done
};

// Measures each output's true end-to-end latency -- WASAPI buffer + driver +
// Bluetooth link + acoustic flight to the mic -- by playing a short sine
// sweep on it while recording the chosen microphone, then locating the sweep
// in the recording by cross-correlation. This captures what the engine's own
// IAudioClock-based estimate (deviceLagMs, the "Mode calage" alignment)
// structurally cannot: latency the driver never reports (common on
// Bluetooth) and the speakers' distance to the listening position.
//
// Runs on its own worker thread; the GUI polls snapshot() on a timer and
// applies the relative delays when isRunning() drops back to false. The
// engine must be stopped for the duration: the sweep has to be the only
// thing playing.
class Calibrator {
public:
    ~Calibrator();

    // Starts a pass measuring outputIds in order. False if one is running.
    bool start(const std::wstring& micId, const std::vector<std::wstring>& outputIds);
    void cancel(); // async: signals the worker and returns immediately
    bool isRunning() const { return running_.load(); }

    struct Snapshot {
        bool running = false;
        bool cancelled = false;
        std::wstring error; // non-empty = the whole pass failed (e.g. mic)
        std::vector<CalOutcome> outcomes;
    };
    Snapshot snapshot();

private:
    void worker(std::wstring micId);
    void setState(size_t idx, CalState st, int delayMs = 0);
    void fail(const std::wstring& error);

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};

    std::mutex mutex_;
    std::wstring error_;
    std::vector<CalOutcome> outcomes_;
};
