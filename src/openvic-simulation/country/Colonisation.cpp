#include "Colonisation.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <queue>
#include <unordered_set>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/defines/CountryDefines.hpp"
#include "openvic-simulation/economy/BuildingInstance.hpp"
#include "openvic-simulation/economy/BuildingType.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/modifier/ModifierEffectCache.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

namespace {
	// Seas that the ports of the country reach
	std::unordered_set<ProvinceDefinition const*> reachable_seas(InstanceManager& instance_manager, CountryInstance& country) {
		std::unordered_set<ProvinceDefinition const*> seas;
		std::queue<std::pair<ProvinceDefinition const*, std::int32_t>> queue;

		for (ProvinceInstance* province : country.get_owned_provinces()) {
			if (province->province_definition.is_water() || !province->province_definition.has_port()
				|| province->province_definition.get_port_adjacent_province() == nullptr) {
				continue;
			}
			std::int32_t level = 0;
			for (BuildingInstance const& building : province->get_buildings()) {
				if (building.building_type.get_identifier() == "naval_base") {
					level = static_cast<std::int32_t>(type_safe::get(building.get_level()));
				}
			}
			// Sea steps from a port of the country that a colonial expedition can cover, per level of the naval base
			OpenVicColonisationDefines const& defines =
				instance_manager.definition_manager.get_define_manager().get_openvic_defines().colonisation;
			const std::int32_t range = defines.sea_range_base + defines.sea_range_per_base_level * level;
			ProvinceDefinition const* const sea = province->province_definition.get_port_adjacent_province();
			if (seas.insert(sea).second) {
				queue.emplace(sea, range);
			}
		}

		while (!queue.empty()) {
			const auto [sea, range] = queue.front();
			queue.pop();
			if (range <= 0) {
				continue;
			}
			for (ProvinceDefinition::adjacency_t const& adjacency : sea->get_adjacencies()) {
				ProvinceDefinition const& to = adjacency.get_to();
				if (to.is_water() && adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::IMPASSABLE && seas.insert(&to).second) {
					queue.emplace(&to, range - 1);
				}
			}
		}
		(void)instance_manager;
		return seas;
	}

	bool touches_country_by_land(CountryInstance const& country, ProvinceInstance const& province, MapInstance& map_instance) {
		for (ProvinceDefinition::adjacency_t const& adjacency : province.province_definition.get_adjacencies()) {
			if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND || adjacency.get_to().is_water()) {
				continue;
			}
			if (map_instance.get_province_instance_by_definition(adjacency.get_to()).get_owner() == &country) {
				return true;
			}
		}
		return false;
	}

	bool touches_seas(ProvinceInstance const& province, std::unordered_set<ProvinceDefinition const*> const& seas) {
		for (ProvinceDefinition::adjacency_t const& adjacency : province.province_definition.get_adjacencies()) {
			if (adjacency.get_to().is_water() && seas.contains(&adjacency.get_to())) {
				return true;
			}
		}
		return false;
	}
}

fixed_point_t ColonisationManager::get_total_points(InstanceManager const& instance_manager, CountryInstance const& country) const {
	ModifierEffectCache const& cache = instance_manager.definition_manager.get_modifier_manager().get_modifier_effect_cache();
	fixed_point_t total = fixed_point_t::_0;
	if (cache.get_colonial_points() != nullptr) {
		total += country.get_modifier_effect_value(*cache.get_colonial_points());
	}
	for (ProvinceInstance const* province : country.get_owned_provinces()) {
		if (province->get_colony_status() != colony_status_t::STATE) {
			continue;
		}
		for (BuildingInstance const& building : province->get_buildings()) {
			auto const& per_level = building.building_type.get_colonial_points();
			const std::int32_t level = static_cast<std::int32_t>(type_safe::get(building.get_level()));
			if (level > 0 && static_cast<size_t>(level) <= per_level.size()) {
				total += per_level[level - 1];
			}
		}
	}
	for (NavyInstance const& navy : country.get_navies()) {
		for (ShipInstance const& ship : navy.get_ship_instances()) {
			total += ship.get_ship_type().colonial_points;
		}
	}
	return total;
}

fixed_point_t ColonisationManager::get_used_points(InstanceManager const& instance_manager, CountryInstance const& country) const {
	fixed_point_t used = fixed_point_t::_0;
	for (Project const& project : projects) {
		if (project.country == &country) {
			used += project.cost;
		}
	}
	// Maintenance of the colonies
	CountryDefines const& defines = instance_manager.definition_manager.get_define_manager().get_country_defines();
	for (ProvinceInstance const* province : country.get_owned_provinces()) {
		if (province->get_colony_status() == colony_status_t::COLONY) {
			used += defines.get_colonization_colony_province_maintenance();
		} else if (province->get_colony_status() == colony_status_t::PROTECTORATE) {
			used += defines.get_colonization_protectorate_province_maintenance();
		}
	}
	return used;
}

ColonisationManager::Project const* ColonisationManager::find_project(CountryInstance const& country, ProvinceInstance const& province) const {
	for (Project const& project : projects) {
		if (project.country == &country && project.province == &province) {
			return &project;
		}
	}
	return nullptr;
}

