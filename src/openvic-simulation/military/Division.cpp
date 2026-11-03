#include "Division.hpp"

#include <algorithm>
#include <limits>
#include <span>
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
	// The file of the default template
	std::span<const memory::vector<memory::string>> const ranks = unit_type_manager.get_default_template_ranks();
	for (std::size_t rank = 0; rank < ranks.size() && rank < DIVISION_MAX_RANKS; ++rank) {
		for (std::size_t row = 0; row < ranks[rank].size() && row < DIVISION_MAX_DEPTH; ++row) {
			RegimentType const* const regiment_type = unit_type_manager.resolve_vic2_regiment_type(ranks[rank][row]);
			const std::size_t kind = regiment_type != nullptr ? get_division_slot_kind(*regiment_type) : DIVISION_MAX_TYPES;
			if (kind < DIVISION_MAX_TYPES) {
				grid[DivisionTemplate::cell_index(rank, row)] = static_cast<std::uint8_t>(kind + 1);
			}
		}
	}
	if (std::any_of(grid.begin(), grid.end(), [](const std::uint8_t cell) -> bool { return cell != 0; })) {
		return grid;
	}
	RegimentType const* const infantry = find_type("line_infantry", INFANTRY);
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

	// The cells of a grid line by line: the first line of every rank, then the second, ... (the order the cells are filled in)
	memory::vector<std::size_t> cells_line_by_line(DivisionGrid const& cells) {
		memory::vector<std::size_t> order;
		for (std::size_t row = 0; row < DIVISION_MAX_DEPTH; ++row) {
			for (std::size_t rank = 0; rank < DIVISION_MAX_RANKS; ++rank) {
				if (cells[DivisionTemplate::cell_index(rank, row)] != 0) {
					order.push_back(DivisionTemplate::cell_index(rank, row));
				}
			}
		}
		return order;
	}

	// The cell every regiment takes (the index of the cell, DIVISION_GRID_SIZE for none): a cell takes a regiment of its type, else one of the
	// same category; the regiments in their order
	memory::vector<std::size_t> match_cells(
		UnitTypeManager const& unit_type_manager, DivisionGrid const& cells, std::span<RegimentType const* const> regiment_types
	) {
		memory::vector<std::size_t> taken_by(regiment_types.size(), DIVISION_GRID_SIZE);
		std::array<bool, DIVISION_GRID_SIZE> filled {};
		const memory::vector<std::size_t> order = cells_line_by_line(cells);
		for (const bool same_category : { false, true }) {
			for (const std::size_t cell : order) {
				if (filled[cell]) {
					continue;
				}
				RegimentType const* const cell_type = unit_type_manager.get_regiment_type_by_index(regiment_type_index_t(cells[cell] - 1));
				if (cell_type == nullptr) {
					continue;
				}
				for (std::size_t index = 0; index < regiment_types.size(); ++index) {
					if (taken_by[index] != DIVISION_GRID_SIZE) {
						continue;
					}
					RegimentType const& regiment_type = *regiment_types[index];
					if (same_category ? regiment_type.unit_category == cell_type->unit_category : &regiment_type == cell_type) {
						taken_by[index] = cell;
						filled[cell] = true;
						break;
					}
				}
			}
		}
		return taken_by;
	}

	// The template a regiment of the country goes to when it fits no division of its army: the default one (the first template), nullptr if the
	// country has none
	DivisionTemplate const* default_template_of(CountryInstance const& country) {
		return country.get_division_templates().empty() ? nullptr : &country.get_division_templates().front();
	}

	// Whether the regiment would take a free cell of the division (its regiments and the candidate matched to the template)
	bool fits_division(
		UnitTypeManager const& unit_type_manager, ArmyInstance const& army, DivisionInstance const& division, DivisionTemplate const& division_template,
		RegimentInstance const& candidate
	) {
		memory::vector<RegimentType const*> types;
		for (RegimentInstance const& regiment : army.get_regiment_instances()) {
			if (regiment.get_division_id() == division.id) {
				types.push_back(&regiment.get_regiment_type());
			}
		}
		types.push_back(&candidate.get_regiment_type());
		return match_cells(unit_type_manager, division_template.cells, types).back() != DIVISION_GRID_SIZE;
	}

	// Lays the loose regiments out by the template (docs/WAR_ASSUMPTIONS.md, section 6): one new division after the other takes the regiments that
	// fit its cells, as long as one does; the regiments that fit no cell of the template go to the new divisions one by one (behind their ranks)
	void lay_out_by_template(
		UnitInstanceManager& unit_manager, UnitTypeManager const& unit_type_manager, ArmyInstance& army, DivisionTemplate const& division_template,
		memory::vector<RegimentInstance*> loose
	) {
		memory::vector<std::uint32_t> made;
		while (!loose.empty()) {
			memory::vector<RegimentType const*> types;
			for (RegimentInstance const* regiment : loose) {
				types.push_back(&regiment->get_regiment_type());
			}
			const memory::vector<std::size_t> taken_by = match_cells(unit_type_manager, division_template.cells, types);
			if (std::all_of(taken_by.begin(), taken_by.end(), [](const std::size_t cell) -> bool { return cell == DIVISION_GRID_SIZE; })) {
				break;
			}
			const std::uint32_t id = unit_manager.next_division_id++;
			army.get_mutable_divisions().push_back({ id, division_template.id });
			made.push_back(id);
			memory::vector<RegimentInstance*> rest;
			for (std::size_t index = 0; index < loose.size(); ++index) {
				if (taken_by[index] != DIVISION_GRID_SIZE) {
					loose[index]->set_division_id(id);
				} else {
					rest.push_back(loose[index]);
				}
			}
			loose = std::move(rest);
		}
		if (loose.empty()) {
			return;
		}
		if (made.empty()) {
			// No new division: the divisions of the template the army has take them, else one division of the template is made for them
			for (DivisionInstance const& division : army.get_divisions()) {
				if (division.template_id == division_template.id) {
					made.push_back(division.id);
				}
			}
			if (made.empty()) {
				made.push_back(unit_manager.next_division_id++);
				army.get_mutable_divisions().push_back({ made.back(), division_template.id });
			}
		}
		for (std::size_t index = 0; index < loose.size(); ++index) {
			loose[index]->set_division_id(made[index % made.size()]);
		}
	}

	// Puts every regiment of the army into a division: the free cells of the divisions of the templates are filled; the others wait in the
	// reserve (out of the template) of a country with mechanic:division_templates, or make divisions of the default template (always on the first
	// day: the regiments of the history)
	void organise_army(
		UnitInstanceManager& unit_manager, UnitTypeManager const& unit_type_manager, ArmyInstance& army, CountryInstance const& country,
		const bool first_day
	) {
		for (RegimentInstance& regiment : army.get_regiment_instances()) {
			if (regiment.get_division_id() != 0 && find_division(army, regiment.get_division_id()) == nullptr) {
				regiment.set_division_id(0);
			}
		}
		memory::vector<RegimentInstance*> loose;
		for (RegimentInstance& regiment : army.get_regiment_instances()) {
			if (regiment.get_division_id() != 0) {
				continue;
			}
			std::uint32_t target = 0;
			for (DivisionInstance const& division : army.get_divisions()) {
				if (division.template_id == 0) {
					continue;
				}
				DivisionTemplate const* const division_template = country.find_division_template(division.template_id);
				if (division_template != nullptr && fits_division(unit_type_manager, army, division, *division_template, regiment)) {
					target = division.id;
					break;
				}
			}
			if (target != 0) {
				regiment.set_division_id(target);
			} else {
				loose.push_back(&regiment);
			}
		}
		if (!loose.empty()) {
			DivisionTemplate const* const division_template = default_template_of(country);
			if (division_template == nullptr || (!first_day && country.has_unlock(UNLOCK_MECHANIC_DIVISION_TEMPLATES))) {
				for (RegimentInstance* regiment : loose) {
					regiment->set_division_id(reserve_division_id(unit_manager, army));
				}
			} else {
				lay_out_by_template(unit_manager, unit_type_manager, army, *division_template, std::move(loose));
			}
		}
		remove_empty_divisions(army);
	}
}

