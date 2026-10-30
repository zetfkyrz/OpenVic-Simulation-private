#include "BattleFormulas.hpp"

#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/military/DivisionStats.hpp"

using namespace OpenVic;

fixed_point_t battle_formulas::row_weight(const std::size_t row, const std::int32_t row_weight_shift) {
	const std::size_t shift = row_weight_shift > 0 ? static_cast<std::size_t>(row_weight_shift) : 0;
	const std::size_t halvings = row > shift ? row - shift : 0;
	// 1, 1/2, 1/4, ...; a row after 8 halvings weighs nothing a fixed point number can show
	return halvings >= 12 ? fixed_point_t::_0 : fixed_point_t::_1 / static_cast<std::int32_t>(1 << halvings);
}

fixed_point_t battle_formulas::density(
	const fixed_point_t strength_sum, const std::uint32_t width, const fixed_point_t density_reduction
) {
	if (width == 0) {
		return fixed_point_t::_0;
	}
	const fixed_point_t reduction = density_reduction < fixed_point_t::_0 ? fixed_point_t::_0
		: density_reduction > fixed_point_t::_1 ? fixed_point_t::_1 : density_reduction;
	return strength_sum / static_cast<std::int32_t>(width) * (fixed_point_t::_1 - reduction);
}

fixed_point_t battle_formulas::weapon_ratio(const fixed_point_t first_points, const fixed_point_t best_points, const fixed_point_t fill) {
	const fixed_point_t clamped_fill = fill < fixed_point_t::_0 ? fixed_point_t::_0 : fill > fixed_point_t::_1 ? fixed_point_t::_1 : fill;
	return first_points + clamped_fill * (best_points - first_points);
}

fixed_point_t battle_formulas::damage_per_day(
	OpenVicWarDefines const& defines, DivisionStats const& attacker, DivisionStats const& defender
) {
	// In proportion to the attack of the attacker (so to its width), more against a dense formation, less against a strong defence
	const fixed_point_t density_factor = fixed_point_t::_1 + defines.battle_density_damage.of(defender.density);
	const fixed_point_t defence_factor = fixed_point_t::_1 + defender.defence / defines.battle_defence_reference;
	return attacker.attack * defines.battle_damage_per_attack * density_factor / defence_factor;
}

fixed_point_t battle_formulas::days_to_destroy(const fixed_point_t durability, const fixed_point_t damage_per_day) {
	return damage_per_day > fixed_point_t::_0 ? durability / damage_per_day : fixed_point_t::_0;
}
