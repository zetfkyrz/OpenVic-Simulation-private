// Fronts and the automatic war plan, part of WarManager
#include <algorithm>

#include "War.hpp"

#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/military/Division.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

namespace {
	// Land provinces next to the province
	std::vector<ProvinceInstance*> get_land_neighbours(ProvinceInstance const& province, MapInstance& map_instance) {
		std::vector<ProvinceInstance*> result;
		for (ProvinceDefinition::adjacency_t const& adjacency : province.province_definition.get_adjacencies()) {
			if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND || adjacency.get_to().is_water()) {
				continue;
			}
			result.push_back(&map_instance.get_province_instance_by_definition(adjacency.get_to()));
		}
		return result;
	}

	// Enemy held provinces next to the line, the targets of the attack (empty in the DEFEND mode)
	std::vector<ProvinceInstance*> get_front_targets(Front const& front, MapInstance& map_instance) {
		std::vector<ProvinceInstance*> targets;
		if (front.mode != Front::mode_t::ATTACK) {
			return targets;
		}
		for (ProvinceInstance* line_province : front.provinces) {
			for (ProvinceInstance* neighbour : get_land_neighbours(*line_province, map_instance)) {
				CountryInstance const* const controller = neighbour->get_controller();
				if (controller != nullptr && front.country->is_at_war_with(*controller)
					&& std::find(targets.begin(), targets.end(), neighbour) == targets.end()) {
					targets.push_back(neighbour);
				}
			}
		}
		return targets;
	}

	bool is_held_by(ProvinceInstance const& province, CountryInstance const& country) {
		return province.get_owner() == &country || province.get_controller() == &country;
	}

	// The provinces of the country that touch land held by the enemy, ordered so that neighbours follow each other
	memory::vector<ProvinceInstance*> build_border_line(
		MapInstance& map_instance, CountryInstance& country, CountryInstance const& enemy
	) {
		std::vector<ProvinceInstance*> border;
		for (ProvinceInstance* province : country.get_owned_provinces()) {
			if (province->get_controller() != &country || province->province_definition.is_water()) {
				continue;
			}
			for (ProvinceInstance* neighbour : get_land_neighbours(*province, map_instance)) {
				if (is_held_by(*neighbour, enemy)) {
					border.push_back(province);
					break;
				}
			}
		}

		memory::vector<ProvinceInstance*> line;
		std::vector<bool> used(border.size(), false);
		ProvinceInstance* current = nullptr;

		for (size_t placed = 0; placed < border.size(); ++placed) {
			size_t next = border.size();

			if (current != nullptr) {
				// Prefer an unplaced border province next to the last one
				for (ProvinceInstance* neighbour : get_land_neighbours(*current, map_instance)) {
					for (size_t i = 0; i < border.size(); ++i) {
						if (!used[i] && border[i] == neighbour) {
							next = i;
							break;
						}
					}
					if (next != border.size()) {
						break;
					}
				}
			}
			if (next == border.size()) {
				// Start of a new stretch of the border
				for (size_t i = 0; i < border.size(); ++i) {
					if (!used[i]) {
						next = i;
						break;
					}
				}
			}

			used[next] = true;
			current = border[next];
			line.push_back(current);
		}

		return line;
	}
}

Front* WarManager::find_front(const std::uint32_t id) {
	for (Front& front : fronts) {
		if (front.id == id) {
			return &front;
		}
	}
	return nullptr;
}

Front const* WarManager::get_front_by_id(const std::uint32_t id) const {
	for (Front const& front : fronts) {
		if (front.id == id) {
			return &front;
		}
	}
	return nullptr;
}

std::uint32_t WarManager::create_front(
	InstanceManager& instance_manager, CountryInstance& country, CountryInstance const* const enemy
) {
	if (!country.exists()) {
		return 0;
	}

	Front front;
	front.id = next_front_id++;
	front.country = &country;
	front.enemy = enemy;
	if (enemy != nullptr && enemy != &country) {
		front.provinces = build_border_line(instance_manager.get_map_instance(), country, *enemy);
		front.mode = Front::mode_t::ATTACK;
	} else {
		front.enemy = nullptr;
	}

	fronts.push_back(std::move(front));
	return fronts.back().id;
}

bool WarManager::delete_front(CountryInstance const& country, const std::uint32_t front_id) {
	const auto it = std::find_if(fronts.begin(), fronts.end(), [&](Front const& front) -> bool {
		return front.id == front_id && front.country == &country;
	});
	if (it == fronts.end()) {
		return false;
	}
	fronts.erase(it);
	return true;
}

