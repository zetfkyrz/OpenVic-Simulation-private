#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "openvic-simulation/core/memory/String.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/dataloader/NodeTools.hpp"

namespace OpenVic {
	struct OpenVicWarDefines;
	struct DivisionStats;
	struct UnitTypeManager;

	// The conditions of the attacker in a battle (ov_data/common/ov_war/battle_conditions.txt, docs/WAR_ASSUMPTIONS.md, section 6): multipliers of the
	// damage the attacking side deals. A terrain that is not listed is 1; the game has no night and no weather yet, their multipliers are kept for them
	struct BattleConditions {
		memory::vector<std::pair<memory::string, fixed_point_t>> terrain; // the id of a terrain type of Vic2 -> the multiplier
		fixed_point_t river_crossing = fixed_point_t::_1; // the attacker came over a river (the border of the two provinces is along a river)
		fixed_point_t night = fixed_point_t::_1;
		fixed_point_t weather = fixed_point_t::_1;

		bool load(std::string_view source, ast::NodeCPtr root);
		[[nodiscard]] fixed_point_t get_terrain(std::string_view terrain_identifier) const;
	};

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

		// The protection of a defending division: its defence x (1 + FORTIFICATION_PER_LEVEL x the levels of its fortification) x the multiplier of the
		// terrain (1 + TERRAIN_DEFENCE_PER_POINT x the defence of the terrain of Vic2). The levels are the fortification of the division (the modifiers and
		// the companies) and the dig in of its army
		[[nodiscard]] fixed_point_t defender_protection(
			OpenVicWarDefines const& defines, fixed_point_t defence, fixed_point_t fortification_levels, fixed_point_t terrain_multiplier = fixed_point_t::_1
		);
		[[nodiscard]] fixed_point_t effective_defence(OpenVicWarDefines const& defines, DivisionStats const& defender, fixed_point_t terrain_multiplier = fixed_point_t::_1);
		// R: the breakthrough of the attacker over the effective defence of the defender (a defence of 0 gives a very big R)
		[[nodiscard]] fixed_point_t breakthrough_ratio(fixed_point_t attacker_breakthrough, fixed_point_t defender_protection);
		[[nodiscard]] fixed_point_t breakthrough_ratio(
			OpenVicWarDefines const& defines, DivisionStats const& attacker, DivisionStats const& defender, fixed_point_t terrain_multiplier = fixed_point_t::_1
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

		// The occupation of a province (docs/WAR_ASSUMPTIONS.md, section 5): the power of a division is the sum of the occupation weights of its regiments over
		// OCCUPATION_REFERENCE_REGIMENTS (six regiments of weight 1 are 6/6; artillery weighs 0.5; the companies do not count). The speed is the power times
		// (1 + the sum of occupation_speed_pct of the country and of the general) times the product of occupation_speed_mult; the days are the base days over the
		// speed. The key mechanic:instant_occupation (instant = true) overrides all: 0 days.
		[[nodiscard]] fixed_point_t occupation_power(OpenVicWarDefines const& defines, fixed_point_t weight_sum);
		[[nodiscard]] fixed_point_t occupation_speed(fixed_point_t power, fixed_point_t speed_pct_sum, fixed_point_t speed_mult_product);
		[[nodiscard]] fixed_point_t occupation_days(fixed_point_t base_days, fixed_point_t speed, bool instant);

		// The damage of a day of battle to one division (docs/WAR_ASSUMPTIONS.md, section 6; the formula of the owner, every coefficient in ov_war):
		//   damage = attack x conditions x density x softening, softening = 1 - BATTLE_BLOCK_SHARE x min(1, protection / attack)
		//   the men it loses = damage x BATTLE_STRENGTH_PER_HIT x casualties_taken (the modifiers casualties_taken_mult: medicine), in strength points
		//   the organisation it loses = damage x BATTLE_ORGANISATION_PER_HIT (points of every regiment)
		// attack: the part of the attack of the enemy side directed at this division (in proportion to its width); conditions: the terrain, the river,
		// the night and the weather of the attacker (1 for the damage the defender deals); density: the density of the division that takes the damage;
		// protection: the defence of a defender (defender_protection), the breakthrough of an attacker. The organisation does not change the damage.
		struct Hit {
			fixed_point_t damage;
			fixed_point_t strength_loss;
			fixed_point_t organisation_loss;
		};
		[[nodiscard]] Hit battle_hit(
			OpenVicWarDefines const& defines, fixed_point_t attack, fixed_point_t conditions, fixed_point_t density, fixed_point_t protection,
			fixed_point_t casualties_taken
		);
	}
}