DivisionLayout OpenVic::layout_division(
	UnitTypeManager const& unit_type_manager, CountryInstance const& country, ArmyInstance const& army, DivisionInstance const& division,
	const std::uint32_t extra_depth
) {
	DivisionLayout layout;
	DivisionTemplate const* const division_template = division.template_id != 0 ? country.find_division_template(division.template_id) : nullptr;
	memory::vector<std::uint32_t> indices;
	memory::vector<RegimentType const*> types;
	std::span<const std::reference_wrapper<const RegimentInstance>> const regiments = army.get_regiment_instances();
	for (std::size_t index = 0; index < regiments.size(); ++index) {
		RegimentInstance const& regiment = regiments[index];
		if (regiment.get_division_id() == division.id && regiment.get_strength() > fixed_point_t::_0) {
			indices.push_back(static_cast<std::uint32_t>(index));
			types.push_back(&regiment.get_regiment_type());
		}
	}
	if (indices.empty()) {
		return layout;
	}
	std::size_t template_ranks = 0, template_depth = 0;
	memory::vector<std::size_t> taken_by(indices.size(), DIVISION_GRID_SIZE);
	if (division_template != nullptr) {
		taken_by = match_cells(unit_type_manager, division_template->cells, types);
		for (std::size_t cell = 0; cell < DIVISION_GRID_SIZE; ++cell) {
			if (division_template->cells[cell] != 0) {
				template_ranks = std::max(template_ranks, cell / DIVISION_MAX_DEPTH + 1);
				template_depth = std::max(template_depth, cell % DIVISION_MAX_DEPTH + 1);
			}
		}
	}
	// The regiments without a cell: behind the ranks of the template, or in ranks of extra_depth rows (a division out of the template)
	const std::size_t rows_per_rank = std::max<std::size_t>(extra_depth, 1);
	std::size_t extra = 0;
	for (std::size_t index = 0; index < indices.size(); ++index) {
		DivisionLayout::Entry entry;
		entry.regiment = indices[index];
		if (taken_by[index] != DIVISION_GRID_SIZE) {
			entry.rank = static_cast<std::uint8_t>(taken_by[index] / DIVISION_MAX_DEPTH);
			entry.row = static_cast<std::uint8_t>(taken_by[index] % DIVISION_MAX_DEPTH);
		} else if (template_ranks > 0) {
			entry.rank = static_cast<std::uint8_t>(extra % template_ranks);
			entry.row = static_cast<std::uint8_t>(template_depth + extra / template_ranks);
			extra++;
		} else {
			const std::size_t column = extra / rows_per_rank;
			entry.rank = static_cast<std::uint8_t>(column % DIVISION_MAX_RANKS);
			entry.row = static_cast<std::uint8_t>(extra % rows_per_rank + rows_per_rank * (column / DIVISION_MAX_RANKS));
			extra++;
		}
		layout.entries.push_back(entry);
	}
	std::sort(layout.entries.begin(), layout.entries.end(), [](DivisionLayout::Entry const& a, DivisionLayout::Entry const& b) -> bool {
		return a.rank != b.rank ? a.rank < b.rank : a.row != b.row ? a.row < b.row : a.regiment < b.regiment;
	});
	// The lines: the rows of a rank that have a regiment, in their order
	std::uint8_t line = 0;
	for (std::size_t index = 0; index < layout.entries.size(); ++index) {
		DivisionLayout::Entry& entry = layout.entries[index];
		if (index == 0 || entry.rank != layout.entries[index - 1].rank) {
			layout.width++;
			line = 0;
		} else if (entry.row != layout.entries[index - 1].row) {
			line++;
		}
		entry.line = line;
	}
	return layout;
}

