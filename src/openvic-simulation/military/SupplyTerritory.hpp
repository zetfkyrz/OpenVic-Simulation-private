#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct CountryInstanceManager;
	struct MapInstance;
	struct ProvinceInstance;
	struct WarManager;

	// Supply of the armies away from the own land (docs/PLAN_ARMY_SUPPLY_FRONTS.md, section 2).
	//
	// "Own" land for the armies of a country: the provinces held (controlled) by the country itself or by a friend, and a friend is the
	// country, an ally, a country that is on the same side in a war and a country that gave the country the passage of its troops (the
	// passage includes the supply). The supply coefficient of an army that stands on land that is not held by a friend depends on how
	// far the nearest provinces that are held by a friend are (by land, through the provinces that are not):
	//   a neighbour is held by a friend: 90 percent (a technology: 100), two or more neighbours: 100 percent,
	//   none, but one province between: 50 percent for the first held province that touches the provinces between and +10 percent for
	//   each next one (at most 4 are counted: up to 80), two provinces between: 25 percent +5 percent for each next one (at most 5: up
	//   to 45), only when the technology opens it, further or not opened: 10 percent (a technology: 20).
	// The supply does not go through the provinces that an enemy controls: the search for the provinces between stops at them. (Before the change of the
	// task 2026-10-30 it went through them, as through the provinces of nobody.)
	// Only technologies change these numbers (the generals do not: they work in the last factor of the supply limit).

	// The countries whose land is own land for the supply of the armies of one country
	struct SupplyFriends {
		std::vector<std::uint8_t> friends; // by the index of the country, 1 = friend (the country itself too)

		[[nodiscard]] bool contains(CountryInstance const* country) const;
		[[nodiscard]] static SupplyFriends build(
			CountryInstance const& country, CountryInstanceManager const& country_instance_manager, WarManager const& war_manager
		);
	};

	// The wiring of the calculation to the instance (the pointers are set by the InstanceManager when it is created, the instance
	// outlives everything that asks)
	struct SupplyContext {
		static void set(CountryInstanceManager const& new_country_instance_manager, WarManager const& new_war_manager);
		// Friends of a country built from the current state of the diplomacy and the wars
		[[nodiscard]] static SupplyFriends build_friends(CountryInstance const& country);
	};

	struct SupplyTerritory {
		enum struct kind_t : std::uint8_t {
			HELD,           // the province is held by the country or a friend: 100 percent
			TWO_NEIGHBOURS, // two or more neighbours are held by friends: 100 percent
			ONE_NEIGHBOUR,  // one neighbour is held
			ONE_BETWEEN,    // one province that is not held lies between
			TWO_BETWEEN,    // two provinces that are not held lie between (only with the technology)
			FAR             // no held province near enough
		};

		kind_t kind = kind_t::HELD;
		fixed_point_t coefficient = fixed_point_t::_1; // 0..1
		std::int32_t held_count = 0; // the held provinces that were counted for the coefficient
	};

	// A province controlled (the owner if nobody has taken it) by a country that is at war with the country: the supply of the country does not go through it
	[[nodiscard]] bool is_enemy_controlled(ProvinceInstance const& province, CountryInstance const& country);
	// A province is held by a friend (the one who controls it: the owner if nobody has taken it)
	[[nodiscard]] bool is_supply_held(ProvinceInstance const& province, SupplyFriends const& friends);

	// The supply coefficient of the armies of the country in the province
	[[nodiscard]] SupplyTerritory calculate_supply_territory(
		ProvinceInstance const& province, MapInstance const& map_instance, CountryInstance const& country,
		SupplyFriends const& friends, OpenVicSupplyDefines const& defines
	);
}
