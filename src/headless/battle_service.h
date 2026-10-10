#pragma once
#include <string>
#include <vector>
// Phase 2 experiment: one detailed battle from a loaded checkpoint, with a result diff and an
// isolation audit. args: battle <data-root> <checkpoint-dir> <x|auto> <y> <radius-km> <seconds>
// <frozen|live> <out-dir> [seed]; the checkpoint must already be loaded.
void RunBattleService(const std::vector<std::string>& args);
