// Fronts, the lines of the plans and the automatic war plan, part of WarManager
#include <algorithm>
#include <deque>
#include <limits>

#include "War.hpp"

#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/military/ArmyGroup.hpp"
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

	// Enemy held provinces next to the line, the targets of the attack (empty in the DEFEND mode); a zone (the offensives of a field marshal) limits them
	std::vector<ProvinceInstance*> get_front_targets(
		Front const& front, MapInstance& map_instance, std::span<ProvinceInstance* const> zone = {}, const bool limited = false
	) {
		std::vector<ProvinceInstance*> targets;
		if (front.mode != Front::mode_t::ATTACK) {
			return targets;
		}
		for (ProvinceInstance* line_province : front.provinces) {
			for (ProvinceInstance* neighbour : get_land_neighbours(*line_province, map_instance)) {
				CountryInstance const* const controller = neighbour->get_controller();
				if (controller != nullptr && front.country->is_at_war_with(*controller)
					&& (!limited || std::find(zone.begin(), zone.end(), neighbour) != zone.end())
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

	// The provinces in order along the line: a neighbour of the last one first, else the first one left (the start of a new stretch)
	memory::vector<ProvinceInstance*> order_along_line(MapInstance& map_instance, std::vector<ProvinceInstance*> const& border) {
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
		return order_along_line(map_instance, border);
	}

	// The pieces of the border of the line of an automatic front: every province of the line with its neighbours the enemy holds
	memory::vector<FrontEdge> build_border_edges(MapInstance& map_instance, memory::vector<ProvinceInstance*> const& line, CountryInstance const& enemy) {
		memory::vector<FrontEdge> edges;
		for (ProvinceInstance* province : line) {
			for (ProvinceInstance* neighbour : get_land_neighbours(*province, map_instance)) {
				if (is_held_by(*neighbour, enemy)) {
					edges.push_back({ province, neighbour });
				}
			}
		}
		return edges;
	}

	// The provinces of the country on the line of the edges, in order along it
	memory::vector<ProvinceInstance*> line_of_edges(MapInstance& map_instance, memory::vector<FrontEdge> const& edges) {
		std::vector<ProvinceInstance*> border;
		for (FrontEdge const& edge : edges) {
			if (std::find(border.begin(), border.end(), edge.own) == border.end()) {
				border.push_back(edge.own);
			}
		}
		return order_along_line(map_instance, border);
	}

	bool is_coastal_neighbour(ProvinceInstance const& land, ProvinceInstance const& sea) {
		for (ProvinceDefinition::adjacency_t const& adjacency : land.province_definition.get_adjacencies()) {
			if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::COASTAL && &adjacency.get_to() == &sea.province_definition) {
				return true;
			}
		}
		return false;
	}

	bool is_land_neighbour(ProvinceInstance const& first, ProvinceInstance const& second) {
		for (ProvinceDefinition::adjacency_t const& adjacency : first.province_definition.get_adjacencies()) {
			if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && &adjacency.get_to() == &second.province_definition) {
				return true;
			}
		}
		return false;
	}

	double centre_distance_squared(ProvinceInstance const& first, ProvinceInstance const& second) {
		const double dx = static_cast<double>(first.province_definition.get_centre().x) - static_cast<double>(second.province_definition.get_centre().x);
		const double dy = static_cast<double>(first.province_definition.get_centre().y) - static_cast<double>(second.province_definition.get_centre().y);
		return dx * dx + dy * dy;
	}
}

// The countries of the sector of a front against the neighbour: he and, while he fights the country, his allies in the same war (docs/WAR_ASSUMPTIONS.md,
// section 6)
static bool is_country_of_sector(WarManager& war_manager, CountryInstance const& country, CountryInstance const& neighbour, CountryInstance const* candidate) {
	if (candidate == nullptr || candidate == &country) {
		return false;
	}
	if (candidate == &neighbour) {
		return true;
	}
	if (!country.is_at_war_with(neighbour) || !country.is_at_war_with(*candidate)) {
		return false;
	}
	War* const war = war_manager.find_war_between(country, neighbour);
	return war != nullptr && war->has_participant(*candidate) && war->is_attacker(*candidate) == war->is_attacker(neighbour);
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
	// Fronts belong to the countries with mechanic:command (the field marshals); the others give orders by hand
	if (!country.exists() || !country.has_army_command()) {
		return 0;
	}

	Front front;
	front.id = next_front_id++;
	front.country = &country;
	front.enemy = enemy;
	if (enemy != nullptr && enemy != &country) {
		front.provinces = build_border_line(instance_manager.get_map_instance(), country, *enemy);
		front.edges = build_border_edges(instance_manager.get_map_instance(), front.provinces, *enemy);
		front.mode = Front::mode_t::ATTACK;
	} else {
		front.enemy = nullptr;
	}

	fronts.push_back(std::move(front));
	return fronts.back().id;
}

namespace {
	// A front of the field marshal (or nobody's) needs mechanic:command and the group
	bool can_hold_front(CountryInstance const& country, const std::uint32_t group_id) {
		return country.exists() && country.has_army_command() && (group_id == 0 || country.find_army_group(group_id) != nullptr);
	}
}

std::uint32_t WarManager::create_border_front(
	InstanceManager& instance_manager, CountryInstance& country, const std::uint32_t group_id, CountryInstance const& neighbour
) {
	if (!can_hold_front(country, group_id) || &neighbour == &country || !neighbour.exists()) {
		return 0;
	}
	MapInstance& map_instance = instance_manager.get_map_instance();
	Front front;
	front.country = &country;
	front.enemy = &neighbour;
	front.group_id = group_id;
	// Every piece of the border of the land the country controls with the land of a country of the sector, in the order of the provinces
	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		if (province.get_controller() != &country || province.province_definition.is_water()) {
			continue;
		}
		for (ProvinceInstance* neighbour_province : get_land_neighbours(province, map_instance)) {
			if (is_country_of_sector(*this, country, neighbour, neighbour_province->get_controller())) {
				front.edges.push_back({ &province, neighbour_province });
			}
		}
	}
	if (front.edges.empty()) {
		return 0;
	}
	front.provinces = line_of_edges(map_instance, front.edges);
	// A field marshal has one order: the new front takes the place of the earlier one (and of its offensive)
	delete_group_lines(instance_manager, country, group_id);
	front.id = next_front_id++;
	fronts.push_back(std::move(front));
	return fronts.back().id;
}

