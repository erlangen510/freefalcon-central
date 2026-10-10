#pragma once
#include <filesystem>
#include <chrono>
void SaveAggregateCheckpoint(const std::filesystem::path& directory);

// Local observer transport. Only the campaign thread reads model state.
// Protocol version of catalog.json/state.json and the command files (migration ADR-0001 section 6).
constexpr int kWatchProtocolVersion = 1;
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
    // Steps requested while running are absorbed by the running clock rather than queued.
    unsigned long long coalescedSteps = 0;
    // Increments whenever simulated time advances or a command changes host state.
    unsigned long long stateVersion = 0;
    // Latest rejected command per channel. A rejection never consumes its sequence number.
    unsigned long long controlRejected = 0, actionRejected = 0;
    std::string controlRejectReason, actionRejectReason;
    unsigned long long actionSequence = 0;
    std::string actionStatus = "idle";
    unsigned long long saveSequence=0;
    std::string saveStatus="idle",saveError,savePath;
    unsigned long long commanderSequence=0;
    std::string commanderStatus="idle";
    int speed = 1;
    bool paused = true, stopping = false;
    bool regionActive = false;
    double regionX = 0, regionY = 0, regionRadius = 10;
    void command();
    void unitAction();
    void saveCommand();
    void commanderCommand();
    void write(const char* name, const std::string& contents);
};
