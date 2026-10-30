#include "UnitEquipment.hpp"

#include <algorithm>

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/military/Equipment.hpp"
#include "openvic-simulation/military/UnitType.hpp"

using namespace OpenVic;

memory::vector<EquipmentDefinition const*> OpenVic::get_allowed_weapons(EquipmentManager const& equipment_manager, RegimentType const& regiment_type) {
	memory::vector<EquipmentDefinition const*> allowed;
	if (regiment_type.ov.is_ov) {
		for (memory::string const& id : regiment_type.ov.equipment) {
			if (EquipmentDefinition const* const weapon = equipment_manager.find_equipment(id); weapon != nullptr) {
				allowed.push_back(weapon);
			}
		}
	} else if (EquipmentLine const* const line = equipment_manager.find_line_of_unit_type(regiment_type.get_identifier()); line != nullptr) {
		allowed = equipment_manager.get_line_equipment(line->identifier);
	}
	std::stable_sort(allowed.begin(), allowed.end(), [](EquipmentDefinition const* a, EquipmentDefinition const* b) { return a->tier < b->tier; });
	allowed.erase(std::unique(allowed.begin(), allowed.end()), allowed.end());
	return allowed;
}

UnitWeapon OpenVic::choose_unit_weapon(
	EquipmentManager const& equipment_manager, CountryInstance const& country, RegimentType const&,
	memory::vector<EquipmentDefinition const*> const& allowed, const fixed_point_t below_min_penalty
) {
	UnitWeapon result;
	if (allowed.empty()) {
		return result;
	}
	result.initial = allowed.front();
	for (EquipmentDefinition const* const weapon : allowed) {
		if (country.has_unlock(weapon->unlock_key)) {
			result.best = weapon;
		}
	}
	if (result.best != nullptr) {
		result.effective = result.best;
		return result;
	}

	// None of the allowed weapons: the nearest lower generation of the line, with the penalty for every generation below the lowest allowed one
	EquipmentDefinition const* fallback = nullptr;
	for (EquipmentDefinition const& weapon : equipment_manager.get_equipment()) {
		if (weapon.line == result.initial->line && weapon.tier < result.initial->tier && country.has_unlock(weapon.unlock_key)
			&& (fallback == nullptr || weapon.tier > fallback->tier)) {
			fallback = &weapon;
		}
	}
	const std::uint32_t tiers_below = fallback != nullptr ? result.initial->tier - fallback->tier : result.initial->tier + 1;
	result.effective = fallback != nullptr ? fallback : result.initial;
	result.keep = std::max(fixed_point_t::_0, fixed_point_t::_1 - below_min_penalty * static_cast<std::int32_t>(tiers_below));
	return result;
}
