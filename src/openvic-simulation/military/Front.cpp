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

namespace {
	// Every piece of the border of the land the country controls with the land of a country of the sector, in the order of the provinces
	memory::vector<FrontEdge> border_edges(WarManager& war_manager, MapInstance& map_instance, CountryInstance const& country, CountryInstance const& neighbour) {
		memory::vector<FrontEdge> edges;
		if (!neighbour.exists()) {
			return edges;
		}
		for (ProvinceInstance& province : map_instance.get_province_instances()) {
			if (province.get_controller() != &country || province.province_definition.is_water()) {
				continue;
			}
			for (ProvinceInstance* neighbour_province : get_land_neighbours(province, map_instance)) {
				if (is_country_of_sector(war_manager, country, neighbour, neighbour_province->get_controller())) {
					edges.push_back({ &province, neighbour_province });
				}
			}
		}
		return edges;
	}

	// The coast of the land the country controls next to the sea province, in the order of the provinces
	memory::vector<FrontEdge> coast_edges(MapInstance& map_instance, CountryInstance const& country, ProvinceInstance const& sea) {
		memory::vector<FrontEdge> edges;
		ProvinceInstance& sea_instance = map_instance.get_province_instance_by_definition(sea.province_definition);
		for (ProvinceDefinition::adjacency_t const& adjacency : sea.province_definition.get_adjacencies()) {
			if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::COASTAL || adjacency.get_to().is_water()) {
				continue;
			}
			ProvinceInstance& land = map_instance.get_province_instance_by_definition(adjacency.get_to());
			if (land.get_controller() == &country) {
				edges.push_back({ &land, &sea_instance });
			}
		}
		std::sort(edges.begin(), edges.end(), [](FrontEdge const& first, FrontEdge const& second) -> bool {
			return first.own->index < second.own->index;
		});
		return edges;
	}
}

memory::vector<FrontEdge> WarManager::get_border_part(
	InstanceManager& instance_manager, CountryInstance const& country, CountryInstance const& neighbour, ProvinceInstance const* from, ProvinceInstance const* to
) {
	MapInstance& map_instance = instance_manager.get_map_instance();
	memory::vector<FrontEdge> edges = border_edges(*this, map_instance, country, neighbour);
	if (from == nullptr || to == nullptr || edges.empty()) {
		return edges;
	}
	// Our provinces in order along the border; a province of the other side stands for the first of ours next to it
	const memory::vector<ProvinceInstance*> line = line_of_edges(map_instance, edges);
	const auto place = [&](ProvinceInstance const* province) -> std::size_t {
		for (FrontEdge const& edge : edges) {
			if (edge.other == province) {
				province = edge.own;
				break;
			}
		}
		const auto it = std::find(line.begin(), line.end(), province);
		return it != line.end() ? static_cast<std::size_t>(it - line.begin()) : line.size();
	};
	std::size_t first = place(from);
	std::size_t last = place(to);
	if (first == line.size() || last == line.size()) {
		return {};
	}
	if (first > last) {
		std::swap(first, last);
	}
	std::erase_if(edges, [&](FrontEdge const& edge) -> bool {
		const std::size_t at = static_cast<std::size_t>(std::find(line.begin(), line.end(), edge.own) - line.begin());
		return at < first || at > last;
	});
	return edges;
}

