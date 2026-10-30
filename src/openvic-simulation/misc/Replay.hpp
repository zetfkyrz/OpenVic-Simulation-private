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
		// 19: fronts spread the divisions of their armies over the line (an army of its own for every province of the line)
		// 20: unlocks of the country (equipment:/formation:/support_company: keys and the modifiers they give, in the state hash), the provisional
		//     layer of the year, the limits of the division templates (ov_war.TEMPLATE_*)
		// 21: the division template is a grid (ranks x rows of regiment types, slots of companies), the standard template is 4 infantry 2x2 and 2 cavalry
		//     1x2, a regiment fills a cell of its own type; create/update_division_template carry the grid
		// 22: the weapons are goods (rifle_1..4, field_gun_1..4; small_arms and artillery are rifle_1 and field_gun_1), ov_data/common/ov_equipment
		// 23: a factory of weapons makes the generation that is chosen (set_factory_production_type), the recipes of ov_data/common/ov_production_types, the AI
		//     takes the newest generation the country has
		// 24: regiments have a weapon of a generation (taken out of the national stockpile, bought by the government), use its goods as supplies and fight with
		//     its attack and defence
		// 25: tier 0 of the weapons (rifle_0, field_gun_0), the weapons are multipliers of the attack, the defence and the breakthrough, default unlocks
		// 26: small_arms and artillery are rifle_0 and field_gun_0 (they were rifle_1 and field_gun_1), recipes of the first generation
		// 27: unit types of OpenVic in files (ov_units; infantry, cavalry, artillery of Vic2 are line_infantry, cavalry, napoleonic_artillery), unit: keys, the weapon by the
		//     list of the type with the penalty below the lowest allowed one
		inline constexpr int SIMULATION_VERSION = 27;

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
