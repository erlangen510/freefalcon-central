#pragma once
#include <filesystem>
#include <chrono>

// Local observer transport. Only the campaign thread reads model state.
class CampaignWatch {
public:
    explicit CampaignWatch(const std::filesystem::path& directory);
    void initialize(const char* scenario);
    bool waitForStep();
    unsigned stepMilliseconds() const { return nextStepMs; }
    void publish(const char* status = "ready");
    void error(const char* message);
private:
    using Clock = std::chrono::steady_clock;
    std::filesystem::path directory;
    Clock::time_point last = Clock::now(), sent = last, heartbeat = last;
    double credit = 0;
    unsigned remainingStepMs = 0, nextStepMs = 5000;
    unsigned long long sequence = 0, requestedSteps = 0, consumedSteps = 0, frame = 0;
    unsigned long long actionSequence = 0;
    std::string actionStatus = "idle";
    int speed = 1;
    bool paused = true, stopping = false;
    bool regionActive = false;
    double regionX = 0, regionY = 0, regionRadius = 10;
    void command();
    void unitAction();
    void write(const char* name, const std::string& contents);
};
