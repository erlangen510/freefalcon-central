#pragma once
#include <filesystem>
#include <string>

// Native commander (ATM/GTM team AI) observation and priority commands.
// Call only from the campaign thread.
std::string CommanderSnapshotJson();
// "priority <team> <creator> <num> <0..100>" or "reset <team> <creator> <num>".
// Returns a status word for the observer acknowledgement.
std::string ApplyCommanderCommand(const std::string& command);
void SaveCommanderCheckpoint(const std::filesystem::path& directory);
void LoadCommanderCheckpoint(const std::filesystem::path& directory);
