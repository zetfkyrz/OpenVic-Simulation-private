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
		// 28: research of the army technologies (ov_data/common/ov_technology): one slot per branch, start technologies of the civilised, the AI researching, the provisional
		//     unlocks of the year are off by default
		// 29: buildings and mechanics by keys (requires_unlock in ov_buildings, mechanic:division_templates / mobilisation), the signals line (telegraph, radio), the AI
		//     researches the first available technology of the files
		// 30: the supply of an army does not go through the provinces that an enemy controls (one and two provinces between), support companies in the templates,
		//     the upgrade of a regiment, the density and the occupation by the new formulas (the statistics of the templates only)
		// 31: the commanders: a group holds at most ov_war.MARSHAL_MAX_ATTACHED (8) armies, the switch of the orders of a group (set_army_group_orders)
		// 32: the natural growth of the population from ov_data/common/ov_population/growth.txt (by country, culture group, culture, year), the medicine and the
		//     sanitation multiply it; the population change of the month in the top bar
		// 38: every country fights with divisions: the default template of ov_data/common/ov_units/default_template.txt lays out the regiments of the
		//     history, generals, field marshals and fronts need mechanic:command (standardized_army), templates mechanic:division_templates; the AI without
		//     mechanic:command gives its divisions orders by hand
		// 39: the land battles of the divisions (military/BattleFormulas.cpp battle_hit, docs/WAR_ASSUMPTIONS.md, section 6): the live divisions, the width of
		//     the battle and the reserve, the losses from the first line, the retreat by the organisation or R, the conditions of the attacker (the river,
		//     ov_data/common/ov_war/battle_conditions.txt); the battles are in the state hash
		// 40: the lines of the field marshals (docs/WAR_ASSUMPTIONS.md, section 6, P2): a front is the pieces of the border (or of the coast) with the
		//     countries of its sector, the lines of the plans (offensive, defence, naval invasion), the modes of the execution (aggression, integrity);
		//     the old plan of one front (set_army_group_plan) and toggle_front_province are gone
		// 41: the fronts of the field marshals follow the border (docs/WAR_ASSUMPTIONS.md, section 6, P3): made again when a province changes its controller
		//     or a war its sides, the allies of the enemy in the war are in the sector, an edited front keeps its pieces, a front without a border stays empty
		// 42: the divisions of a field marshal go to his order by themselves (the front, the line of defence, the port of the landing), the execution lets
		//     them attack; a division split from the army of a general stays in the group under him (parent_army_id); a front on a part of the border
		// 43: the divisions of a field marshal are spread along the line of his order by the weights of its provinces (P4: the minimum and the enemy divisions
		//     next to it, the integrity of the front), a line of defence is a chain of neighbouring provinces (extend_plan_line)
		// 44: the executor of the offensives of the field marshals (P5): the targets next to the line, best first, attacked by the free divisions next to
		//     them by the estimate of the battle and the mode (careful R >= 2 with the victory, balanced R >= 1.5, aggressive always), the minimum holds
		// 45: the chance of the victory of a battle in its estimate (BATTLE_WIN_STEEPNESS), the modes of the offensive by it (80 %, 65 %, 50 %)
		// 46: the bonus of the planning of the field marshals (P6): it grows while an order waits, falls while it is executed, multiplies the attack and the
		//     breakthrough; the modifiers planning_max_add and planning_speed_mult; a manual order takes an army off every front and off the bonus
		// 47: the breakthrough of the field marshals (P7): a way of neighbouring provinces into the enemy land for the strongest third of the divisions, at the
		//     aggressive threshold; a new offensive no longer deletes the other lines of the front
		// 48: the encirclements (P8): the land reached from the capital and the ports with a naval base over the land of the friends, the armies cut off get
		//     no supply and no new men, lose organisation and give themselves up; the executor keeps the flank and closes the pockets
		// 49: the fallback of the divisions of the field marshals (P9): worn out on the line next to the enemy they fall back behind it and recover before they
		//     go back (FALLBACK_ORG_THRESHOLD, FALLBACK_RETURN_ORG)
		// 50: the naval invasion of the field marshals (P10): the divisions gather in the port, after NAVAL_PLAN_PREP_DAYS of the preparation they board the
		//     fleets there, sail to the sea of the target and land with less of the penalty (NAVAL_PLAN_PENALTY_REDUCTION)
		// 51: the plans of the field marshals for the AI countries (P11), behind ov_war.PLAN_AI_ENABLED (0: off, the AI as before)
		// 52: an army cut off from the supply far from the enemy (a colony) gets no supply and no new men but keeps its organisation; only a pocket next to
		//     the land of an enemy at war (or deep in it) takes the organisation and makes it give itself up
		// 53: the experience of the leaders: a general learns the attack, the defence and the terrains in battles (5 levels of 5 %), a field marshal the
		//     supply and the planning in wars (3 levels of 5 %); a new level gives the country prestige; the armies cross the straits (not past an enemy
		//     navy in the strait), an army sent by hand stays off the orders of its marshal until the player gives it back, a division with no way to the
		//     line does not take a place of it
		// 54: the recruitment of a division by a template (the manpower of the pops of soldiers, 3000 men a regiment), the state of the executed plan
		//     (the war with the countries of the front, the targets, the divisions, the chance) checked every day and when the execution is pressed
		// 55: an AI country at war recruits whole divisions by its first template (ov_ai.AI_DIVISIONS_PER_MONTH), single regiments when it cannot
		// 56: the way by sea of an army with no way over the land (through the ports of its country, P4d)
		// 57: the stockpile of the start: START_SUPPLY_STOCKPILE_DAYS of the supply of the units of every country
		// 58: the manpower in men (the pops of soldiers, +10 % at war, 3000 men a regiment; the armies of the start fitted to it), the losses die (the
		//     pops of soldiers shrink), the new men of the regiments cost the free manpower and the goods of the regiments, an army over the manpower
		//     fights weaker, the AI disbands down to it at peace; the attrition of an overloaded province; the attacker loses more, the attack from
		//     several provinces, worn out divisions go to the reserve, the terrain for the attacker
		// 59: a regiment uses every good of its construction cost as supply, the food and the drink 100 % a month (ARMY_SUPPLY_CONSUMABLE_SHARE), the
		//     maximum of the manpower is 10 % bigger (MANPOWER_BASE_BONUS)
		// 60: the regiments of the start (the types of Vic2, line infantry, the first artillery) eat grain instead of canned food, the canned food the
		//     stockpile does not have is replaced by twice as much grain (CANNED_FOOD_GRAIN_SUBSTITUTE); a division at sea is off the line of its front
		inline constexpr int SIMULATION_VERSION = 60;

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
