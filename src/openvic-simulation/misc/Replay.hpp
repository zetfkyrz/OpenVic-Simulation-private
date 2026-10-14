#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "openvic-simulation/misc/GameAction.hpp"

namespace OpenVic {
	// Save games are replays: the simulation is deterministic, so the list of the executed game actions (grouped by the
	// batch that executed them) recreates the exact state when it is replayed on a freshly loaded bookmark.
	struct ReplayEntry {
		std::uint32_t batch;
		game_action_t action;
	};

	namespace Replay {
		// Whether the action changes the simulation and must be recorded (pause and speed only affect the clock)
		bool is_recorded(game_action_t const& action);
		// One line per action: "<batch> <name> <values...>"
		std::string serialise(std::vector<ReplayEntry> const& entries);
		// Returns false if a line is malformed or an action is unknown
		bool deserialise(std::string const& text, std::vector<ReplayEntry>& entries);
	}
}
