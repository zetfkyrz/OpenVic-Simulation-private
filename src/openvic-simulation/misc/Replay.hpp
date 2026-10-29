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
		// Layout of the save file: 1 = header without versions, 2 = adds the simulation version and the hash of the defines
		inline constexpr int FORMAT_VERSION = 2;
		// Raise it when a change of the rules of the simulation makes the old replays end in another state
		// 2: artisans get the whole revenue of their sales (the cost of the inputs was lost), see docs/BUGS_FIXED.md
		// 3: prices rise when more is wanted than offered even if the buyers are short of money (stuck prices)
		// 4: the revenue an artisan compares with its costs is yesterday's, not the sum over the whole game (artisans never changed type)
		// 5: a price rises only if the buyers can pay the whole supply, otherwise it falls (stuck prices, run-away scarce goods)
		// 6: pops start with 7 days of savings (POP_START_SAVINGS_DAYS), against the hunger of the first months
		// 7: the credit system is new (offers, delays, schedule), the old credit actions are skipped when a save is loaded
		// 13: the simulation no longer depends on the threads: a profitable artisan keeps its type (it was decided by an unset flag), the gold
		//     is paid after the market has run, see docs/BUGS_FIXED.md
		// 14: world trade (routes by land and sea with freight and capacity, the workers keep part of the output), the unemployed who are hungry
		//     for a month emigrate or die, see docs/TRADE.md
		inline constexpr int SIMULATION_VERSION = 18;

		// What the version and the defines hash of a save say about loading it on this build
		struct Compatibility {
			// False when the save cannot be loaded at all (a newer format): the reason is in blocked_reason
			bool loadable = true;
			std::string blocked_reason;
			// The save can be opened, but the result may differ from the saved game: shown with "open anyway"
			std::vector<std::string> warnings;
		};
		// sim_version < 0 / has_defines_hash == false when the save does not have them (format 1, migrated on loading)
		Compatibility check_header(
			int format, int sim_version, bool has_defines_hash, std::uint64_t defines_hash, std::uint64_t current_defines_hash
		);
		std::string hash_to_string(std::uint64_t hash);
		bool hash_from_string(std::string const& text, std::uint64_t& hash);

		// Whether the action changes the simulation and must be recorded (pause and speed only affect the clock)
		bool is_recorded(game_action_t const& action);
		// One line per action: "<batch> <name> <values...>"
		std::string serialise(std::vector<ReplayEntry> const& entries);
		// Returns false if a line is malformed or an action is unknown
		bool deserialise(std::string const& text, std::vector<ReplayEntry>& entries);
	}
}
