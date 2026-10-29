#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "openvic-simulation/types/UnitBranchType.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;
	struct UnitInstanceManager;
	struct UnitTypeManager;

	// Divisions and the templates of the divisions (docs/PLAN_ARMY_SUPPLY_FRONTS.md, sections 4 and 4b).
	//
	// A country that has the technology of the organisation of the army (CountryInstance::has_army_organisation, the civilised countries) has
	// armies that consist of divisions and the divisions of regiments. A division has a template or none: "out of the template", like all the
	// regiments of the start of the game, which are not touched. The regiments that are not in a division of a template wait in a division
	// out of the template (the reserve of the army). A country without the technology has groups of regiments only, that are merged and
	// split like in Victoria 2 (no divisions, no templates, no generals).
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

	// The standard template: four regiments of infantry as 2 ranks of 2 rows and two of cavalry as 1 rank of 2 rows (3 ranks, 2 rows). The types
	// are the ones named "infantry" and "cavalry", or the first of the category. The grid is empty if the game has no such type.
	[[nodiscard]] DivisionGrid make_standard_division_grid(UnitTypeManager const& unit_type_manager);
	// Whether the grid is a template the country may have: it uses the ranks and the rows the limits give, the types are regiment types, it has a regiment
	[[nodiscard]] bool is_valid_division_grid(InstanceManager const& instance_manager, CountryInstance const& country, DivisionGrid const& grid);

	// Every day (one thread, before the supply is worked out): the armies of the countries with the technology get their regiments into
	// divisions (new regiments fill the free slots of the divisions of the templates or wait in the reserve), the armies of the countries
	// without it have no divisions and no generals; a country that has the technology for the first time gets the standard template
	void update_all_divisions(InstanceManager& instance_manager);

	// The units a front spreads over its line (docs/PLAN_ARMY_SUPPLY_FRONTS.md, section 5): a division of a template is one unit, the regiments out
	// of the templates are counted in groups of chunk_size
	[[nodiscard]] std::size_t count_division_units(ArmyInstance const& army, std::uint32_t chunk_size);
	// Moves one unit (a division of a template, else chunk_size regiments out of the templates) out of the army into a new army in the same
	// province; a division keeps its id and template. nullptr if the army has fewer than two units.
	ArmyInstance* split_division_unit(UnitInstanceManager& unit_manager, ArmyInstance& army, std::uint32_t chunk_size);

	// The commands (false if nothing changed or the command is not allowed):
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