void OpenVic::update_all_divisions(InstanceManager& instance_manager) {
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	UnitTypeManager const& unit_type_manager = instance_manager.definition_manager.get_military_manager().get_unit_type_manager();
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists()) {
			continue;
		}
		update_army_groups(instance_manager, country);
		const bool first_day = !country.are_division_templates_made();
		if (first_day) {
			country.set_division_templates_made(true);
			// The default template (ov_data/common/ov_units/default_template.txt)
			country.get_mutable_division_templates().push_back(
				make_template(country.take_division_template_id(), make_standard_division_grid(unit_type_manager))
			);
		}
		const bool command = country.has_army_command();
		for (ArmyInstance& army : country.get_armies()) {
			organise_army(unit_manager, unit_type_manager, army, country, first_day);
			// Generals command only the armies of the countries with mechanic:command (they are not deleted, only taken off)
			if (!command && army.get_leader() != nullptr) {
				army.set_leader(nullptr);
			}
		}
	}
}

std::size_t OpenVic::count_division_units(ArmyInstance const& army) {
	std::size_t units = 0;
	for (DivisionInstance const& division : army.get_divisions()) {
		units += count_regiments_in(army, division.id) > 0;
	}
	// Regiments that are in no division yet (they get one the next day) are a unit each
	for (RegimentInstance const& regiment : army.get_regiment_instances()) {
		units += regiment.get_division_id() == 0 || find_division(army, regiment.get_division_id()) == nullptr;
	}
	return units;
}

