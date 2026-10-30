#pragma once

#include <cstdint>

#include "openvic-simulation/core/memory/String.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/military/Division.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;

	struct DivisionGoodAmount {
		memory::string good; // the identifier of the good
		fixed_point_t amount;
	};

	// What a division template is worth (docs/PLAN_ARMY_SUPPLY_FRONTS.md, 4b). The same function gives the numbers of the template window (the
	// weapons the country can have of the highest generation, fill = 1) and of a division in battle (the weapons it really has, fill < 1).
	struct DivisionStats {
		// Base
		fixed_point_t speed; // the slowest regiment
		fixed_point_t weight; // kilograms: the men with their kit and the weapons (the ships carry it)
		fixed_point_t durability; // how long it lasts: all the cells count, so depth makes it last
		fixed_point_t organisation; // the average of the regiments
		fixed_point_t reconnaissance;
		fixed_point_t occupation_speed; // 1 is the base; technologies add to it
		bool occupation_instant = false;
		// Combat
		fixed_point_t attack; // the possible damage; the cells of the rows weigh 1, 1/2, 1/4, ...
		fixed_point_t defence; // is counted the same way
		fixed_point_t breakthrough; // the third parameter of the battle: how fast the attacker breaks a defence (counted the same way)
		std::uint32_t width = 0; // ranks that have a regiment
		fixed_point_t depth; // regiments per rank, the average (1.5 for ranks of 1 and 2)
		fixed_point_t density; // strength points per rank, less by the density_reduction
		fixed_point_t fortification;
		fixed_point_t reliability; // the share of the best weapons the division really has (1 for a template)
		// The support companies (military/Companies.hpp): their number and what they add to the attack, the defence, the breakthrough (shares) and the
		// fortification (levels) of a regiment that counts in the scope all; the buff of a regiment type is worked out in its own scopes
		std::uint32_t companies = 0;
		fixed_point_t company_attack, company_defence, company_breakthrough, company_fortification;
		// Cost
		std::uint32_t regiments = 0;
		fixed_point_t men;
		memory::vector<DivisionGoodAmount> cost; // goods of the construction of all the regiments, with the weapons of the best generation
		memory::vector<DivisionGoodAmount> supply_per_day; // goods the regiments use in a day on the move or in camp (weapons, artillery too)
		memory::vector<DivisionGoodAmount> ammo_per_day; // ammunition the weapons use in a day of battle, by good
	};

	[[nodiscard]] DivisionStats compute_division_stats(
		InstanceManager const& instance_manager, CountryInstance const& country, DivisionTemplate const& division_template,
		fixed_point_t equipment_fill = fixed_point_t::_1
	);
}