memory::vector<ProvinceInstance*> ColonisationManager::get_candidates(InstanceManager& instance_manager, CountryInstance& country) const {
	memory::vector<ProvinceInstance*> result;
	const auto seas = reachable_seas(instance_manager, country);
	for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
		if (province.province_definition.is_water() || province.get_owner() != nullptr) {
			continue;
		}
		if (touches_country_by_land(country, province, instance_manager.get_map_instance())
			|| (province.province_definition.is_coastal() && touches_seas(province, seas))) {
			result.push_back(&province);
		}
	}
	return result;
}

memory::string ColonisationManager::get_block_reason(
	InstanceManager& instance_manager, CountryInstance& country, ProvinceInstance& province
) const {
	if (province.province_definition.is_water() || province.get_owner() != nullptr) {
		return "Провинция уже принадлежит стране или это море";
	}
	if (find_project(country, province) != nullptr) {
		return "Колонизация уже идёт";
	}
	if (get_total_points(instance_manager, country) - get_used_points(instance_manager, country) < get_project_cost(instance_manager, country, province)) {
		return "Нет свободных колониальных очков";
	}
	const bool by_land = touches_country_by_land(country, province, instance_manager.get_map_instance());
	if (country.get_total_rank() == 0 || country.get_total_rank() > get_max_colonial_rank(instance_manager)) {
		{
			const std::string text = fmt::format("Колонизировать могут только страны не ниже {}-го места в рейтинге", get_max_colonial_rank(instance_manager));
			return memory::string { text.data(), text.size() };
		}
	}
	if (!by_land && !(province.province_definition.is_coastal() && touches_seas(province, reachable_seas(instance_manager, country)))) {
		return "Провинция вне досягаемости: нужна граница или порт поблизости";
	}
	return {};
}

memory::vector<ProvinceInstance*> ColonisationManager::get_startable_candidates(
	InstanceManager& instance_manager, CountryInstance& country
) const {
	memory::vector<ProvinceInstance*> result;
	// get_block_reason: only countries of a high enough place in the ranking colonise
	if (country.get_total_rank() == 0 || country.get_total_rank() > get_max_colonial_rank(instance_manager)) {
		return result;
	}
	const fixed_point_t free_points = get_total_points(instance_manager, country) - get_used_points(instance_manager, country);
	const auto seas = reachable_seas(instance_manager, country);
	for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
		if (province.province_definition.is_water() || province.get_owner() != nullptr) {
			continue;
		}
		// Reach: a border by land or a port that reaches the seas next to the province (as in get_candidates)
		if (!(touches_country_by_land(country, province, instance_manager.get_map_instance())
			|| (province.province_definition.is_coastal() && touches_seas(province, seas)))) {
			continue;
		}
		if (find_project(country, province) != nullptr) {
			continue;
		}
		if (free_points < get_project_cost(instance_manager, country, province)) {
			continue;
		}
		result.push_back(&province);
	}
	return result;
}

fixed_point_t ColonisationManager::get_project_cost(
	InstanceManager& instance_manager, CountryInstance& country, ProvinceInstance& province
) const {
	CountryDefines const& defines = instance_manager.definition_manager.get_define_manager().get_country_defines();
	fixed_point_t cost = defines.get_colonization_interest_cost_initial();
	if (touches_country_by_land(country, province, instance_manager.get_map_instance())) {
		cost += defines.get_colonization_interest_cost_neighbor_modifier();
	}
	return std::max(cost, fixed_point_t::_1);
}

std::size_t ColonisationManager::get_max_colonial_rank(InstanceManager const& instance_manager) {
	return static_cast<std::size_t>(
		instance_manager.definition_manager.get_define_manager().get_openvic_defines().colonisation.max_colonial_rank
	);
}

bool ColonisationManager::start_project(InstanceManager& instance_manager, CountryInstance& country, ProvinceInstance& province) {
	if (!get_block_reason(instance_manager, country, province).empty()) {
		return false;
	}
	const std::int32_t days = std::max<std::int32_t>(
		instance_manager.definition_manager.get_define_manager().get_openvic_defines().colonisation.min_colonisation_days,
		static_cast<std::int32_t>(instance_manager.definition_manager.get_define_manager().get_country_defines().get_colonization_duration().to_int())
	);
	projects.push_back({ &country, &province, 0, days, get_project_cost(instance_manager, country, province) });
	SPDLOG_INFO("Colonisation started: {} in {} ({} days)", country.get_identifier(), province.get_identifier(), days);
	return true;
}

bool ColonisationManager::cancel_project(CountryInstance const& country, ProvinceInstance const& province) {
	const auto before = projects.size();
	projects.erase(std::remove_if(projects.begin(), projects.end(), [&](Project const& project) -> bool {
		return project.country == &country && project.province == &province;
	}), projects.end());
	return projects.size() != before;
}

void ColonisationManager::tick(InstanceManager& instance_manager) {
	for (size_t i = 0; i < projects.size();) {
		Project& project = projects[i];
		// The province was taken by somebody else
		if (project.province->get_owner() != nullptr) {
			projects.erase(projects.begin() + i);
			continue;
		}
		if (++project.days_done < project.days_total) {
			++i;
			continue;
		}

		CountryInstance& country = *project.country;
		ProvinceInstance& province = *project.province;
		province.set_owner(&country);
		province.set_controller(&country);
		province.set_colony_status(colony_status_t::COLONY);
		SPDLOG_INFO("Colony: {} colonises {}", country.get_identifier(), province.get_identifier());
		instance_manager.get_notification_log().add(instance_manager.get_today(), &country, "colony", province.get_identifier());
		projects.erase(projects.begin() + i);
		// Other projects for the same province end (the loop above sees the owner on the next days)
	}
}
