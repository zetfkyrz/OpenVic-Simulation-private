#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/types/UnitBranchType.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;
	struct UnitInstanceManager;
	struct UnitTypeManager;

	// Divisions and the templates of the divisions (docs/PLAN_ARMY_SUPPLY_FRONTS.md, sections 4 and 4b; docs/WAR_ASSUMPTIONS.md, section 6).
	//
	// Every army of every country consists of divisions and the divisions of regiments: a regiment is always in a division. Every country has the
	// default template (ov_data/common/ov_units/default_template.txt); the regiments of the history of Vic2 are laid out by it when the game starts
	// (whole divisions, then what is left). A country with mechanic:division_templates (the technology standardized_army) makes more templates and
	// changes them; the regiments of such a country that fit no division wait in a division out of the template (the reserve of the army), the ones
	// of a country without the mechanic make new divisions of the default template. Generals, field marshals, fronts and plans need mechanic:command
	// (the same technology, CountryInstance::has_army_command); without it the divisions only take orders by hand.
	//
	// A template is a grid: the ranks (columns, the width of the front) of rows (lines, the depth), a cell holds a regiment type or is
	// empty; every rank also has slots of support companies. How many ranks, rows and slots a country may use is
	// get_template_limits (military/Unlocks.hpp): the defaults and the maximum of ov_war.TEMPLATE_* plus the effects max_columns, max_rows and
	// max_company_slots of its modifiers. What a template is worth: military/DivisionStats.hpp.
	inline constexpr std::size_t DIVISION_MAX_TYPES = 32; // regiment types a cell can hold
	inline constexpr std::size_t DIVISION_MAX_RANKS = 12; // the largest value of ov_war.TEMPLATE_RANKS_MAX
	inline constexpr std::size_t DIVISION_MAX_DEPTH = 8; // the largest value of ov_war.TEMPLATE_DEPTH_MAX
	inline constexpr std::size_t DIVISION_MAX_COMPANY_SLOTS = 8; // the largest value of ov_war.TEMPLATE_COMPANY_SLOTS_MAX
	inline constexpr std::size_t DIVISION_GRID_SIZE = DIVISION_MAX_RANKS * DIVISION_MAX_DEPTH;
	inline constexpr std::uint32_t MAX_DIVISION_TEMPLATES = 24;

	// A cell is at [rank * DIVISION_MAX_DEPTH + row]: the index of the regiment type + 1, 0 for an empty cell
	using DivisionGrid = std::array<std::uint8_t, DIVISION_GRID_SIZE>;
	// A company slot is at [rank * DIVISION_MAX_COMPANY_SLOTS + slot]: the type of the company + 1, 0 for an empty slot (the companies themselves come
	// later: they need their definitions and their cost, see docs/PENDING_PLANS.md)
	using DivisionCompanies = std::array<std::uint8_t, DIVISION_MAX_RANKS * DIVISION_MAX_COMPANY_SLOTS>;

	struct DivisionTemplate {
		std::uint32_t id = 0;
		DivisionGrid cells {};
		DivisionCompanies companies {};
		// The regiments of every type the cells need (made from the cells by rebuild_counts)
		std::array<std::uint32_t, DIVISION_MAX_TYPES> counts {};

		void rebuild_counts() {
			counts.fill(0);
			for (const std::uint8_t cell : cells) {
				if (cell != 0 && cell <= DIVISION_MAX_TYPES) {
					counts[cell - 1]++;
				}
			}
		}
		[[nodiscard]] std::uint32_t get_total_slots() const {
			std::uint32_t total = 0;
			for (const std::uint32_t count : counts) {
				total += count;
			}
			return total;
		}
		[[nodiscard]] static constexpr std::size_t cell_index(const std::size_t rank, const std::size_t row) {
			return rank * DIVISION_MAX_DEPTH + row;
		}
	};

	// A division of an army: the regiments of the army that have its id (RegimentInstance::division_id)
	struct DivisionInstance {
		std::uint32_t id = 0;
		std::uint32_t template_id = 0; // 0: out of the template
	};

	// The kind of the slot a regiment fills: the index of its regiment type (a cell holds a regiment type, so a regiment fills a cell of its own type)
	[[nodiscard]] std::size_t get_division_slot_kind(RegimentType const& regiment_type);

	// The default template: the ranks of ov_data/common/ov_units/default_template.txt (UnitTypeManager::get_default_template_ranks); without the
	// file, four regiments of infantry as 2 ranks of 2 rows and two of cavalry as 1 rank of 2 rows. The grid is empty if the game has no such type.
	[[nodiscard]] DivisionGrid make_standard_division_grid(UnitTypeManager const& unit_type_manager);
	// Whether the grid is a template the country may have: it uses the ranks and the rows the limits give, the types are regiment types, it has a regiment
	[[nodiscard]] bool is_valid_division_grid(InstanceManager const& instance_manager, CountryInstance const& country, DivisionGrid const& grid);

	// Where the regiments of a division stand (docs/WAR_ASSUMPTIONS.md, section 6). The cells of the template are filled line by line (the first
	// line of every rank, then the second, ...): a cell takes a regiment of its type, else one of the same category (infantry, cavalry, support);
	// the regiments in the order of the army. The regiments with no cell stand behind the ranks: the rows after the depth of the template, rank by
	// rank. A division out of the template has no cells: its regiments stand in ranks of extra_depth rows. A regiment with no men left is not in
	// the layout (the next line of its rank takes its place); a rank with no regiment does not count (the width is the ranks that have one).
	struct DivisionLayout {
		struct Entry {
			std::uint32_t regiment = 0; // the index of the regiment in ArmyInstance::get_regiment_instances
			std::uint8_t rank = 0;
			std::uint8_t row = 0;
			std::uint8_t line = 0; // the line it fights in: 0 for the first row of its rank that has a regiment, 1 for the next one, ...
		};
		memory::vector<Entry> entries; // by rank, then by row
		std::uint32_t width = 0;
	};
	[[nodiscard]] DivisionLayout layout_division(
		UnitTypeManager const& unit_type_manager, CountryInstance const& country, ArmyInstance const& army, DivisionInstance const& division,
		std::uint32_t extra_depth
	);

	// Every day (one thread, before the supply is worked out): every regiment gets into a division (new regiments fill the free cells of the
	// divisions of their army, the others wait in the reserve of a country with mechanic:division_templates or make new divisions of the default
	// template). The first time a country is seen it gets the default template and the regiments of its armies are laid out by it. The armies of a
	// country without mechanic:command have no generals.
	void update_all_divisions(InstanceManager& instance_manager);

	// The units a front spreads over its line (docs/PLAN_ARMY_SUPPLY_FRONTS.md, section 5): the divisions of the army
	[[nodiscard]] std::size_t count_division_units(ArmyInstance const& army);
	// Moves the last division out of the army into a new army in the same province (it keeps its id and template). nullptr if the army has fewer
	// than two divisions.
	ArmyInstance* split_division_unit(UnitInstanceManager& unit_manager, ArmyInstance& army);
	// Moves the division into a new army in the same province; nullptr if the army has no other division (the army itself is the division)
	ArmyInstance* split_division(UnitInstanceManager& unit_manager, ArmyInstance& army, std::uint32_t division_id);

	// The commands (false if nothing changed or the command is not allowed; making, changing and deleting templates needs mechanic:division_templates):
	// a new template; changing the cells of a template changes all the divisions made of it (the regiments that do not fit go to the reserve
	// of the army, the free slots are filled from it); deleting a template leaves its divisions out of the template
	bool create_division_template(InstanceManager& instance_manager, CountryInstance& country, DivisionGrid const& grid);
	bool update_division_template(
		InstanceManager& instance_manager, CountryInstance& country, std::uint32_t template_id, DivisionGrid const& grid
	);
	bool delete_division_template(CountryInstance& country, std::uint32_t template_id);
	// The regiments of the divisions out of the template (of one division if division_id is not 0) of the army make as many whole divisions of
	// the template as they can; what is left stays where it was
	bool reform_army_to_template(
		InstanceManager& instance_manager, ArmyInstance& army,
		std::uint32_t division_id, std::uint32_t template_id
	);
}
