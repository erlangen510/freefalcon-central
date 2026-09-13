#pragma once
#include <filesystem>
#include <chrono>

// Local observer transport. Only the campaign thread reads model state.
class CampaignWatch {
public:
    explicit CampaignWatch(const std::filesystem::path& directory);
    void initialize(const char* scenario);
    bool waitForStep();
    void publish(const char* status = "ready");
    void error(const char* message);
private:
    using Clock = std::chrono::steady_clock;
    std::filesystem::path directory;
    Clock::time_point last = Clock::now(), sent = last, heartbeat = last;
    double credit = 0;
    unsigned long long sequence = 0, requestedSteps = 0, consumedSteps = 0, frame = 0;
    int speed = 1;
    bool paused = true, stopping = false;
    void command();
    void write(const char* name, const std::string& contents);
};
