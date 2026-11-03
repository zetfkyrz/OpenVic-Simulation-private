#include "BattleFormulas.hpp"

#include <algorithm>

#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/military/DivisionStats.hpp"
#include "openvic-simulation/utility/Logger.hpp"

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

fixed_point_t battle_formulas::defender_protection(
	OpenVicWarDefines const& defines, const fixed_point_t defence, const fixed_point_t fortification_levels, const fixed_point_t terrain_multiplier
) {
	return defence * (fixed_point_t::_1 + defines.fortification_per_level.of(fortification_levels)) * terrain_multiplier;
}

fixed_point_t battle_formulas::effective_defence(OpenVicWarDefines const& defines, DivisionStats const& defender, const fixed_point_t terrain_multiplier) {
	return defender_protection(defines, defender.defence, defender.fortification, terrain_multiplier);
}

fixed_point_t battle_formulas::breakthrough_ratio(const fixed_point_t attacker_breakthrough, const fixed_point_t defender_protection) {
	if (defender_protection <= fixed_point_t::_0) {
		return attacker_breakthrough > fixed_point_t::_0 ? fixed_point_t(1000) : fixed_point_t::_0;
	}
	return std::min(fixed_point_t(1000), attacker_breakthrough / defender_protection);
}

fixed_point_t battle_formulas::breakthrough_ratio(
	OpenVicWarDefines const& defines, DivisionStats const& attacker, DivisionStats const& defender, const fixed_point_t terrain_multiplier
) {
	return breakthrough_ratio(attacker.breakthrough, effective_defence(defines, defender, terrain_multiplier));
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

battle_formulas::Hit battle_formulas::battle_hit(
	OpenVicWarDefines const& defines, const fixed_point_t attack, const fixed_point_t conditions, const fixed_point_t density,
	const fixed_point_t protection, const fixed_point_t casualties_taken
) {
	Hit hit;
	if (attack <= fixed_point_t::_0) {
		return hit;
	}
	// The protection blocks BATTLE_BLOCK_SHARE of the attack at most: a protection as big as the attack or bigger lets 1 - BATTLE_BLOCK_SHARE through
	const fixed_point_t blocked = std::min(fixed_point_t::_1, std::max(protection, fixed_point_t::_0) / attack);
	const fixed_point_t softening = fixed_point_t::_1 - defines.battle_block_share.of(blocked);
	hit.damage = attack * conditions * density * softening;
	hit.strength_loss = hit.damage * defines.battle_strength_per_hit * casualties_taken;
	hit.organisation_loss = hit.damage * defines.battle_organisation_per_hit;
	return hit;
}

bool BattleConditions::load(std::string_view source, ast::NodeCPtr root) {
	using namespace NodeTools;
	terrain.clear();
	const bool parsed = expect_dictionary_keys(
		"terrain", ZERO_OR_ONE, expect_dictionary([this](std::string_view identifier, ast::NodeCPtr value) -> bool {
			fixed_point_t multiplier = fixed_point_t::_1;
			const bool ok = expect_fixed_point(assign_variable_callback(multiplier))(value);
			terrain.emplace_back(memory::string { identifier }, multiplier);
			return ok;
		}),
		"river_crossing", ZERO_OR_ONE, expect_fixed_point(assign_variable_callback(river_crossing)),
		"night", ZERO_OR_ONE, expect_fixed_point(assign_variable_callback(night)),
		"weather", ZERO_OR_ONE, expect_fixed_point(assign_variable_callback(weather))
	)(root);
	if (!parsed) {
		spdlog::warn_s("The conditions of the battles {} are broken, what could be read is used", source);
	}
	return parsed;
}

fixed_point_t BattleConditions::get_terrain(std::string_view terrain_identifier) const {
	for (auto const& [identifier, multiplier] : terrain) {
		if (identifier == terrain_identifier) {
			return multiplier;
		}
	}
	return fixed_point_t::_1;
}
