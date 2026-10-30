#pragma once

#include <cstddef>
#include <cstdint>

#include "openvic-simulation/core/object/FixedPoint.hpp"

namespace OpenVic {
	struct OpenVicWarDefines;
	struct DivisionStats;

	// ALL the battle mathematics of the divisions is in this file and in BattleFormulas.cpp, nothing else computes damage. The formulas are
	// simple on purpose (the rules of the owner: the weights of the rows, the density, the damage in proportion to the width, the depth as the
	// durability) and can be replaced without touching the rest; every coefficient is in ov_war (common/openvic_defines.lua).
	// The more detailed rules (the window of superiority, suppression, trenches, breakthrough, the days of occupation) are written by the owner here.
	namespace battle_formulas {
		// The weight of a row of the grid in the attack and in the defence of a division: the first row fires with all it has, every next one with half
		// of the one before (1, 1/2, 1/4, 1/8); with row_weight_shift = s (a formation of the technologies) the first s+1 rows weigh the same:
		// s = 1 gives 1, 1, 1/2, 1/4
		[[nodiscard]] fixed_point_t row_weight(std::size_t row, std::int32_t row_weight_shift);

		// The density of the formation: the strength points of the regiments per rank of the front, less by density_reduction (0..1) of the technologies
		[[nodiscard]] fixed_point_t density(fixed_point_t strength_sum, std::uint32_t width, fixed_point_t density_reduction);

		// What the weapon is worth to the attack, the defence or the breakthrough of a regiment: the multiplier of its weapon; with an equipment fill
		// (0..1) the regiment has the best weapon in this share and the one it was made with in the rest
		[[nodiscard]] fixed_point_t weapon_ratio(fixed_point_t first_points, fixed_point_t best_points, fixed_point_t fill);

		// Durability points a division loses in a day of battle against another one (the damage is in proportion to the attack, so to the width of the
		// front: a division half as wide deals half the damage at the same depth; the defence of the one who gets it and its density change it)
		[[nodiscard]] fixed_point_t damage_per_day(
			OpenVicWarDefines const& defines, DivisionStats const& attacker, DivisionStats const& defender
		);
		// The days a division with the durability lasts under the damage per day (0 damage: never, the result is 0)
		[[nodiscard]] fixed_point_t days_to_destroy(fixed_point_t durability, fixed_point_t damage_per_day);
	}
}
