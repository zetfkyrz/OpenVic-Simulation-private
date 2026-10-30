#include "BattleFormulas.hpp"

#include <algorithm>

#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/military/DivisionStats.hpp"

using namespace OpenVic;

fixed_point_t battle_formulas::row_weight(const std::size_t row, const std::int32_t row_weight_shift) {
	const std::size_t shift = row_weight_shift > 0 ? static_cast<std::size_t>(row_weight_shift) : 0;
	const std::size_t halvings = row > shift ? row - shift : 0;
	// 1, 1/2, 1/4, ...; a row after 8 halvings weighs nothing a fixed point number can show
	return halvings >= 12 ? fixed_point_t::_0 : fixed_point_t::_1 / static_cast<std::int32_t>(1 << halvings);
}

fixed_point_t battle_formulas::raw_density(OpenVicWarDefines const& defines, const std::uint32_t regiments, const std::uint32_t width) {
	if (width == 0) {
		return fixed_point_t::_0;
	}
	const fixed_point_t depth = fixed_point_t(static_cast<std::int32_t>(regiments)) / static_cast<std::int32_t>(width);
	return defines.density_depth_factor * depth / static_cast<std::int32_t>(width);
}

fixed_point_t battle_formulas::effective_density(
	const fixed_point_t raw_density, const fixed_point_t coefficient_mult, const fixed_point_t excess_reduction
) {
	if (raw_density <= fixed_point_t::_1) {
		return raw_density;
	}
	const fixed_point_t reduction = std::clamp(excess_reduction, fixed_point_t::_0, fixed_point_t::_1);
	const fixed_point_t scaled = raw_density * coefficient_mult;
	if (scaled <= fixed_point_t::_1) {
		return fixed_point_t::_1;
	}
	return fixed_point_t::_1 + (scaled - fixed_point_t::_1) * (fixed_point_t::_1 - reduction);
}

battle_formulas::DensityEffect battle_formulas::density_effect(OpenVicWarDefines const& defines, const fixed_point_t density) {
	DensityEffect effect;
	const fixed_point_t x = density >= fixed_point_t::_1 ? density - fixed_point_t::_1 : fixed_point_t::_1 - density;
	const fixed_point_t penalty = defines.density_linear * x + defines.density_quadratic * x * x;
	if (density < fixed_point_t::_1) {
		const fixed_point_t factor = std::max(fixed_point_t::_0, fixed_point_t::_1 - penalty);
		effect.defence_mult = factor;
		effect.breakthrough_mult = factor;
	} else {
		effect.casualties_mult = fixed_point_t::_1 + penalty;
		effect.breakthrough_mult = fixed_point_t::_1 + penalty;
		effect.attack_mult = fixed_point_t::_1 + defines.density_attack_share.of(penalty);
	}
	return effect;
}

fixed_point_t battle_formulas::effective_defence(OpenVicWarDefines const& defines, DivisionStats const& defender, const fixed_point_t extra_bonus) {
	return defender.defence * (fixed_point_t::_1 + defines.fortification_per_level.of(defender.fortification) + extra_bonus);
}

fixed_point_t battle_formulas::breakthrough_ratio(
	OpenVicWarDefines const& defines, DivisionStats const& attacker, DivisionStats const& defender, const fixed_point_t extra_defence_bonus
) {
	const fixed_point_t defence = effective_defence(defines, defender, extra_defence_bonus);
	if (defence <= fixed_point_t::_0) {
		return attacker.breakthrough > fixed_point_t::_0 ? fixed_point_t(1000) : fixed_point_t::_0;
	}
	return std::min(fixed_point_t(1000), attacker.breakthrough / defence);
}

bool battle_formulas::breakthrough_day(OpenVicWarDefines const& defines, const fixed_point_t ratio, BreakthroughCounter& counter) {
	if (ratio >= defines.break_ratio_day) {
		counter.days = 0;
		return true;
	}
	if (ratio >= defines.break_ratio_week) {
		counter.days++;
		if (counter.days >= defines.break_week_days) {
			counter.days = 0;
			return true;
		}
		return false;
	}
	counter.days = 0;
	return false;
}

fixed_point_t battle_formulas::occupation_power(OpenVicWarDefines const& defines, const fixed_point_t weight_sum) {
	return weight_sum / defines.occupation_reference_regiments;
}

fixed_point_t battle_formulas::occupation_speed(const fixed_point_t power, const fixed_point_t speed_pct_sum, const fixed_point_t speed_mult_product) {
	return power * (fixed_point_t::_1 + speed_pct_sum) * speed_mult_product;
}

fixed_point_t battle_formulas::occupation_days(const fixed_point_t base_days, const fixed_point_t speed, const bool instant) {
	if (instant) {
		return fixed_point_t::_0;
	}
	// A division with no power or speed never takes a province (a very big number of days)
	return speed > fixed_point_t::_0 ? base_days / speed : fixed_point_t(36500);
}

fixed_point_t battle_formulas::weapon_ratio(const fixed_point_t first_points, const fixed_point_t best_points, const fixed_point_t fill) {
	const fixed_point_t clamped_fill = fill < fixed_point_t::_0 ? fixed_point_t::_0 : fill > fixed_point_t::_1 ? fixed_point_t::_1 : fill;
	return first_points + clamped_fill * (best_points - first_points);
}

fixed_point_t battle_formulas::damage_per_day(
	OpenVicWarDefines const& defines, DivisionStats const& attacker, DivisionStats const& defender, const bool target_is_attacker
) {
	// In proportion to the attack of the one who deals it (so to its width; its density is in it), less against a strong defence, more against a dense
	// formation (the casualties of the density). The attacker's defence softens the damage it takes only by ATTACKER_DEFENSE_FACTOR.
	const fixed_point_t defence = effective_defence(defines, defender);
	const fixed_point_t softening = target_is_attacker ? defines.attacker_defense_factor.of(defence) : defence;
	const fixed_point_t defence_factor = fixed_point_t::_1 + softening / defines.battle_defence_reference;
	return attacker.attack * defines.battle_damage_per_attack * defender.casualties_mult / defence_factor;
}

fixed_point_t battle_formulas::days_to_destroy(const fixed_point_t durability, const fixed_point_t damage_per_day) {
	return damage_per_day > fixed_point_t::_0 ? durability / damage_per_day : fixed_point_t::_0;
}
