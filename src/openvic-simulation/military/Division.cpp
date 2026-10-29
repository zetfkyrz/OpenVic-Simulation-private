#include "Division.hpp"

#include <algorithm>
#include <limits>
#include <vector>

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/country/CountryInstanceManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/military/ArmyGroup.hpp"
#include "openvic-simulation/military/UnitInstance.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/military/UnitType.hpp"
#include "openvic-simulation/military/Unlocks.hpp"
#include "openvic-simulation/DefinitionManager.hpp"

#include <type_safe/strong_typedef.hpp>

using namespace OpenVic;

std::size_t OpenVic::get_division_slot_kind(RegimentType const& regiment_type) {
	return static_cast<std::size_t>(type_safe::get(regiment_type.index));
}

DivisionGrid OpenVic::make_standard_division_grid(UnitTypeManager const& unit_type_manager) {
	using enum UnitType::unit_category_t;
	const auto find_type = [&unit_type_manager](std::string_view identifier, const UnitType::unit_category_t category) -> RegimentType const* {
		if (RegimentType const* const named = unit_type_manager.get_regiment_type_by_identifier(identifier); named != nullptr) {
			return named;
		}
		for (RegimentType const& regiment_type : unit_type_manager.get_regiment_types()) {
			if (regiment_type.unit_category == category) {
				return &regiment_type;
			}
		}
		return nullptr;
	};
	DivisionGrid grid {};
	RegimentType const* const infantry = find_type("infantry", INFANTRY);
	RegimentType const* const cavalry = find_type("cavalry", CAVALRY);
	const auto cell_of = [](RegimentType const& regiment_type) -> std::uint8_t {
		const std::size_t kind = get_division_slot_kind(regiment_type);
		return kind < DIVISION_MAX_TYPES ? static_cast<std::uint8_t>(kind + 1) : 0;
	};
	if (infantry != nullptr) {
		// 2 ranks of 2 rows
		for (std::size_t rank = 0; rank < 2; ++rank) {
			for (std::size_t row = 0; row < 2; ++row) {
				grid[DivisionTemplate::cell_index(rank, row)] = cell_of(*infantry);
			}
		}
	}
	if (cavalry != nullptr) {
		// 1 rank of 2 rows
		for (std::size_t row = 0; row < 2; ++row) {
			grid[DivisionTemplate::cell_index(2, row)] = cell_of(*cavalry);
		}
	}
	return grid;
}

bool OpenVic::is_valid_division_grid(InstanceManager const& instance_manager, CountryInstance const& country, DivisionGrid const& grid) {
	const DivisionTemplateLimits limits = get_template_limits(instance_manager, country);
	UnitTypeManager const& unit_type_manager = instance_manager.definition_manager.get_military_manager().get_unit_type_manager();
	std::uint32_t cells = 0;
	for (std::size_t rank = 0; rank < DIVISION_MAX_RANKS; ++rank) {
		for (std::size_t row = 0; row < DIVISION_MAX_DEPTH; ++row) {
			const std::uint8_t cell = grid[DivisionTemplate::cell_index(rank, row)];
			if (cell == 0) {
				continue;
			}
			// Only the cells the country may use, with a regiment type that exists
			if (rank >= limits.ranks || row >= limits.depth || cell > DIVISION_MAX_TYPES
				|| unit_type_manager.get_regiment_type_by_index(regiment_type_index_t(cell - 1)) == nullptr) {
				return false;
			}
			cells++;
		}
	}
	return cells >= 1;
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
				if (division_template != nullptr && kind < DIVISION_MAX_TYPES && count_kind(army, division.id, kind) < division_template->counts[kind]) {
					target = division.id;
					break;
				}
			}
			regiment.set_division_id(target != 0 ? target : reserve_division_id(unit_manager, army));
		}
		remove_empty_divisions(army);
	}

	std::size_t count_regiments_in(ArmyInstance const& army, const std::uint32_t division_id) {
		std::size_t count = 0;
		for (RegimentInstance const& regiment : army.get_regiment_instances()) {
			count += regiment.get_division_id() == division_id;
		}
		return count;
	}

	DivisionTemplate make_template(const std::uint32_t id, DivisionGrid const& grid) {
		DivisionTemplate division_template;
		division_template.id = id;
		division_template.cells = grid;
		division_template.rebuild_counts();
		return division_template;
	}
}