std::uint32_t WarManager::create_coastal_front(
	InstanceManager& instance_manager, CountryInstance& country, const std::uint32_t group_id, ProvinceInstance const& sea
) {
	if (!can_hold_front(country, group_id) || !sea.province_definition.is_water()) {
		return 0;
	}
	MapInstance& map_instance = instance_manager.get_map_instance();
	Front front;
	front.country = &country;
	front.kind = Front::kind_t::COAST;
	front.sea = &sea;
	front.group_id = group_id;
	for (ProvinceDefinition::adjacency_t const& adjacency : sea.province_definition.get_adjacencies()) {
		if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::COASTAL || adjacency.get_to().is_water()) {
			continue;
		}
		ProvinceInstance& land = map_instance.get_province_instance_by_definition(adjacency.get_to());
		if (land.get_controller() == &country) {
			front.edges.push_back({ &land, &map_instance.get_province_instance_by_definition(sea.province_definition) });
		}
	}
	if (front.edges.empty()) {
		return 0;
	}
	std::sort(front.edges.begin(), front.edges.end(), [](FrontEdge const& first, FrontEdge const& second) -> bool {
		return first.own->index < second.own->index;
	});
	front.provinces = line_of_edges(map_instance, front.edges);
	delete_group_lines(instance_manager, country, group_id);
	front.id = next_front_id++;
	fronts.push_back(std::move(front));
	return fronts.back().id;
}

