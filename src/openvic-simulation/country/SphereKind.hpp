#pragma once

#include <cstdint>
#include <string_view>

namespace OpenVic {
	// The kinds of the spheres of influence (docs/PLAN_SPHERES.md); COUNT also means "no sphere" where a country keeps the kind of its sphere
	enum struct sphere_kind_t : std::uint8_t { CUSTOMS_UNION, IMPERIAL, MILITARY, IDEOLOGICAL, ALLIANCE, COUNT };

	std::string_view sphere_kind_identifier(sphere_kind_t kind);
}
