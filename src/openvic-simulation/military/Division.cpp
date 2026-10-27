#include "Division.hpp"

#include <algorithm>
#include <limits>
#include <vector>

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/country/CountryInstanceManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/military/UnitInstance.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/military/UnitType.hpp"

using namespace OpenVic;

std::size_t OpenVic::get_division_slot_kind(RegimentType const& regiment_type) {
	using enum UnitType::unit_category_t;
	switch (regiment_type.unit_category) {
	case INFANTRY: return 0;
	case CAVALRY: return 1;
	case SUPPORT: return 2;
	default: return 3;
	}
}

namespace {
	DivisionInstance const* find_division(ArmyInstance const& army, const std::uint32_t id) {
		for (DivisionInstance const& division : army.get_divisions()) {
			if (division.id == id) {
				return &division;
			}
		}
		return nullptr;
	}

	// The division of the army out of the template that takes the regiments no division of a template wants (made when there is none)
	std::uint32_t reserve_division_id(UnitInstanceManager& unit_manager, ArmyInstance& army) {
		for (DivisionInstance const& division : army.get_divisions()) {
			if (division.template_id == 0) {
				return division.id;
			}
		}
		const std::uint32_t id = unit_manager.next_division_id++;
		army.get_mutable_divisions().push_back({ id, 0 });
		return id;
	}

	std::size_t count_kind(ArmyInstance& army, const std::uint32_t division_id, const std::size_t kind) {
		std::size_t count = 0;
		for (RegimentInstance const& regiment : army.get_regiment_instances()) {
			if (regiment.get_division_id() == division_id && get_division_slot_kind(regiment.get_regiment_type()) == kind) {
				count++;
			}
		}
		return count;
	}

	void remove_empty_divisions(ArmyInstance& army) {
		memory::vector<DivisionInstance>& divisions = army.get_mutable_divisions();
		std::erase_if(divisions, [&army](DivisionInstance const& division) -> bool {
			for (RegimentInstance const& regiment : army.get_regiment_instances()) {
				if (regiment.get_division_id() == division.id) {
					return false;
				}
			}
			return true;
		});
	}

	// Puts every regiment of the army into a division: the free slots of the divisions of the templates are filled, the others wait in the
	// reserve (out of the template)
	void organise_army(UnitInstanceManager& unit_manager, ArmyInstance& army, CountryInstance const& country) {
		for (RegimentInstance& regiment : army.get_regiment_instances()) {
			if (regiment.get_division_id() != 0 && find_division(army, regiment.get_division_id()) == nullptr) {
				regiment.set_division_id(0);
			}
		}
		for (RegimentInstance& regiment : army.get_regiment_instances()) {
			if (regiment.get_division_id() != 0) {
				continue;
			}
			const std::size_t kind = get_division_slot_kind(regiment.get_regiment_type());
			std::uint32_t target = 0;
			for (DivisionInstance const& division : army.get_divisions()) {
				if (division.template_id == 0) {
					continue;
				}
				DivisionTemplate const* const division_template = country.find_division_template(division.template_id);
				if (division_template != nullptr && count_kind(army, division.id, kind) < division_template->slots[kind]) {
					target = division.id;
					break;
				}
			}
			regiment.set_division_id(target != 0 ? target : reserve_division_id(unit_manager, army));
		}
		remove_empty_divisions(army);
	}

	bool valid_slots(std::array<std::uint32_t, DIVISION_SLOT_KINDS> const& slots) {
		std::uint32_t total = 0;
		for (const std::uint32_t slot : slots) {
			if (slot > MAX_DIVISION_SLOTS) {
				return false;
			}
			total += slot;
		}
		return total >= 1 && total <= MAX_DIVISION_SLOTS;
	}
}

void OpenVic::update_all_divisions(InstanceManager& instance_manager) {
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists()) {
			continue;
		}
		if (country.has_army_organisation()) {
			if (!country.are_division_templates_made()) {
				country.set_division_templates_made(true);
				// The standard division: three regiments of infantry and one of cavalry
				country.get_mutable_division_templates().push_back({ country.take_division_template_id(), { 3, 1, 0, 0 } });
			}
			for (ArmyInstance& army : country.get_armies()) {
				organise_army(unit_manager, army, country);
			}
		} else {
			for (ArmyInstance& army : country.get_armies()) {
				if (!army.get_divisions().empty()) {
					army.get_mutable_divisions().clear();
					for (RegimentInstance& regiment : army.get_regiment_instances()) {
						regiment.set_division_id(0);
					}
				}
				// Generals command only the armies of the countries with the technology (they are not deleted, only taken off)
				if (army.get_leader() != nullptr) {
					army.set_leader(nullptr);
				}
			}
		}
	}
}

bool OpenVic::create_division_template(CountryInstance& country, std::array<std::uint32_t, DIVISION_SLOT_KINDS> const& slots) {
	if (!country.has_army_organisation() || !valid_slots(slots) || country.get_division_templates().size() >= MAX_DIVISION_TEMPLATES) {
		return false;
	}
	country.get_mutable_division_templates().push_back({ country.take_division_template_id(), slots });
	return true;
}