bool WarManager::toggle_front_edge(
	InstanceManager& instance_manager, CountryInstance const& country, const std::uint32_t front_id, ProvinceInstance& own, ProvinceInstance& other
) {
	Front* const front = find_front(front_id);
	if (front == nullptr || front->country != &country) {
		return false;
	}
	const FrontEdge edge { &own, &other };
	const auto it = std::find(front->edges.begin(), front->edges.end(), edge);
	if (it != front->edges.end()) {
		if (front->edges.size() == 1) {
			return false; // the last piece: the front is deleted instead
		}
		front->edges.erase(it);
	} else {
		if (own.get_controller() != &country || own.province_definition.is_water()) {
			return false;
		}
		if (front->kind == Front::kind_t::COAST) {
			if (&other != front->sea || !is_coastal_neighbour(own, other)) {
				return false;
			}
		} else if (front->enemy == nullptr || !is_land_neighbour(own, other) || other.province_definition.is_water()
			|| !is_country_of_sector(*this, country, *front->enemy, other.get_controller())) {
			return false;
		}
		front->edges.push_back(edge);
	}
	front->edited = true;
	front->provinces = line_of_edges(instance_manager.get_map_instance(), front->edges);
	return true;
}

bool WarManager::delete_front(CountryInstance const& country, const std::uint32_t front_id) {
	const auto it = std::find_if(fronts.begin(), fronts.end(), [&](Front const& front) -> bool {
		return front.id == front_id && front.country == &country;
	});
	if (it == fronts.end()) {
		return false;
	}
	fronts.erase(it);
	// The offensives from the front are gone with it
	std::erase_if(plan_lines, [front_id](PlanLine const& line) -> bool {
		return line.type == PlanLine::type_t::OFFENSIVE && line.front_id == front_id;
	});
	return true;
}

PlanLine* WarManager::find_plan_line(const std::uint32_t id) {
	for (PlanLine& line : plan_lines) {
		if (line.id == id) {
			return &line;
		}
	}
	return nullptr;
}

PlanLine const* WarManager::get_plan_line_by_id(const std::uint32_t id) const {
	for (PlanLine const& line : plan_lines) {
		if (line.id == id) {
			return &line;
		}
	}
	return nullptr;
}

namespace {
	// Can the province be on a line of the type: the targets of an offensive are land the country does not control, a defence is land it controls,
	// a naval invasion lands on a coast the country does not control
	bool fits_plan_line(PlanLine::type_t type, CountryInstance const& country, ProvinceInstance const& province) {
		if (province.province_definition.is_water()) {
			return false;
		}
		switch (type) {
		case PlanLine::type_t::OFFENSIVE:
			return province.get_controller() != &country;
		case PlanLine::type_t::DEFENCE:
			return province.get_controller() == &country;
		case PlanLine::type_t::NAVAL:
			return province.get_controller() != &country && province.province_definition.is_coastal();
		}
		return false;
	}
}

std::uint32_t WarManager::create_plan_line(
	InstanceManager& instance_manager, CountryInstance& country, const std::uint32_t group_id, const PlanLine::type_t type, ProvinceInstance& province
) {
	if (!country.exists() || !country.has_army_command() || country.find_army_group(group_id) == nullptr || province.province_definition.is_water()) {
		return 0;
	}
	PlanLine line;
	line.country = &country;
	line.group_id = group_id;
	line.type = type;
	if (type == PlanLine::type_t::NAVAL) {
		// The first province is the port the divisions embark in
		if (province.get_controller() != &country || !province.province_definition.has_port()) {
			return 0;
		}
		line.port = &province;
	} else {
		if (!fits_plan_line(type, country, province)) {
			return 0;
		}
		if (type == PlanLine::type_t::OFFENSIVE) {
			// From the front of the marshal that is nearest to the target (the nearest province of its line; ties: the earlier front)
			double best = std::numeric_limits<double>::max();
			for (Front const& front : fronts) {
				if (front.country != &country || front.group_id != group_id || front.kind != Front::kind_t::BORDER) {
					continue;
				}
				for (ProvinceInstance const* const line_province : front.provinces) {
					const double distance = centre_distance_squared(*line_province, province);
					if (distance < best) {
						best = distance;
						line.front_id = front.id;
					}
				}
			}
			if (line.front_id == 0) {
				return 0; // an offensive starts from a front
			}
		}
		line.provinces.push_back(&province);
	}
	// A field marshal has one order: a new offensive takes the place of the earlier one (the front stays, the offensive goes from it); a line of
	// defence or a naval invasion takes the place of everything (the front, the offensive, the line)
	if (type == PlanLine::type_t::OFFENSIVE) {
		std::erase_if(plan_lines, [&country, group_id](PlanLine const& other) -> bool {
			return other.country == &country && other.group_id == group_id;
		});
	} else {
		delete_group_lines(instance_manager, country, group_id);
	}
	line.id = next_plan_line_id++;
	plan_lines.push_back(std::move(line));
	return plan_lines.back().id;
}

