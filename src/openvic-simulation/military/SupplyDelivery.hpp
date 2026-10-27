#pragma once

#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/military/SupplyPriority.hpp"

namespace OpenVic {
	struct InstanceManager;
	struct OpenVicSupplyDefines;

	// The delivery of the supply to the armies (docs/PLAN_ARMY_SUPPLY_FRONTS.md, section 3), worked out every day before the countries
	// work out what the supplies cost. For every province with armies and every country whose armies stand in it:
	//   the limit is the supply limit of the province for the country (with the coefficient of the land that is not held), the load is
	//   the need of the armies of the country and of its friends in the province, d = limit / load (at most 1); a province that cannot feed
	//   its armies takes the free limit of the neighbours held by the country or its friends with the efficiency SUPPLY_NEIGHBOUR_EFFICIENCY.
	// The delivery of an army: the priority sends more than the need (but not more than 1 / d and not more than the multiplier of the
	// priority); what arrives is the share f = d * sent (at most 1) of the need.

	// How many times more than the need the priority may send
	[[nodiscard]] fixed_point_t get_supply_priority_multiplier(OpenVicSupplyDefines const& defines, supply_priority_t priority);

	// Sent multiplier and arrived share of an army when the province lets the share d of the need through (d in 0..1)
	struct SupplyDeliveryResult {
		fixed_point_t send_multiplier = fixed_point_t::_1; // how many times the need is sent (1..priority multiplier)
		fixed_point_t delivery = fixed_point_t::_1; // the share of the need that arrives (0..1)
	};
	[[nodiscard]] SupplyDeliveryResult calculate_supply_delivery(const fixed_point_t share, const fixed_point_t priority_multiplier);

	// The reserve of supplies of a regiment (in days) after one day. shortfall is the share of the need that did not arrive (0..1),
	// refill the days the reserve may rise if everything arrived (0 if it did not), capacity the days it can hold. deficit is what the
	// reserve could not cover (the share of the need the regiment was without).
	struct SupplyReserveResult {
		fixed_point_t reserve;
		fixed_point_t deficit;
	};
	[[nodiscard]] SupplyReserveResult step_supply_reserve(
		fixed_point_t reserve, const fixed_point_t shortfall, const fixed_point_t refill, const fixed_point_t capacity
	);

	// Works out the delivery of all the armies (one thread, in the order of the countries and the provinces)
	void update_army_supply_delivery(InstanceManager& instance_manager);
}