bool OpenVic::update_division_template(
	InstanceManager& instance_manager, CountryInstance& country, const std::uint32_t template_id,
	std::array<std::uint32_t, DIVISION_SLOT_KINDS> const& slots
) {
	if (!country.has_army_organisation() || !valid_slots(slots)) {
		return false;
	}
	DivisionTemplate* division_template = nullptr;
	for (DivisionTemplate& candidate : country.get_mutable_division_templates()) {
		if (candidate.id == template_id) {
			division_template = &candidate;
		}
	}
	if (division_template == nullptr || division_template->slots == slots) {
		return false;
	}
	division_template->slots = slots;

	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	for (ArmyInstance& army : country.get_armies()) {
		// The ids of the divisions of the template (the vector of the divisions changes when the reserve is made)
		std::vector<std::uint32_t> division_ids;
		for (DivisionInstance const& division : army.get_divisions()) {
			if (division.template_id == template_id) {
				division_ids.push_back(division.id);
			}
		}
		if (division_ids.empty()) {
			continue;
		}

		// The regiments that do not fit go to the reserve
		for (const std::uint32_t division_id : division_ids) {
			std::array<std::uint32_t, DIVISION_SLOT_KINDS> kept {};
			for (RegimentInstance& regiment : army.get_regiment_instances()) {
				if (regiment.get_division_id() != division_id) {
					continue;
				}
				const std::size_t kind = get_division_slot_kind(regiment.get_regiment_type());
				if (kept[kind] < slots[kind]) {
					kept[kind]++;
				} else {
					regiment.set_division_id(reserve_division_id(unit_manager, army));
				}
			}
		}
		// The free slots are filled from the reserve (the regiments that are not in a division of a template)
		for (const std::uint32_t division_id : division_ids) {
			for (std::size_t kind = 0; kind < DIVISION_SLOT_KINDS; ++kind) {
				std::size_t have = count_kind(army, division_id, kind);
				for (RegimentInstance& regiment : army.get_regiment_instances()) {
					if (have >= slots[kind]) {
						break;
					}
					DivisionInstance const* const from = find_division(army, regiment.get_division_id());
					if (from != nullptr && from->template_id == 0 && get_division_slot_kind(regiment.get_regiment_type()) == kind) {
						regiment.set_division_id(division_id);
						have++;
					}
				}
			}
		}
		remove_empty_divisions(army);
	}
	return true;
}

bool OpenVic::delete_division_template(CountryInstance& country, const std::uint32_t template_id) {
	memory::vector<DivisionTemplate>& templates = country.get_mutable_division_templates();
	const auto it = std::find_if(templates.begin(), templates.end(), [template_id](DivisionTemplate const& candidate) -> bool {
		return candidate.id == template_id;
	});
	if (it == templates.end()) {
		return false;
	}
	templates.erase(it);
	// The divisions of the template stay, out of the template
	for (ArmyInstance& army : country.get_armies()) {
		for (DivisionInstance& division : army.get_mutable_divisions()) {
			if (division.template_id == template_id) {
				division.template_id = 0;
			}
		}
	}
	return true;
}

bool OpenVic::reform_army_to_template(
	InstanceManager& instance_manager, ArmyInstance& army, const std::uint32_t division_id, const std::uint32_t template_id
) {
	CountryInstance& country = army.get_country();
	if (!country.has_army_organisation() || army.is_in_combat()) {
		return false;
	}
	DivisionTemplate const* const division_template = country.find_division_template(template_id);
	if (division_template == nullptr) {
		return false;
	}
	if (division_id != 0) {
		DivisionInstance const* const division = find_division(army, division_id);
		if (division == nullptr || division->template_id != 0) {
			return false;
		}
	}

	// The regiments of the divisions out of the template, by kind, in the order of the army
	std::array<std::vector<RegimentInstance*>, DIVISION_SLOT_KINDS> candidates;
	for (RegimentInstance& regiment : army.get_regiment_instances()) {
		DivisionInstance const* const division = find_division(army, regiment.get_division_id());
		if (division == nullptr || division->template_id != 0 || (division_id != 0 && division->id != division_id)) {
			continue;
		}
		candidates[get_division_slot_kind(regiment.get_regiment_type())].push_back(&regiment);
	}

	std::size_t whole = std::numeric_limits<std::size_t>::max();
	for (std::size_t kind = 0; kind < DIVISION_SLOT_KINDS; ++kind) {
		if (division_template->slots[kind] > 0) {
			whole = std::min(whole, candidates[kind].size() / division_template->slots[kind]);
		}
	}
	if (whole == 0 || whole == std::numeric_limits<std::size_t>::max()) {
		return false;
	}

	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	std::array<std::size_t, DIVISION_SLOT_KINDS> next {};
	for (std::size_t made = 0; made < whole; ++made) {
		const std::uint32_t id = unit_manager.next_division_id++;
		army.get_mutable_divisions().push_back({ id, template_id });
		for (std::size_t kind = 0; kind < DIVISION_SLOT_KINDS; ++kind) {
			for (std::uint32_t slot = 0; slot < division_template->slots[kind]; ++slot) {
				candidates[kind][next[kind]++]->set_division_id(id);
			}
		}
	}
	remove_empty_divisions(army);
	return true;
}
