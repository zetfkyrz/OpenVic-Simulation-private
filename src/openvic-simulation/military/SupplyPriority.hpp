#pragma once

#include <cstdint>

namespace OpenVic {
	// How much the country sends to an army (docs/PLAN_ARMY_SUPPLY_FRONTS.md, section 3): the standard priority sends what the army needs,
	// the increased one more to make up for what is lost on the way (up to SUPPLY_PRIORITY_INCREASED_MAX times the need), the maximum one up
	// to SUPPLY_PRIORITY_MAX_MAX times
	enum struct supply_priority_t : std::uint8_t { STANDARD = 0, INCREASED = 1, MAXIMUM = 2 };
}
