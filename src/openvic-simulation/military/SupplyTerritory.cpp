#include "SupplyTerritory.hpp"

#include <algorithm>

#include <type_safe/strong_typedef.hpp>

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/country/CountryInstanceManager.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceDefinition.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/military/War.hpp"

using namespace OpenVic;

namespace {
	CountryInstanceManager const* context_countries = nullptr;
	WarManager const* context_wars = nullptr;
}

bool SupplyFriends::contains(CountryInstance const* country) const {
	if (country == nullptr) {
		return false;
	}
	const std::size_t index = type_safe::get(country->index);
	return index < friends.size() && friends[index] != 0;
}

SupplyFriends SupplyFriends::build(
	CountryInstance const& country, CountryInstanceManager const& country_instance_manager, WarManager const& war_manager
) {
	SupplyFriends ret;
	ret.friends.assign(
		static_cast<std::size_t>(type_safe::get(country_instance_manager.get_country_instances().size())), std::uint8_t { 0 }
	);
	ret.friends[type_safe::get(country.index)] = 1;

	for (CountryInstance const& other : country_instance_manager.get_country_instances()) {
		if (&other == &country || !other.exists()) {
			continue;
		}
		// An ally, or a country that gave the passage of the troops to this one (the passage includes the supply)
		if (country.has_alliance_with(other) || country.has_military_access_to(other)) {
			ret.friends[type_safe::get(other.index)] = 1;
		}
	}

	// The countries on the same side in a war (while the war lasts)
	for (War const* war : war_manager.get_wars_of(country)) {
		for (CountryInstance const* member : war->get_side(war->is_attacker(country))) {
			if (member != nullptr) {
				ret.friends[type_safe::get(member->index)] = 1;
			}
		}
	}
	return ret;
}

void SupplyContext::set(CountryInstanceManager const& new_country_instance_manager, WarManager const& new_war_manager) {
	context_countries = &new_country_instance_manager;
	context_wars = &new_war_manager;
}

SupplyFriends SupplyContext::build_friends(CountryInstance const& country) {
	if (context_countries == nullptr || context_wars == nullptr) {
		return {};
	}
	return SupplyFriends::build(country, *context_countries, *context_wars);
}

bool OpenVic::is_supply_held(ProvinceInstance const& province, SupplyFriends const& friends) {
	CountryInstance const* const holder = province.get_controller() != nullptr ? province.get_controller() : province.get_owner();
	return friends.contains(holder);
}

namespace {
	// The numbers of the provinces that the search has seen, without a vector of flags for every call: the stamp of the call
	struct SearchMarks {
		std::vector<std::uint32_t> stamps;
		std::uint32_t stamp = 0;

		void start(const std::size_t size) {
			if (stamps.size() < size) {
				stamps.assign(size, 0);
				stamp = 0;
			}
			if (++stamp == 0) {
				std::fill(stamps.begin(), stamps.end(), 0);
				stamp = 1;
			}
		}
		bool seen(ProvinceInstance const& province) const {
			return stamps[type_safe::get(province.index)] == stamp;
		}
		void mark(ProvinceInstance const& province) {
			stamps[type_safe::get(province.index)] = stamp;
		}
	};
}

SupplyTerritory OpenVic::calculate_supply_territory(
	ProvinceInstance const& province, MapInstance const& map_instance, CountryInstance const& country,
	SupplyFriends const& friends, OpenVicSupplyDefines const& defines
) {
	using kind_t = SupplyTerritory::kind_t;
	SupplyTerritory ret;
	if (is_supply_held(province, friends)) {
		return ret;
	}

	static thread_local SearchMarks marks;
	marks.start(map_instance.get_province_instances().size() + 1);
	marks.mark(province);

	// The search goes by land through the provinces that are not held; the first level that meets held provinces decides
	std::vector<ProvinceInstance const*> frontier { &province };
	std::vector<ProvinceInstance const*> next;
	for (std::int32_t level = 1; level <= 3 && !frontier.empty(); ++level) {
		next.clear();
		std::int32_t held = 0;
		for (ProvinceInstance const* const current : frontier) {
			for (ProvinceDefinition::adjacency_t const& adjacency : current->province_definition.get_adjacencies()) {
				if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND) {
					continue;
				}
				ProvinceInstance const& neighbour = map_instance.get_province_instance_by_definition(adjacency.get_to());
				if (marks.seen(neighbour)) {
					continue;
				}
				marks.mark(neighbour);
				if (is_supply_held(neighbour, friends)) {
					held++;
				} else {
					next.push_back(&neighbour);
				}
			}
		}

		if (held > 0) {
			ret.held_count = held;
			switch (level) {
			case 1:
				if (held >= 2) {
					ret.kind = kind_t::TWO_NEIGHBOURS;
					ret.coefficient = fixed_point_t::_1;
				} else {
					ret.kind = kind_t::ONE_NEIGHBOUR;
					ret.coefficient = defines.supply_neighbour_one.fixed();
					if (country.has_supply_neighbour_technology()) {
						ret.coefficient += defines.supply_neighbour_one_tech.fixed();
					}
				}
				break;
			case 2:
				ret.kind = kind_t::ONE_BETWEEN;
				ret.coefficient = defines.supply_gap1_base.fixed()
					+ defines.supply_gap1_step.fixed() * (std::min(held, defines.supply_gap1_max_count) - 1);
				break;
			default:
				if (defines.supply_gap2_enabled != 0 || country.has_supply_gap2_technology()) {
					ret.kind = kind_t::TWO_BETWEEN;
					ret.coefficient = defines.supply_gap2_base.fixed()
						+ defines.supply_gap2_step.fixed() * (std::min(held, defines.supply_gap2_max_count) - 1);
				} else {
					ret.kind = kind_t::FAR;
					ret.held_count = 0;
					ret.coefficient = defines.supply_far.fixed();
					if (country.has_supply_far_technology()) {
						ret.coefficient += defines.supply_far_tech.fixed();
					}
				}
				break;
			}
			ret.coefficient = std::clamp(ret.coefficient, fixed_point_t::_0, fixed_point_t::_1);
			return ret;
		}
		frontier.swap(next);
	}

	ret.kind = kind_t::FAR;
	ret.coefficient = defines.supply_far.fixed();
	if (country.has_supply_far_technology()) {
		ret.coefficient += defines.supply_far_tech.fixed();
	}
	ret.coefficient = std::clamp(ret.coefficient, fixed_point_t::_0, fixed_point_t::_1);
	return ret;
}