void OpenVic::update_all_divisions(InstanceManager& instance_manager) {
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists()) {
			continue;
		}
		update_army_groups(instance_manager, country);
		if (country.has_army_organisation()) {
			if (!country.are_division_templates_made()) {
				country.set_division_templates_made(true);
				// The standard division: four regiments of infantry (2 ranks of 2 rows) and two of cavalry (1 rank of 2 rows)
				country.get_mutable_division_templates().push_back(make_template(
					country.take_division_template_id(),
					make_standard_division_grid(instance_manager.definition_manager.get_military_manager().get_unit_type_manager())
				));
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

std::size_t OpenVic::count_division_units(ArmyInstance const& army, const std::uint32_t chunk_size) {
	std::size_t units = 0;
	std::size_t loose_regiments = 0;
	for (DivisionInstance const& division : army.get_divisions()) {
		std::size_t in_division = 0;
		for (RegimentInstance const& regiment : army.get_regiment_instances()) {
			if (regiment.get_division_id() == division.id) {
				in_division++;
			}
		}
		if (in_division == 0) {
			continue;
		}
		if (division.template_id != 0) {
			units++;
		} else {
			loose_regiments += in_division;
		}
	}
	// Regiments that are in no division yet (they are put in the reserve the next day) are loose too
	for (RegimentInstance const& regiment : army.get_regiment_instances()) {
		if (regiment.get_division_id() == 0 || find_division(army, regiment.get_division_id()) == nullptr) {
			loose_regiments++;
		}
	}
	const std::size_t chunk = std::max<std::size_t>(chunk_size, 1);
	return units + (loose_regiments + chunk - 1) / chunk;
}

ArmyInstance* OpenVic::split_division_unit(UnitInstanceManager& unit_manager, ArmyInstance& army, const std::uint32_t chunk_size) {
	if (count_division_units(army, chunk_size) < 2) {
		return nullptr;
	}

	// A division of a template goes as a whole, the last one first
	DivisionInstance const* division_of_template = nullptr;
	for (DivisionInstance const& division : army.get_divisions()) {
		if (division.template_id != 0 && count_regiments_in(army, division.id) > 0) {
			division_of_template = &division;
		}
	}
	const DivisionInstance moved_division = division_of_template != nullptr ? *division_of_template : DivisionInstance {};

	std::vector<RegimentInstance*> moved;
	for (RegimentInstance& regiment : army.get_regiment_instances()) {
		if (division_of_template != nullptr) {
			if (regiment.get_division_id() == moved_division.id) {
				moved.push_back(&regiment);
			}
		} else if (moved.size() < std::max<std::size_t>(chunk_size, 1)) {
			moved.push_back(&regiment);
		}
	}
	if (moved.empty() || moved.size() >= army.get_unit_count()) {
		return nullptr;
	}

	ArmyInstance* const new_army = unit_manager.split_regiment(army, *moved.front());
	if (new_army == nullptr) {
		return nullptr;
	}
	for (size_t i = 1; i < moved.size(); ++i) {
		moved[i]->set_division_id(0);
		if (army.remove_unit(*moved[i])) {
			new_army->add_unit(*moved[i]);
		}
	}
	if (division_of_template != nullptr) {
		// The division keeps its id and its template in the new army
		std::erase_if(army.get_mutable_divisions(), [&moved_division](DivisionInstance const& division) -> bool {
			return division.id == moved_division.id;
		});
		new_army->get_mutable_divisions().push_back(moved_division);
		for (RegimentInstance& regiment : new_army->get_regiment_instances()) {
			regiment.set_division_id(moved_division.id);
		}
	}
	army.update_gamestate();
	new_army->update_gamestate();
	return new_army;
}

bool OpenVic::create_division_template(InstanceManager& instance_manager, CountryInstance& country, DivisionGrid const& grid) {
	if (!country.has_army_organisation() || !is_valid_division_grid(instance_manager, country, grid)
		|| country.get_division_templates().size() >= MAX_DIVISION_TEMPLATES) {
		return false;
	}
	country.get_mutable_division_templates().push_back(make_template(country.take_division_template_id(), grid));
	return true;
}

bool OpenVic::update_division_template(
	InstanceManager& instance_manager, CountryInstance& country, const std::uint32_t template_id, DivisionGrid const& grid
) {
	if (!country.has_army_organisation() || !is_valid_division_grid(instance_manager, country, grid)) {
		return false;
	}
	DivisionTemplate* division_template = nullptr;
	for (DivisionTemplate& candidate : country.get_mutable_division_templates()) {
		if (candidate.id == template_id) {
			division_template = &candidate;
		}
	}
	if (division_template == nullptr || division_template->cells == grid) {
		return false;
	}
	division_template->cells = grid;
	division_template->rebuild_counts();
	const std::array<std::uint32_t, DIVISION_MAX_TYPES> counts = division_template->counts;

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
			std::array<std::uint32_t, DIVISION_MAX_TYPES> kept {};
			for (RegimentInstance& regiment : army.get_regiment_instances()) {
				if (regiment.get_division_id() != division_id) {
					continue;
				}
				const std::size_t kind = get_division_slot_kind(regiment.get_regiment_type());
				if (kind < DIVISION_MAX_TYPES && kept[kind] < counts[kind]) {
					kept[kind]++;
				} else {
					regiment.set_division_id(reserve_division_id(unit_manager, army));
				}
			}
		}
		// The free slots are filled from the reserve (the regiments that are not in a division of a template)
		for (const std::uint32_t division_id : division_ids) {
			for (std::size_t kind = 0; kind < DIVISION_MAX_TYPES; ++kind) {
				std::size_t have = count_kind(army, division_id, kind);
				for (RegimentInstance& regiment : army.get_regiment_instances()) {
					if (have >= counts[kind]) {
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

	// The regiments of the divisions out of the template, by type, in the order of the army
	std::array<std::vector<RegimentInstance*>, DIVISION_MAX_TYPES> candidates;
	for (RegimentInstance& regiment : army.get_regiment_instances()) {
		DivisionInstance const* const division = find_division(army, regiment.get_division_id());
		if (division == nullptr || division->template_id != 0 || (division_id != 0 && division->id != division_id)) {
			continue;
		}
		const std::size_t candidate_kind = get_division_slot_kind(regiment.get_regiment_type());
		if (candidate_kind < DIVISION_MAX_TYPES) {
			candidates[candidate_kind].push_back(&regiment);
		}
	}

	std::size_t whole = std::numeric_limits<std::size_t>::max();
	for (std::size_t kind = 0; kind < DIVISION_MAX_TYPES; ++kind) {
		if (division_template->counts[kind] > 0) {
			whole = std::min(whole, candidates[kind].size() / division_template->counts[kind]);
		}
	}
	if (whole == 0 || whole == std::numeric_limits<std::size_t>::max()) {
		return false;
	}

	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	std::array<std::size_t, DIVISION_MAX_TYPES> next {};
	for (std::size_t made = 0; made < whole; ++made) {
		const std::uint32_t id = unit_manager.next_division_id++;
		army.get_mutable_divisions().push_back({ id, template_id });
		for (std::size_t kind = 0; kind < DIVISION_MAX_TYPES; ++kind) {
			for (std::uint32_t slot = 0; slot < division_template->counts[kind]; ++slot) {
				candidates[kind][next[kind]++]->set_division_id(id);
			}
		}
	}
	remove_empty_divisions(army);
	return true;
}
