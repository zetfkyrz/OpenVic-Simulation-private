#include "SupplyDelivery.hpp"

#include <algorithm>
#include <vector>

#include <type_safe/strong_typedef.hpp>

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/country/CountryInstanceManager.hpp"
#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceDefinition.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/misc/NotificationLog.hpp"
#include "openvic-simulation/military/ArmyGroup.hpp"
#include "openvic-simulation/military/Leader.hpp"
#include "openvic-simulation/military/SupplyTerritory.hpp"
#include "openvic-simulation/modifier/ModifierEffectCache.hpp"
#include "openvic-simulation/modifier/ModifierManager.hpp"
#include "openvic-simulation/military/UnitInstance.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/military/UnitType.hpp"

using namespace OpenVic;

fixed_point_t OpenVic::get_supply_priority_multiplier(OpenVicSupplyDefines const& defines, const supply_priority_t priority) {
	switch (priority) {
	case supply_priority_t::INCREASED: return std::max(fixed_point_t::_1, defines.supply_priority_increased_max);
	case supply_priority_t::MAXIMUM: return std::max(fixed_point_t::_1, defines.supply_priority_max_max);
	default: return fixed_point_t::_1;
	}
}

SupplyDeliveryResult OpenVic::calculate_supply_delivery(const fixed_point_t share, const fixed_point_t priority_multiplier) {
	SupplyDeliveryResult ret;
	const fixed_point_t d = std::clamp(share, fixed_point_t::_0, fixed_point_t::_1);
	const fixed_point_t multiplier = std::max(fixed_point_t::_1, priority_multiplier);
	if (d <= fixed_point_t::_0) {
		// Nothing gets through: nothing is sent either (the goods would be lost)
		ret.send_multiplier = fixed_point_t::_1;
		ret.delivery = fixed_point_t::_0;
		return ret;
	}
	// More than the need is sent to make up for the losses, but not more than makes up for all of them and not more than the priority allows
	if (multiplier * d >= fixed_point_t::_1) {
		// The priority is enough to make up for all the losses: exactly the need arrives
		ret.send_multiplier = std::min(multiplier, fixed_point_t::_1 / d);
		ret.delivery = fixed_point_t::_1;
	} else {
		ret.send_multiplier = multiplier;
		ret.delivery = d * multiplier;
	}
	return ret;
}

SupplyReserveResult OpenVic::step_supply_reserve(
	fixed_point_t reserve, const fixed_point_t shortfall, const fixed_point_t refill, const fixed_point_t capacity
) {
	SupplyReserveResult ret;
	const fixed_point_t cap = std::max(fixed_point_t::_0, capacity);
	reserve = std::clamp(reserve, fixed_point_t::_0, cap);
	const fixed_point_t lost = std::clamp(shortfall, fixed_point_t::_0, fixed_point_t::_1);
	if (lost > fixed_point_t::_0) {
		if (reserve >= lost) {
			reserve -= lost;
		} else {
			ret.deficit = lost - reserve;
			reserve = fixed_point_t::_0;
		}
	} else {
		reserve = std::min(cap, reserve + std::max(fixed_point_t::_0, refill));
	}
	ret.reserve = reserve;
	return ret;
}

namespace {
	// The need of an army: the supply consumption of its regiments
	fixed_point_t army_need(ArmyInstance const& army) {
		fixed_point_t need = fixed_point_t::_0;
		for (RegimentInstance const& regiment : army.get_regiment_instances()) {
			need += regiment.unit_type.supply_consumption;
		}
		return need;
	}

	// The need of the armies of the friends of a country in a province
	fixed_point_t friends_load(ProvinceInstance const& province, SupplyFriends const& friends) {
		fixed_point_t load = fixed_point_t::_0;
		for (ArmyInstance const& army : province.get_armies()) {
			if (friends.contains(&army.get_country())) {
				load += army_need(army);
			}
		}
		return load;
	}
}