bool WarManager::toggle_front_province(CountryInstance const& country, const std::uint32_t front_id, ProvinceInstance& province) {
	Front* const front = find_front(front_id);
	if (front == nullptr || front->country != &country || province.province_definition.is_water()) {
		return false;
	}

	const auto it = std::find(front->provinces.begin(), front->provinces.end(), &province);
	if (it != front->provinces.end()) {
		front->provinces.erase(it);
		return true;
	}

	// Only land the country holds can be part of the line
	if (province.get_controller() != &country) {
		return false;
	}
	front->provinces.push_back(&province);
	// A hand drawn line is no longer the generated border
	front->enemy = nullptr;
	return true;
}

bool WarManager::set_front_mode(CountryInstance const& country, const std::uint32_t front_id, const Front::mode_t mode) {
	Front* const front = find_front(front_id);
	if (front == nullptr || front->country != &country) {
		return false;
	}
	front->mode = mode;
	return true;
}

bool WarManager::set_front_army(
	InstanceManager& instance_manager, CountryInstance const& country, const std::uint32_t front_id, const unique_id_t army_id,
	const bool assigned
) {
	Front* const front = find_front(front_id);
	if (front == nullptr || front->country != &country) {
		return false;
	}

	UnitInstanceGroup* const group = instance_manager.get_unit_instance_manager().get_unit_instance_group_by_unique_id(army_id);
	if (group == nullptr || group->branch != unit_branch_t::LAND || &group->get_country() != &country) {
		return false;
	}

	if (assigned) {
		// An army is on one front at a time
		for (Front& other : fronts) {
			std::erase(other.army_ids, army_id);
		}
		front->army_ids.push_back(army_id);
	} else {
		std::erase(front->army_ids, army_id);
	}
	return true;
}

bool WarManager::assign_free_armies(
	InstanceManager& instance_manager, CountryInstance const& country, const std::uint32_t front_id
) {
	Front* const front = find_front(front_id);
	if (front == nullptr || front->country != &country) {
		return false;
	}

	for (ArmyInstance const& army : country.get_armies()) {
		bool on_front = false;
		for (Front const& other : fronts) {
			if (std::find(other.army_ids.begin(), other.army_ids.end(), army.unique_id) != other.army_ids.end()) {
				on_front = true;
				break;
			}
		}
		if (!on_front) {
			front->army_ids.push_back(army.unique_id);
		}
	}
	return true;
}

void WarManager::set_auto_plan(CountryInstance const& country, const bool enabled) {
	if (enabled) {
		auto_plan_countries.insert(&country);
	} else {
		auto_plan_countries.erase(&country);
	}
}

void WarManager::order_front_armies(InstanceManager& instance_manager, Front& front) {
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	MapInstance& map_instance = instance_manager.get_map_instance();
	CountryInstance& country = *front.country;

	// Drop armies that no longer exist
	std::vector<ArmyInstance*> armies;
	for (auto it = front.army_ids.begin(); it != front.army_ids.end();) {
		UnitInstanceGroup* const group = unit_manager.get_unit_instance_group_by_unique_id(*it);
		if (group == nullptr || group->branch != unit_branch_t::LAND || &group->get_country() != &country) {
			it = front.army_ids.erase(it);
		} else {
			armies.push_back(static_cast<ArmyInstance*>(group));
			++it;
		}
	}
	if (armies.empty() || front.provinces.empty()) {
		return;
	}

	std::vector<ProvinceInstance*> const targets = get_front_targets(front, map_instance);

	for (size_t i = 0; i < armies.size(); ++i) {
		ArmyInstance& army = *armies[i];
		if (army.is_in_combat() || army.is_moving() || army.empty()) {
			continue;
		}

		ProvinceInstance* destination = nullptr;

		if (!targets.empty()) {
			// An army standing in enemy land stays to besiege it
			CountryInstance const* const here = army.get_location().get_controller();
			if (here != nullptr && country.is_at_war_with(*here)) {
				continue;
			}
			destination = targets[i % targets.size()];
		} else {
			destination = front.provinces[i % front.provinces.size()];
		}

		if (destination != nullptr && destination != &army.get_location()) {
			unit_manager.order_move(army, *destination, map_instance);
		}
	}
}