std::uint32_t WarManager::create_border_front(
	InstanceManager& instance_manager, CountryInstance& country, const std::uint32_t group_id, CountryInstance const& neighbour,
	ProvinceInstance const* from, ProvinceInstance const* to
) {
	if (!can_hold_front(country, group_id) || &neighbour == &country || !neighbour.exists()) {
		return 0;
	}
	MapInstance& map_instance = instance_manager.get_map_instance();
	Front front;
	front.country = &country;
	front.enemy = &neighbour;
	front.group_id = group_id;
	front.edges = get_border_part(instance_manager, country, neighbour, from, to);
	// A part of the border is drawn by the player: the front keeps its pieces (as an edited one)
	front.edited = from != nullptr && to != nullptr;
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
	front.edges = coast_edges(map_instance, country, sea);
	if (front.edges.empty()) {
		return 0;
	}
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
	if (line->type == PlanLine::type_t::DEFENCE) {
		// The chain: an end province is taken away, a province outside the line joins the nearer end over our land
		const auto at = std::find(line->provinces.begin(), line->provinces.end(), &province);
		if (at != line->provinces.end()) {
			if (line->provinces.size() == 1 || (at != line->provinces.begin() && at != line->provinces.end() - 1)) {
				return false;
			}
			line->provinces.erase(at);
			return true;
		}
		const std::size_t before = line->provinces.size();
		memory::vector<ProvinceInstance*> const kept = line->provinces;
		if (!extend_plan_line(instance_manager, country, line_id, true, province)) {
			return extend_plan_line(instance_manager, country, line_id, false, province);
		}
		// The other end may be nearer
		const std::size_t by_end = line->provinces.size() - before;
		line->provinces = kept;
		if (extend_plan_line(instance_manager, country, line_id, false, province) && line->provinces.size() - before <= by_end) {
			return true;
		}
		line->provinces = kept;
		return extend_plan_line(instance_manager, country, line_id, true, province);
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

bool WarManager::extend_plan_line(
	InstanceManager& instance_manager, CountryInstance const& country, const std::uint32_t line_id, const bool at_end, ProvinceInstance& province
) {
	PlanLine* const line = find_plan_line(line_id);
	if (line == nullptr || line->country != &country || line->type != PlanLine::type_t::DEFENCE || line->provinces.empty()
		|| province.get_controller() != &country || province.province_definition.is_water()) {
		return false;
	}
	memory::vector<ProvinceInstance*>& chain = line->provinces;
	const auto at = std::find(chain.begin(), chain.end(), &province);
	if (at != chain.end()) {
		// The line is cut at the province: from the other end to it
		const std::size_t index = static_cast<std::size_t>(at - chain.begin());
		const std::size_t size_before = chain.size();
		if (at_end) {
			chain.resize(index + 1);
		} else {
			chain.erase(chain.begin(), chain.begin() + static_cast<std::ptrdiff_t>(index));
		}
		return chain.size() != size_before;
	}
	// The shortest way over our land from the end to the province, not over the line itself (the order of the neighbours: the same in every run).
	// No way from that end (the line would have to go back over itself): from the other end
	if (!extend_chain(instance_manager, country, chain, at_end, province)) {
		return extend_chain(instance_manager, country, chain, !at_end, province);
	}
	return true;
}

bool WarManager::extend_chain(
	InstanceManager& instance_manager, CountryInstance const& country, memory::vector<ProvinceInstance*>& chain, const bool at_end, ProvinceInstance& province
) const {
	MapInstance& map_instance = instance_manager.get_map_instance();
	ProvinceInstance* const start = at_end ? chain.back() : chain.front();
	const std::size_t count = map_instance.get_province_instances().size();
	std::vector<ProvinceInstance*> came_from(count, nullptr);
	std::vector<std::int32_t> steps(count, -1);
	std::deque<ProvinceInstance*> queue;
	steps[type_safe::get(start->index)] = 0;
	queue.push_back(start);
	const std::int32_t max_steps = get_war_defines().plan_line_max_steps;
	while (!queue.empty() && steps[type_safe::get(province.index)] < 0) {
		ProvinceInstance* const current = queue.front();
		queue.pop_front();
		const std::int32_t next_steps = steps[type_safe::get(current->index)] + 1;
		if (next_steps > max_steps) {
			continue;
		}
		for (ProvinceInstance* neighbour : get_land_neighbours(*current, map_instance)) {
			const std::size_t index = type_safe::get(neighbour->index);
			if (steps[index] >= 0 || neighbour->get_controller() != &country || std::find(chain.begin(), chain.end(), neighbour) != chain.end()) {
				continue;
			}
			steps[index] = next_steps;
			came_from[index] = current;
			queue.push_back(neighbour);
		}
	}
	if (steps[type_safe::get(province.index)] < 0) {
		return false;
	}
	memory::vector<ProvinceInstance*> way;
	for (ProvinceInstance* current = &province; current != start;) {
		way.push_back(current);
		current = came_from[type_safe::get(current->index)];
	}
	if (at_end) {
		chain.insert(chain.end(), way.rbegin(), way.rend());
	} else {
		chain.insert(chain.begin(), way.begin(), way.end());
	}
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

void WarManager::rebuild_front_edges(InstanceManager& instance_manager, Front& front) {
	MapInstance& map_instance = instance_manager.get_map_instance();
	memory::vector<FrontEdge> full;
	if (front.kind == Front::kind_t::COAST) {
		if (front.sea != nullptr) {
			full = coast_edges(map_instance, *front.country, *front.sea);
		}
	} else if (front.enemy != nullptr) {
		full = border_edges(*this, map_instance, *front.country, *front.enemy);
	}
	if (front.edited) {
		// The pieces still on the border stay; the pieces of the provinces the line moved over are added: the ones taken from the other side and our
		// neighbours of the ones the enemy took (the pieces the player took away do not come back)
		std::vector<ProvinceInstance const*> touched;
		for (FrontEdge const& edge : front.edges) {
			if (edge.other->get_controller() == front.country) {
				touched.push_back(edge.other);
			}
			if (edge.own->get_controller() != front.country) {
				for (ProvinceInstance* next : get_land_neighbours(*edge.own, map_instance)) {
					touched.push_back(next);
				}
			}
		}
		memory::vector<FrontEdge> kept;
		for (FrontEdge const& edge : full) {
			const bool was = std::find(front.edges.begin(), front.edges.end(), edge) != front.edges.end();
			if (was || std::find(touched.begin(), touched.end(), edge.own) != touched.end()) {
				kept.push_back(edge);
			}
		}
		full = std::move(kept);
	}
	if (full == front.edges) {
		return;
	}
	// A front whose border is gone stays empty: its divisions stand where they are
	front.edges = std::move(full);
	front.provinces = line_of_edges(map_instance, front.edges);
}

void WarManager::update_front_lines(InstanceManager& instance_manager) {
	// The signature: the controller of every land province and the sides of every war (an event changed the border: the lines are made again)
	std::uint64_t signature = 1469598103934665603ULL;
	const auto mix = [&signature](const std::uint64_t value) {
		signature = (signature ^ value) * 1099511628211ULL;
	};
	for (ProvinceInstance const& province : instance_manager.get_map_instance().get_province_instances()) {
		if (!province.province_definition.is_water()) {
			CountryInstance const* const controller = province.get_controller();
			mix(controller != nullptr ? static_cast<std::uint64_t>(type_safe::get(controller->index)) + 1 : 0);
		}
	}
	for (War const& war : wars) {
		mix(war.id);
		for (CountryInstance const* const country : war.attackers) {
			mix(static_cast<std::uint64_t>(type_safe::get(country->index)) + 1);
		}
		mix(0);
		for (CountryInstance const* const country : war.defenders) {
			mix(static_cast<std::uint64_t>(type_safe::get(country->index)) + 1);
		}
	}
	if (signature == border_signature) {
		return;
	}
	border_signature = signature;
	// The fronts of the field marshals (the automatic fronts of the AI are made again by its plan)
	for (Front& front : fronts) {
		if (front.group_id != 0) {
			rebuild_front_edges(instance_manager, front);
		}
	}
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
	// The divisions of a field marshal go to his front and hold it at once; the execution of the plan only lets them attack
	return front.country->find_army_group(front.group_id) != nullptr;
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

void WarManager::join_group_armies(Front& front) {
	// The armies (and the divisions) of the field marshal are on his order: the ones of the group join it, the others leave it. An army that moves
	// and is not on the order (a manual order) joins when it stops
	CountryInstance& country = *front.country;
	std::erase_if(front.army_ids, [&country, &front](const unique_id_t army_id) -> bool {
		for (ArmyInstance const& army : country.get_armies()) {
			if (army.unique_id == army_id) {
				return army.get_army_group_id() != front.group_id;
			}
		}
		return true;
	});
	for (ArmyInstance const& army : country.get_armies()) {
		if (army.get_army_group_id() != front.group_id || army.get_transport_navy() != nullptr || army.empty()) {
			continue;
		}
		if (std::find(front.army_ids.begin(), front.army_ids.end(), army.unique_id) != front.army_ids.end()) {
			continue;
		}
		if (army.is_moving()) {
			continue;
		}
		for (Front& other : fronts) {
			if (&other != &front) {
				std::erase(other.army_ids, army.unique_id);
			}
		}
		front.army_ids.push_back(army.unique_id);
	}
}

void WarManager::distribute_divisions(InstanceManager& instance_manager, Front& front, const std::uint32_t integrity) {
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	MapInstance& map_instance = instance_manager.get_map_instance();
	OpenVicWarDefines const& defines = get_war_defines();
	CountryInstance& country = *front.country;
	if (front.provinces.empty()) {
		return;
	}
	std::vector<ArmyInstance*> armies;
	for (const unique_id_t army_id : front.army_ids) {
		UnitInstanceGroup* const group = unit_manager.get_unit_instance_group_by_unique_id(army_id);
		if (group != nullptr && group->branch == unit_branch_t::LAND && &group->get_country() == &country && !group->empty()) {
			armies.push_back(static_cast<ArmyInstance*>(group));
		}
	}
	// Every army that stands is split into divisions of their own
	std::int32_t splits = 0;
	for (std::size_t index = 0; index < armies.size() && splits < defines.front_group_splits_per_day; ++index) {
		while (splits < defines.front_group_splits_per_day && !armies[index]->is_moving() && !armies[index]->is_in_combat()
			&& count_division_units(*armies[index]) >= 2) {
			ArmyInstance* const split_army = split_division_unit(unit_manager, *armies[index]);
			if (split_army == nullptr) {
				break;
			}
			splits++;
			front.army_ids.push_back(split_army->unique_id);
			armies.push_back(split_army);
		}
	}
	const std::size_t province_count = front.provinces.size();
	const auto place_of = [&](ProvinceInstance const* province) -> std::size_t {
		const auto it = std::find(front.provinces.begin(), front.provinces.end(), province);
		return static_cast<std::size_t>(it - front.provinces.begin());
	};
	// The weights: the minimum and the enemy divisions next to the province (across the pieces of the line, or all its land neighbours for a line
	// without pieces)
	std::vector<std::int64_t> weights(province_count, defines.front_min_weight);
	std::vector<ProvinceInstance const*> seen;
	for (std::size_t index = 0; index < province_count; ++index) {
		seen.clear();
		const auto threat_of = [&](ProvinceInstance const& other) {
			if (std::find(seen.begin(), seen.end(), &other) != seen.end()) {
				return;
			}
			seen.push_back(&other);
			for (ArmyInstance const& enemy : other.get_armies()) {
				if (country.is_at_war_with(enemy.get_country())) {
					weights[index] += static_cast<std::int64_t>(enemy.get_divisions().size());
				}
			}
		};
		if (!front.edges.empty()) {
			for (FrontEdge const& edge : front.edges) {
				if (edge.own == front.provinces[index]) {
					threat_of(*edge.other);
				}
			}
		} else {
			for (ProvinceInstance* other : get_land_neighbours(*front.provinces[index], map_instance)) {
				threat_of(*other);
			}
		}
	}
	// The shares (the largest remainders; ties: the earlier province of the line)
	std::int64_t total_weight = 0;
	for (const std::int64_t weight : weights) {
		total_weight += weight;
	}
	const std::int64_t divisions = static_cast<std::int64_t>(armies.size());
	std::vector<std::int64_t> shares(province_count, 0);
	std::vector<std::pair<std::int64_t, std::size_t>> remainders;
	std::int64_t given = 0;
	for (std::size_t index = 0; index < province_count; ++index) {
		shares[index] = divisions * weights[index] / total_weight;
		given += shares[index];
		remainders.emplace_back(divisions * weights[index] % total_weight, index);
	}
	std::stable_sort(remainders.begin(), remainders.end(), [](auto const& first, auto const& second) -> bool {
		return first.first > second.first;
	});
	for (std::size_t index = 0; given < divisions && index < remainders.size(); ++index, ++given) {
		shares[remainders[index].second]++;
	}
	// Where the divisions are (or go)
	std::vector<std::int64_t> placed(province_count, 0);
	std::vector<std::size_t> place(armies.size(), province_count);
	for (std::size_t index = 0; index < armies.size(); ++index) {
		ArmyInstance const& army = *armies[index];
		ProvinceInstance const* const at = army.is_moving() ? &static_cast<ProvinceInstance const&>(army.get_path().back()) : &army.get_location();
		place[index] = place_of(at);
		if (place[index] < province_count) {
			placed[place[index]]++;
		}
	}
	// The divisions without a place that stand, and the ones over the share where the integrity lets them move
	const std::int64_t threshold = integrity == 0 ? std::numeric_limits<std::int64_t>::max() : integrity == 1 ? defines.front_rebalance_threshold : 1;
	std::vector<std::size_t> free_divisions;
	for (std::size_t index = 0; index < armies.size(); ++index) {
		if (place[index] == province_count && !armies[index]->is_moving() && !armies[index]->is_in_combat()) {
			free_divisions.push_back(index);
		}
	}
	for (std::size_t province = 0; province < province_count; ++province) {
		if (placed[province] - shares[province] < threshold) {
			continue;
		}
		// The last ones of the line go first (the order of the armies: the same in every run)
		for (std::size_t index = armies.size(); index-- > 0 && placed[province] > shares[province];) {
			if (place[index] == province && !armies[index]->is_moving() && !armies[index]->is_in_combat()) {
				placed[province]--;
				place[index] = province_count;
				free_divisions.push_back(index);
			}
		}
	}
	// Each to the nearest province that lacks divisions (ties: the earlier one of the line)
	for (const std::size_t index : free_divisions) {
		ArmyInstance& army = *armies[index];
		std::size_t best = province_count;
		double best_distance = std::numeric_limits<double>::max();
		for (std::size_t province = 0; province < province_count; ++province) {
			if (placed[province] >= shares[province]) {
				continue;
			}
			const double distance = centre_distance_squared(army.get_location(), *front.provinces[province]);
			if (distance < best_distance) {
				best_distance = distance;
				best = province;
			}
		}
		if (best == province_count) {
			break;
		}
		placed[best]++;
		if (front.provinces[best] != &army.get_location()) {
			unit_manager.order_move(army, *front.provinces[best], map_instance);
		}
	}
}

void WarManager::update_fronts(InstanceManager& instance_manager) {
	std::erase_if(fronts, [](Front const& front) -> bool {
		return !front.country->exists();
	});

	update_plan_lines(instance_manager);
	update_front_lines(instance_manager);
	for (Front& front : fronts) {
		if (!is_front_commanded(front)) {
			continue;
		}
		if (front.group_id == 0) {
			spread_front_divisions(instance_manager, front);
			order_front_armies(instance_manager, front);
			continue;
		}
		join_group_armies(front);
		// A front of a field marshal holds its line; while the plan is executed it attacks only into the zones of his offensives from it (the executor
		// of the plan is P5 of docs/WAR_ASSUMPTIONS.md, section 6)
		ArmyGroup const* const group = front.country->find_army_group(front.group_id);
		memory::vector<ProvinceInstance*> zone;
		if (group != nullptr && group->plan_active) {
			for (PlanLine const& line : plan_lines) {
				if (line.type == PlanLine::type_t::OFFENSIVE && line.front_id == front.id) {
					for (ProvinceInstance* province : get_offensive_zone(instance_manager, line)) {
						if (std::find(zone.begin(), zone.end(), province) == zone.end()) {
							zone.push_back(province);
						}
					}
				}
			}
		}
		front.mode = zone.empty() ? Front::mode_t::DEFEND : Front::mode_t::ATTACK;
		// The divisions stand along the line; while an offensive is executed the ones next to its zone attack (the executor is P5)
		distribute_divisions(instance_manager, front, group != nullptr ? group->integrity : 1);
		if (front.mode == Front::mode_t::ATTACK) {
			order_front_armies(instance_manager, front, zone, true);
		}
	}

	// The line of defence and the naval invasion of a field marshal (his order when he has no front): his divisions go to the provinces of the line
	// and hold them (P9), to the port of the landing (P10)
	for (PlanLine const& line : plan_lines) {
		if (line.type == PlanLine::type_t::OFFENSIVE || !line.country->exists() || line.country->find_army_group(line.group_id) == nullptr) {
			continue;
		}
		Front orders;
		orders.country = line.country;
		orders.group_id = line.group_id;
		orders.mode = Front::mode_t::DEFEND;
		if (line.type == PlanLine::type_t::DEFENCE) {
			for (ProvinceInstance* province : line.provinces) {
				if (province->get_controller() == line.country) {
					orders.provinces.push_back(province);
				}
			}
		} else if (line.port != nullptr && line.port->get_controller() == line.country) {
			orders.provinces.push_back(line.port);
		}
		join_group_armies(orders);
		if (line.type == PlanLine::type_t::DEFENCE) {
			ArmyGroup const* const group = line.country->find_army_group(line.group_id);
			distribute_divisions(instance_manager, orders, group != nullptr ? group->integrity : 1);
		} else {
			order_front_armies(instance_manager, orders);
		}
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
