#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "openvic-simulation/types/UnitBranchType.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;

	// Divisions and the templates of the divisions (docs/PLAN_ARMY_SUPPLY_FRONTS.md, section 4).
	//
	// A country that has the technology of the organisation of the army (CountryInstance::has_army_organisation, the civilised countries) has
	// armies that consist of divisions and the divisions of regiments. A division has a template (the number of regiments of every kind it
	// needs) or none: "out of the template", like all the regiments of the start of the game, which are not touched. The regiments that
	// are not in a division of a template wait in a division out of the template (the reserve of the army). A country without the technology
	// has groups of regiments only, that are merged and split like in Victoria 2 (no divisions, no templates, no generals).
	inline constexpr std::size_t DIVISION_SLOT_KINDS = 4; // infantry, cavalry, support, special
	inline constexpr std::uint32_t MAX_DIVISION_TEMPLATES = 24;
	inline constexpr std::uint32_t MAX_DIVISION_SLOTS = 40; // regiments of a division

	struct DivisionTemplate {
		std::uint32_t id = 0;
		std::array<std::uint32_t, DIVISION_SLOT_KINDS> slots {};

		[[nodiscard]] std::uint32_t get_total_slots() const {
			std::uint32_t total = 0;
			for (const std::uint32_t slot : slots) {
				total += slot;
			}
			return total;
		}
	};

	// A division of an army: the regiments of the army that have its id (RegimentInstance::division_id)
	struct DivisionInstance {
		std::uint32_t id = 0;
		std::uint32_t template_id = 0; // 0: out of the template
	};

	// The kind of the slot a regiment fills: 0 infantry, 1 cavalry, 2 support, 3 special (artillery, tanks, ...)
	[[nodiscard]] std::size_t get_division_slot_kind(RegimentType const& regiment_type);

	// Every day (one thread, before the supply is worked out): the armies of the countries with the technology get their regiments into
	// divisions (new regiments fill the free slots of the divisions of the templates or wait in the reserve), the armies of the countries
	// without it have no divisions and no generals; a country that has the technology for the first time gets the standard template
	void update_all_divisions(InstanceManager& instance_manager);

	// The commands (false if nothing changed or the command is not allowed):
	// a new template; changing the slots of a template changes all the divisions made of it (the regiments that do not fit go to the reserve
	// of the army, the free slots are filled from it); deleting a template leaves its divisions out of the template
	bool create_division_template(CountryInstance& country, std::array<std::uint32_t, DIVISION_SLOT_KINDS> const& slots);
	bool update_division_template(
		InstanceManager& instance_manager, CountryInstance& country, std::uint32_t template_id,
		std::array<std::uint32_t, DIVISION_SLOT_KINDS> const& slots
	);
	bool delete_division_template(CountryInstance& country, std::uint32_t template_id);
	// The regiments of the divisions out of the template (of one division if division_id is not 0) of the army make as many whole divisions of
	// the template as they can; what is left stays where it was
	bool reform_army_to_template(
		InstanceManager& instance_manager, ArmyInstance& army,
		std::uint32_t division_id, std::uint32_t template_id
	);
}
