#pragma once

#include <cstdint>
#include <string_view>

namespace OpenVic {
	// The kinds of the spheres of influence (docs/PLAN_SPHERES.md); COUNT also means "no sphere" where a country keeps the kind of its sphere
	enum struct sphere_kind_t : std::uint8_t { CUSTOMS_UNION, IMPERIAL, MILITARY, IDEOLOGICAL, ALLIANCE, COUNT };

	std::string_view sphere_kind_identifier(sphere_kind_t kind);

	// What a principle of a sphere does (version 94, docs/PLAN_SPHERES.md; the principles themselves are data: ov_data/common/ov_sphere_principles):
	// the output of some goods, the research, the growth of the literacy, the freight between the members, the interest of the loans between the
	// members, the growth of the militancy (less)
	enum struct principle_effect_t : std::uint8_t { OUTPUT, RESEARCH, LITERACY, FREIGHT, LOAN_INTEREST, MILITANCY, COUNT };

	std::string_view principle_effect_identifier(principle_effect_t effect);
}
