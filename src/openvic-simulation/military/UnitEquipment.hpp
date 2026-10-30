#pragma once

#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/types/UnitBranchType.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct EquipmentDefinition;
	struct EquipmentManager;

	// The weapons of a unit type (docs/WAR_ASSUMPTIONS.md, section 5): a unit type uses the best weapon of its list the country has the unlock for,
	// never a better one than the list allows. If the country has none of the weapons of the list, the unit takes the nearest lower generation of the line
	// and loses penalty (ov_war.EQUIPMENT_BELOW_MIN_PENALTY, 25 %) of the attack, the defence and the breakthrough for every generation below the lowest
	// allowed one; the organisation and the speed are not cut.

	// The weapons a regiment type may have, the lowest generation first: the list of the file of the type (ov_units), for the types of Vic2 (no list)
	// all the weapons of their line
	[[nodiscard]] memory::vector<EquipmentDefinition const*> get_allowed_weapons(EquipmentManager const& equipment_manager, RegimentType const& regiment_type);

	struct UnitWeapon {
		EquipmentDefinition const* initial = nullptr; // what the type is built with: the lowest allowed weapon
		EquipmentDefinition const* best = nullptr; // the best allowed weapon the country has, nullptr if it has none
		EquipmentDefinition const* effective = nullptr; // the weapon that counts for a unit that has no other: best, else the fallback, else the initial one
		fixed_point_t keep = fixed_point_t::_1; // what is left of the attack, the defence and the breakthrough (1, or less below the lowest allowed generation)
	};
	[[nodiscard]] UnitWeapon choose_unit_weapon(
		EquipmentManager const& equipment_manager, CountryInstance const& country, RegimentType const& regiment_type,
		memory::vector<EquipmentDefinition const*> const& allowed, fixed_point_t below_min_penalty
	);
}