bool WarManager::toggle_plan_line_province(
	InstanceManager& instance_manager, CountryInstance const& country, const std::uint32_t line_id, ProvinceInstance& province
) {
	PlanLine* const line = find_plan_line(line_id);
	if (line == nullptr || line->country != &country) {
		return false;
	}
	const auto it = std::find(line->provinces.begin(), line->provinces.end(), &province);
	if (it != line->provinces.end()) {
		if (line->provinces.size() == 1 && line->type != PlanLine::type_t::NAVAL) {
			return false; // the last province: the line is deleted instead
		}
		line->provinces.erase(it);
		return true;
	}
	if (!fits_plan_line(line->type, country, province)) {
		return false;
	}
	line->provinces.push_back(&province);
	return true;
}

bool WarManager::delete_plan_line(CountryInstance const& country, const std::uint32_t line_id) {
	return std::erase_if(plan_lines, [&country, line_id](PlanLine const& line) -> bool {
		return line.id == line_id && line.country == &country;
	}) > 0;
}

bool WarManager::delete_group_lines(InstanceManager& instance_manager, CountryInstance const& country, const std::uint32_t group_id) {
	if (group_id == 0) {
		return false;
	}
	bool deleted = false;
	for (Front const& front : fronts) {
		if (front.country == &country && front.group_id == group_id) {
			stop_front_armies(instance_manager, front.id);
			deleted = true;
		}
	}
	std::erase_if(fronts, [&country, group_id](Front const& front) -> bool {
		return front.country == &country && front.group_id == group_id;
	});
	deleted |= std::erase_if(plan_lines, [&country, group_id](PlanLine const& line) -> bool {
		return line.country == &country && line.group_id == group_id;
	}) > 0;
	return deleted;
}

memory::vector<ProvinceInstance*> WarManager::get_offensive_zone(InstanceManager& instance_manager, PlanLine const& line) const {
	memory::vector<ProvinceInstance*> zone;
	Front const* const front = get_front_by_id(line.front_id);
	if (line.type != PlanLine::type_t::OFFENSIVE || front == nullptr || line.provinces.empty()) {
		return zone;
	}
	MapInstance& map_instance = instance_manager.get_map_instance();
	CountryInstance const& country = *line.country;
	const std::size_t count = map_instance.get_province_instances().size();
	constexpr std::int32_t FAR = std::numeric_limits<std::int32_t>::max();
	// The steps over land the country does not control: from the land next to the front and from the targets
	const auto steps_from = [&](std::vector<ProvinceInstance*> const& starts) -> std::vector<std::int32_t> {
		std::vector<std::int32_t> steps(count, FAR);
		std::deque<ProvinceInstance*> queue;
		for (ProvinceInstance* start : starts) {
			const std::size_t index = type_safe::get(start->index);
			if (steps[index] == FAR) {
				steps[index] = 0;
				queue.push_back(start);
			}
		}
		while (!queue.empty()) {
			ProvinceInstance* const current = queue.front();
			queue.pop_front();
			const std::int32_t next_steps = steps[type_safe::get(current->index)] + 1;
			for (ProvinceInstance* neighbour : get_land_neighbours(*current, map_instance)) {
				const std::size_t index = type_safe::get(neighbour->index);
				if (steps[index] == FAR && neighbour->get_controller() != &country) {
					steps[index] = next_steps;
					queue.push_back(neighbour);
				}
			}
		}
		return steps;
	};
	std::vector<ProvinceInstance*> front_side;
	for (FrontEdge const& edge : front->edges) {
		if (edge.other->get_controller() != &country && std::find(front_side.begin(), front_side.end(), edge.other) == front_side.end()) {
			front_side.push_back(edge.other);
		}
	}
	std::vector<ProvinceInstance*> targets;
	for (ProvinceInstance* target : line.provinces) {
		if (target->get_controller() != &country) {
			targets.push_back(target);
		}
	}
	if (targets.empty()) {
		return zone;
	}
	const std::vector<std::int32_t> from_front = steps_from(front_side);
	const std::vector<std::int32_t> from_targets = steps_from(targets);
	// The provinces on the shortest ways from the front to the targets, with PLAN_ZONE_SLACK steps more: the corridor of the arrow
	std::int64_t shortest = FAR;
	for (std::size_t index = 0; index < count; ++index) {
		if (from_front[index] != FAR && from_targets[index] != FAR) {
			shortest = std::min<std::int64_t>(shortest, static_cast<std::int64_t>(from_front[index]) + from_targets[index]);
		}
	}
	const std::int64_t slack = get_war_defines().plan_zone_slack;
	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		const std::size_t index = type_safe::get(province.index);
		const bool target = std::find(targets.begin(), targets.end(), &province) != targets.end();
		if (target || (shortest != FAR && from_front[index] != FAR && from_targets[index] != FAR
			&& static_cast<std::int64_t>(from_front[index]) + from_targets[index] <= shortest + slack)) {
			zone.push_back(&province);
		}
	}
	return zone;
}