ArmyInstance* OpenVic::split_division(UnitInstanceManager& unit_manager, ArmyInstance& army, const std::uint32_t division_id) {
	DivisionInstance const* const division = find_division(army, division_id);
	if (division == nullptr) {
		return nullptr;
	}
	const DivisionInstance moved_division = *division;
	std::vector<RegimentInstance*> moved;
	for (RegimentInstance& regiment : army.get_regiment_instances()) {
		if (regiment.get_division_id() == moved_division.id) {
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
	// The division keeps its id and its template in the new army
	std::erase_if(army.get_mutable_divisions(), [&moved_division](DivisionInstance const& other) -> bool {
		return other.id == moved_division.id;
	});
	new_army->get_mutable_divisions().clear();
	new_army->get_mutable_divisions().push_back(moved_division);
	for (RegimentInstance& regiment : new_army->get_regiment_instances()) {
		regiment.set_division_id(moved_division.id);
	}
	army.update_gamestate();
	new_army->update_gamestate();
	return new_army;
}

ArmyInstance* OpenVic::split_division_unit(UnitInstanceManager& unit_manager, ArmyInstance& army) {
	if (count_division_units(army) < 2) {
		return nullptr;
	}
	// The last division that has regiments goes as a whole
	std::uint32_t last = 0;
	for (DivisionInstance const& division : army.get_divisions()) {
		if (count_regiments_in(army, division.id) > 0) {
			last = division.id;
		}
	}
	return last != 0 ? split_division(unit_manager, army, last) : nullptr;
}

bool OpenVic::create_division_template(InstanceManager& instance_manager, CountryInstance& country, DivisionGrid const& grid) {
	if (!country.has_unlock(UNLOCK_MECHANIC_DIVISION_TEMPLATES) || !is_valid_division_grid(instance_manager, country, grid)
		|| country.get_division_templates().size() >= MAX_DIVISION_TEMPLATES) {
		return false;
	}
	country.get_mutable_division_templates().push_back(make_template(country.take_division_template_id(), grid));
	return true;
}

bool OpenVic::update_division_template(
	InstanceManager& instance_manager, CountryInstance& country, const std::uint32_t template_id, DivisionGrid const& grid
) {
	if (!country.has_unlock(UNLOCK_MECHANIC_DIVISION_TEMPLATES) || !is_valid_division_grid(instance_manager, country, grid)) {
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
	if (!country.has_unlock(UNLOCK_MECHANIC_DIVISION_TEMPLATES)) {
		return false;
	}
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
	if (army.is_in_combat()) {
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