void WarManager::spread_front_divisions(InstanceManager& instance_manager, Front& front) {
	CountryInstance& country = *front.country;
	OpenVicWarDefines const& defines = get_war_defines();
	if (!country.has_army_organisation() || front.provinces.empty() || defines.front_spread_max_splits_per_day <= 0) {
		return;
	}
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	const std::vector<ProvinceInstance*> targets = get_front_targets(front, instance_manager.get_map_instance());
	const size_t sectors = !targets.empty() ? targets.size() : front.provinces.size();

	std::vector<ArmyInstance*> armies;
	for (const unique_id_t army_id : front.army_ids) {
		UnitInstanceGroup* const group = unit_manager.get_unit_instance_group_by_unique_id(army_id);
		if (group != nullptr && group->branch == unit_branch_t::LAND && &group->get_country() == &country) {
			armies.push_back(static_cast<ArmyInstance*>(group));
		}
	}

	for (std::int32_t splits = 0; armies.size() < sectors && splits < defines.front_spread_max_splits_per_day; ++splits) {
		ArmyInstance* biggest = nullptr;
		size_t biggest_units = 1;
		for (ArmyInstance* army : armies) {
			if (army->is_in_combat() || army->is_moving() || army->empty()) {
				continue;
			}
			const size_t units = count_division_units(*army, defines.front_spread_chunk_size);
			if (units > biggest_units) {
				biggest = army;
				biggest_units = units;
			}
		}
		if (biggest == nullptr) {
			break;
		}
		ArmyInstance* const split_army = split_division_unit(unit_manager, *biggest, defines.front_spread_chunk_size);
		if (split_army == nullptr) {
			break;
		}
		front.army_ids.push_back(split_army->unique_id);
		armies.push_back(split_army);
	}
}

void WarManager::update_auto_plan(InstanceManager& instance_manager) {
	// Fronts are refreshed every 10 days
	if (instance_manager.get_today().get_day() % 10 != 0) {
		return;
	}

	// Countries with the plan switched on, and every AI country that is at war (a minimal war AI: the same plan)
	std::vector<CountryInstance*> planning_countries;
	for (CountryInstance& candidate : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (auto_plan_countries.contains(&candidate)
			|| (candidate.is_ai() && candidate.is_at_war() && !candidate.is_rebel_country())) {
			planning_countries.push_back(&candidate);
		}
	}

	for (CountryInstance* const planning_country : planning_countries) {
		CountryInstance& country = *planning_country;
		if (!country.exists()) {
			continue;
		}

		// Remove the generated fronts of enemies that are gone
		std::erase_if(fronts, [&country](Front const& front) -> bool {
			return front.country == &country && front.enemy != nullptr && !country.is_at_war_with(*front.enemy);
		});

		std::vector<std::uint32_t> own_fronts; // ids, the fronts vector may reallocate
		for (CountryInstance const& enemy : country.get_war_enemies()) {
			Front* front = nullptr;
			for (Front& candidate : fronts) {
				if (candidate.country == &country && candidate.enemy == &enemy) {
					front = &candidate;
					break;
				}
			}

			memory::vector<ProvinceInstance*> line = build_border_line(instance_manager.get_map_instance(), country, enemy);
			if (line.empty()) {
				continue;
			}

			if (front == nullptr) {
				const std::uint32_t id = create_front(instance_manager, country, &enemy);
				front = find_front(id);
				if (front == nullptr) {
					continue;
				}
			} else {
				front->provinces = std::move(line);
			}

			// Attack unless the war is going clearly badly
			front->mode = Front::mode_t::ATTACK;
			if (War* const war = find_war_between(country, enemy); war != nullptr) {
				const bool is_attacker = war->is_attacker(country);
				if (war->get_score(!is_attacker) - war->get_score(is_attacker) > 20) {
					front->mode = Front::mode_t::DEFEND;
				}
			}
			own_fronts.push_back(front->id);
		}

		if (own_fronts.empty()) {
			continue;
		}

		// Give every army without a front to one of the fronts
		size_t next_front = 0;
		for (ArmyInstance const& army : country.get_armies()) {
			bool on_front = false;
			for (Front const& other : fronts) {
				if (std::find(other.army_ids.begin(), other.army_ids.end(), army.unique_id) != other.army_ids.end()) {
					on_front = true;
					break;
				}
			}
			if (!on_front) {
				if (Front* const target = find_front(own_fronts[next_front++ % own_fronts.size()]); target != nullptr) {
					target->army_ids.push_back(army.unique_id);
				}
			}
		}
	}
}

void WarManager::update_fronts(InstanceManager& instance_manager) {
	std::erase_if(fronts, [](Front const& front) -> bool {
		return !front.country->exists();
	});

	for (Front& front : fronts) {
		spread_front_divisions(instance_manager, front);
		order_front_armies(instance_manager, front);
	}
}