Front const* WarManager::find_front_of_group(CountryInstance const& country, const std::uint32_t group_id) const {
	for (Front const& front : fronts) {
		if (front.country == &country && front.group_id == group_id && group_id != 0) {
			return &front;
		}
	}
	return nullptr;
}

void WarManager::update_plan_lines(InstanceManager& instance_manager) {
	// The lines of groups that are gone, the offensives without their front, the fronts of the groups that are gone go with them
	std::erase_if(plan_lines, [this](PlanLine const& line) -> bool {
		if (!line.country->exists() || line.country->find_army_group(line.group_id) == nullptr) {
			return true;
		}
		if (line.type == PlanLine::type_t::OFFENSIVE) {
			Front const* const front = get_front_by_id(line.front_id);
			return front == nullptr || front->group_id != line.group_id;
		}
		return false;
	});
}

bool WarManager::set_front_group(CountryInstance const& country, const std::uint32_t front_id, const std::uint32_t group_id) {
	Front* const front = find_front(front_id);
	if (front == nullptr || front->country != &country || front->group_id == group_id) {
		return false;
	}
	if (group_id != 0 && country.find_army_group(group_id) == nullptr) {
		return false;
	}
	front->group_id = group_id;
	return true;
}

bool WarManager::is_front_commanded(Front const& front) const {
	if (front.group_id == 0) {
		return true;
	}
	ArmyGroup const* const group = front.country->find_army_group(front.group_id);
	return group != nullptr && group->plan_active;
}

