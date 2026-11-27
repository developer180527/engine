#pragma once
// engine_player --conformance <kind> [--mutate] (WO-093). Only in players
// built with ENGINE_PLAYER_CONFORMANCE; see player_conformance.cpp.
#include <string>

// 0: every check passed; 1: a check failed; 2: no suite for that kind.
int runPlayerConformance(const std::string& kind, bool mutate);