void OpenVic::update_army_supply_delivery(InstanceManager& instance_manager) {
	OpenVicSupplyDefines const& defines = instance_manager.definition_manager.get_define_manager().get_openvic_defines().supply;
	MapInstance const& map = instance_manager.get_map_instance();
	CountryInstanceManager& countries = instance_manager.get_country_instance_manager();
	const fixed_point_t efficiency = defines.supply_neighbour_efficiency.fixed();
	const fixed_point_t alert_level = defines.supply_alert_level.fixed();
	ModifierEffectCache const& modifier_effect_cache = instance_manager.definition_manager.get_modifier_manager().get_modifier_effect_cache();

	// How much of the free limit of a province was taken by the neighbours in the work of this country (reset after every country)
	std::vector<fixed_point_t> lent(map.get_province_instances().size() + 1, fixed_point_t::_0);
	std::vector<std::size_t> lent_provinces;
	std::vector<ProvinceInstance*> provinces;

	for (CountryInstance& country : countries.get_country_instances()) {
		if (!country.has_armies()) {
			continue;
		}

		// An army that is not on land (a fleet carries it) has all it needs; the others are set below
		provinces.clear();
		for (ArmyInstance& army : country.get_armies()) {
			// The message when the supply level of an army goes below the level of the alert, and when it is above again
			const bool low = army.get_unit_count() > 0 && army.get_supply_level() < alert_level;
			if (low != army.is_supply_alert()) {
				army.set_supply_alert(low);
				instance_manager.get_notification_log().add(
					instance_manager.get_today(), &country, low ? "army_no_supply" : "army_supply_restored", army.get_name()
				);
			}
			army.set_supply_delivery(fixed_point_t::_1);
			army.set_supply_send_multiplier(fixed_point_t::_1);
			ProvinceInstance& location = army.get_location();
			if (!location.province_definition.is_water() && army.get_unit_count() > 0) {
				provinces.push_back(&location);
			}
		}
		if (provinces.empty()) {
			continue;
		}
		std::sort(provinces.begin(), provinces.end(), [](ProvinceInstance const* a, ProvinceInstance const* b) {
			return type_safe::get(a->index) < type_safe::get(b->index);
		});
		provinces.erase(std::unique(provinces.begin(), provinces.end()), provinces.end());

		const SupplyFriends friends = SupplyFriends::build(country, countries, instance_manager.get_war_manager());

		for (ProvinceInstance* const province : provinces) {
			const SupplyTerritory territory = calculate_supply_territory(*province, map, country, friends, defines);
			const ProvinceInstance::supply_breakdown_t supply = province->get_supply_breakdown(&country, territory);
			const fixed_point_t load = friends_load(*province, friends);
			fixed_point_t limit = supply.capacity;

			// A province that cannot feed what stands in it takes the free limit of the neighbours that are held by the country or its friends
			if (load > limit && efficiency > fixed_point_t::_0) {
				fixed_point_t missing = load - limit;
				for (ProvinceDefinition::adjacency_t const& adjacency : province->province_definition.get_adjacencies()) {
					if (missing <= fixed_point_t::_0) {
						break;
					}
					if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND) {
						continue;
					}
					ProvinceInstance const& neighbour = map.get_province_instance_by_definition(adjacency.get_to());
					if (!is_supply_held(neighbour, friends)) {
						continue;
					}
					const std::size_t neighbour_index = type_safe::get(neighbour.index);
					const fixed_point_t neighbour_limit = neighbour.get_supply_breakdown(&country, SupplyTerritory {}).capacity;
					const fixed_point_t free = neighbour_limit - friends_load(neighbour, friends) - lent[neighbour_index];
					if (free <= fixed_point_t::_0) {
						continue;
					}
					// What is taken gives only the efficiency of it to the province
					const fixed_point_t taken = std::min(free, missing / efficiency);
					if (lent[neighbour_index] == fixed_point_t::_0) {
						lent_provinces.push_back(neighbour_index);
					}
					lent[neighbour_index] += taken;
					limit += taken * efficiency;
					missing -= taken * efficiency;
				}
			}

			const fixed_point_t share = load <= fixed_point_t::_0
				? fixed_point_t::_1
				: std::clamp(limit / load, fixed_point_t::_0, fixed_point_t::_1);
			for (ArmyInstance& army : province->get_armies()) {
				if (&army.get_country() != &country || army.get_unit_count() == 0) {
					continue;
				}
				const supply_priority_t priority = country.get_effective_supply_priority(
					army.is_supply_priority_custom(), army.get_supply_priority()
				);
				// The third factor: the bonus of the general and of the field marshal of the group less the temporary debuffs (not on the territory
				// coefficient: only the technologies change that)
				fixed_point_t leader_bonus = fixed_point_t::_0;
				if (army.get_leader() != nullptr) {
					leader_bonus += calculate_leader_supply_bonus(*army.get_leader(), modifier_effect_cache, defines);
				}
				if (army.get_army_group_id() != 0) {
					ArmyGroup const* const army_group = country.find_army_group(army.get_army_group_id());
					LeaderInstance const* const marshal = army_group != nullptr && army_group->marshal_id != 0
						? instance_manager.get_unit_instance_manager().get_leader_instance_by_unique_id(army_group->marshal_id) : nullptr;
					if (marshal != nullptr) {
						leader_bonus += calculate_leader_supply_bonus(*marshal, modifier_effect_cache, defines) * defines.supply_marshal_share.fixed();
					}
				}
				army.set_supply_leader_bonus(leader_bonus);
				const fixed_point_t third_factor = std::max(fixed_point_t::_0, fixed_point_t::_1 + leader_bonus - calculate_temporary_supply_debuff(country));
				const SupplyDeliveryResult result = calculate_supply_delivery(
					std::clamp(share * third_factor, fixed_point_t::_0, fixed_point_t::_1), get_supply_priority_multiplier(defines, priority)
				);
				army.set_supply_send_multiplier(result.send_multiplier);
				army.set_supply_delivery(result.delivery);
			}
		}

		for (const std::size_t index : lent_provinces) {
			lent[index] = fixed_point_t::_0;
		}
		lent_provinces.clear();
	}
}