void WarManager::stop_front_armies(InstanceManager& instance_manager, const std::uint32_t front_id) {
	Front const* const front = get_front_by_id(front_id);
	if (front == nullptr) {
		return;
	}
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	for (const unique_id_t army_id : front->army_ids) {
		UnitInstanceGroup* const group = unit_manager.get_unit_instance_group_by_unique_id(army_id);
		if (group != nullptr && group->branch == unit_branch_t::LAND && !group->is_in_combat()) {
			group->clear_path();
		}
	}
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

void WarManager::order_front_armies(InstanceManager& instance_manager, Front& front, std::span<ProvinceInstance* const> zone, const bool limited) {
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

	std::vector<ProvinceInstance*> const targets = get_front_targets(front, map_instance, zone, limited);

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

void WarManager::spread_front_divisions(InstanceManager& instance_manager, Front& front, std::span<ProvinceInstance* const> zone, const bool limited) {
	CountryInstance& country = *front.country;
	OpenVicWarDefines const& defines = get_war_defines();
	if (front.provinces.empty() || defines.front_spread_max_splits_per_day <= 0) {
		return;
	}
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	const std::vector<ProvinceInstance*> targets = get_front_targets(front, instance_manager.get_map_instance(), zone, limited);
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
			const size_t units = count_division_units(*army);
			if (units > biggest_units) {
				biggest = army;
				biggest_units = units;
			}
		}
		if (biggest == nullptr) {
			break;
		}
		ArmyInstance* const split_army = split_division_unit(unit_manager, *biggest);
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

		// Remove the generated fronts of enemies that are gone (all the fronts of a country without mechanic:command: its divisions get orders
		// by hand, update_fronts)
		const bool command = country.has_army_command();
		std::erase_if(fronts, [&country, command](Front const& front) -> bool {
			return front.country == &country && (!command || (front.enemy != nullptr && front.group_id == 0 && !country.is_at_war_with(*front.enemy)));
		});
		if (!command) {
			continue;
		}

		std::vector<std::uint32_t> own_fronts; // ids, the fronts vector may reallocate
		for (CountryInstance const& enemy : country.get_war_enemies()) {
			Front* front = nullptr;
			for (Front& candidate : fronts) {
				if (candidate.country == &country && candidate.enemy == &enemy) {
					front = &candidate;
					break;
				}
			}

			// A front that a field marshal holds is his: the automatic plan leaves its line, its mode and its armies alone
			if (front != nullptr && front->group_id != 0) {
				continue;
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
				front->edges = build_border_edges(instance_manager.get_map_instance(), line, enemy);
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

	update_plan_lines(instance_manager);
	for (Front& front : fronts) {
		if (!is_front_commanded(front)) {
			continue;
		}
		if (front.group_id == 0) {
			spread_front_divisions(instance_manager, front);
			order_front_armies(instance_manager, front);
			continue;
		}
		// A front of a field marshal attacks only into the zones of his offensives from it and holds the line without one (the executor of the plan
		// is P5 of docs/WAR_ASSUMPTIONS.md, section 6)
		memory::vector<ProvinceInstance*> zone;
		for (PlanLine const& line : plan_lines) {
			if (line.type == PlanLine::type_t::OFFENSIVE && line.front_id == front.id) {
				for (ProvinceInstance* province : get_offensive_zone(instance_manager, line)) {
					if (std::find(zone.begin(), zone.end(), province) == zone.end()) {
						zone.push_back(province);
					}
				}
			}
		}
		front.mode = zone.empty() ? Front::mode_t::DEFEND : Front::mode_t::ATTACK;
		spread_front_divisions(instance_manager, front, zone, true);
		order_front_armies(instance_manager, front, zone, true);
	}

	// The AI countries at war without mechanic:command have no fronts: the same minimal plan gives their divisions orders by hand, every division
	// an army of its own (docs/WAR_ASSUMPTIONS.md, section 6). The line is the border with all the enemies, rebuilt every day
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || !country.is_ai() || !country.is_at_war() || country.is_rebel_country() || country.has_army_command()) {
			continue;
		}
		Front orders;
		orders.country = &country;
		orders.mode = Front::mode_t::ATTACK;
		for (CountryInstance const& enemy : country.get_war_enemies()) {
			for (ProvinceInstance* province : build_border_line(instance_manager.get_map_instance(), country, enemy)) {
				if (std::find(orders.provinces.begin(), orders.provinces.end(), province) == orders.provinces.end()) {
					orders.provinces.push_back(province);
				}
			}
			// Hold the border when a war goes clearly badly
			if (War* const war = find_war_between(country, enemy); war != nullptr) {
				const bool is_attacker = war->is_attacker(country);
				if (war->get_score(!is_attacker) - war->get_score(is_attacker) > 20) {
					orders.mode = Front::mode_t::DEFEND;
				}
			}
		}
		for (ArmyInstance const& army : country.get_armies()) {
			orders.army_ids.push_back(army.unique_id);
		}
		spread_front_divisions(instance_manager, orders);
		order_front_armies(instance_manager, orders);
	}
}
