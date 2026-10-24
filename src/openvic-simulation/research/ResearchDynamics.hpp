#pragma once

#include <cstdint>
#include <unordered_map>

#include "openvic-simulation/core/object/FixedPoint.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;

	/* Inventions (E1), once a month. The Victoria 2 scripts of the inventions (limit, chance) cannot be evaluated by the
	 * simulation yet, so an invention is discovered by a simple rule: it becomes possible when a technology it is
	 * associated with is researched, and then progresses each month by a rate that grows with the literacy of the country
	 * (about 1.5 years for a country with 50% literacy). New technologies and inventions are written to the notification
	 * log for the interface. */
	namespace ResearchDynamics {
		// What the dynamics remember between the months. It belongs to the game instance (not to the process), so that
		// two games one after another (a loaded save game, the tests) start from the same state.
		struct State {
			// Progress toward each invention of each country, and the number of technologies seen last time
			std::unordered_map<std::uint64_t, fixed_point_t> progress;
			std::unordered_map<CountryInstance const*, std::int32_t> known_technologies;
		};

		// The rates are in the group ov_research of common/openvic_defines.lua
		void monthly_update(InstanceManager& instance_manager);
	}
}
