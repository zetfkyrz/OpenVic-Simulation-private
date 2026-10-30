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

		// The density of a division (docs/WAR_ASSUMPTIONS.md, section 5): the raw density is ov_war.DENSITY_DEPTH_FACTOR x depth / width, with depth = regiments /
		// width (a width twice the depth is 1.0). The technologies change it: the raw density times density_coefficient_mult, then the part of it over 1.0 is
		// cut by density_excess_reduction, and a density that was over 1.0 does not fall under it (1.2 x 0.9 = 1.08 -> 1 + 0.08 x 0.85 = 1.068). A density of 1.0 or
		// less is not changed by the technologies.
		[[nodiscard]] fixed_point_t raw_density(OpenVicWarDefines const& defines, std::uint32_t regiments, std::uint32_t width);
		[[nodiscard]] fixed_point_t effective_density(fixed_point_t raw_density, fixed_point_t coefficient_mult, fixed_point_t excess_reduction);

		// What a density does (x = |1 - density|, penalty = DENSITY_LINEAR x + DENSITY_QUADRATIC x^2): below 1 the defence and the breakthrough are
		// 1 - penalty; above 1 the casualties and the breakthrough are 1 + penalty and the attack 1 + DENSITY_ATTACK_SHARE x penalty
		struct DensityEffect {
			fixed_point_t attack_mult = fixed_point_t::_1;
			fixed_point_t defence_mult = fixed_point_t::_1;
			fixed_point_t breakthrough_mult = fixed_point_t::_1;
			fixed_point_t casualties_mult = fixed_point_t::_1;
		};
		[[nodiscard]] DensityEffect density_effect(OpenVicWarDefines const& defines, fixed_point_t density);

		// The defence of a division in battle: with its fortification (each level adds FORTIFICATION_PER_LEVEL) and a bonus of the terrain or the
		// modifiers (a share, 0.1 is +10 %)
		[[nodiscard]] fixed_point_t effective_defence(OpenVicWarDefines const& defines, DivisionStats const& defender, fixed_point_t extra_bonus = fixed_point_t::_0);
		// R: the breakthrough of the attacker over the effective defence of the defender (a defence of 0 gives a very big R)
		[[nodiscard]] fixed_point_t breakthrough_ratio(
			OpenVicWarDefines const& defines, DivisionStats const& attacker, DivisionStats const& defender, fixed_point_t extra_defence_bonus = fixed_point_t::_0
		);
		// One day of the battle for the breakthrough: R of BREAK_RATIO_DAY at once, or R of BREAK_RATIO_WEEK for BREAK_WEEK_DAYS days in a row, makes the defence
		// retreat (true; the counter starts again); the counter goes back to 0 on a day with R under BREAK_RATIO_WEEK
		struct BreakthroughCounter {
			std::int32_t days = 0;
		};
		[[nodiscard]] bool breakthrough_day(OpenVicWarDefines const& defines, fixed_point_t ratio, BreakthroughCounter& counter);

		// What the weapon is worth to the attack, the defence or the breakthrough of a regiment: the multiplier of its weapon; with an equipment fill
		// (0..1) the regiment has the best weapon in this share and the one it was made with in the rest
		[[nodiscard]] fixed_point_t weapon_ratio(fixed_point_t first_points, fixed_point_t best_points, fixed_point_t fill);

		// Durability points a division loses in a day of battle against another one (the damage is in proportion to the attack, so to the width of the
		// front: a division half as wide deals half the damage at the same depth; the defence of the one who gets it softens it and the casualties of its density change it)
		// target_is_attacker: the one who takes the damage is the one who attacks, its defence softens the damage by ATTACKER_DEFENSE_FACTOR only
		[[nodiscard]] fixed_point_t damage_per_day(
			OpenVicWarDefines const& defines, DivisionStats const& attacker, DivisionStats const& defender, bool target_is_attacker = false
		);
		// The days a division with the durability lasts under the damage per day (0 damage: never, the result is 0)
		[[nodiscard]] fixed_point_t days_to_destroy(fixed_point_t durability, fixed_point_t damage_per_day);
	}
}
