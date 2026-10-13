#include "UnitInstance.hpp"

using namespace OpenVic;

UnitInstance::UnitInstance(
	unique_id_t new_unique_id,
	std::string_view new_name,
	UnitType const& new_unit_type
) : unique_id { new_unique_id },
	name { new_name },
	unit_type { new_unit_type },
	organisation { new_unit_type.default_organisation },
	max_organisation { organisation },
	strength { new_unit_type.max_strength } {}

void UnitInstance::set_name(std::string_view new_name) {
	name = new_name;
}

void UnitInstance::take_damage(fixed_point_t organisation_loss, fixed_point_t strength_loss) {
	organisation -= organisation_loss;
	if (organisation < fixed_point_t::_0) {
		organisation = fixed_point_t::_0;
	}
	strength -= strength_loss;
	if (strength < fixed_point_t::_0) {
		strength = fixed_point_t::_0;
	}
}

void UnitInstance::recover(fixed_point_t organisation_gain, fixed_point_t strength_gain) {
	organisation += organisation_gain;
	if (organisation > max_organisation) {
		organisation = max_organisation;
	}
	strength += strength_gain;
	if (strength > unit_type.max_strength) {
		strength = unit_type.max_strength;
	}
}

UnitInstanceBranched<unit_branch_t::LAND>::UnitInstanceBranched(
	unique_id_t new_unique_id,
	std::string_view new_name,
	RegimentType const& new_regiment_type,
	Pop* new_pop,
	bool new_mobilised
) : UnitInstance { new_unique_id, new_name, new_regiment_type },
	pop { new_pop },
	mobilised { new_mobilised } {}

UnitInstanceBranched<unit_branch_t::NAVAL>::UnitInstanceBranched(
	unique_id_t new_unique_id,
	std::string_view new_name,
	ShipType const& new_ship_type
) : UnitInstance { new_unique_id, new_name, new_ship_type } {}
