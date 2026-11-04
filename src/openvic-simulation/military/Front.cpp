// Fronts, the lines of the plans and the automatic war plan, part of WarManager
#include <algorithm>
#include <deque>
#include <unordered_map>
#include <limits>

#include "War.hpp"

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/military/ArmyGroup.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/military/Division.hpp"
#include "openvic-simulation/military/MilitaryManager.hpp"
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
	// The offensives and the breakthroughs from the front are gone with it
	std::erase_if(plan_lines, [front_id](PlanLine const& line) -> bool {
		return (line.type == PlanLine::type_t::OFFENSIVE || line.type == PlanLine::type_t::BREAKTHROUGH) && line.front_id == front_id;
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
		case PlanLine::type_t::BREAKTHROUGH:
			return province.get_controller() != &country;
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
		if (type == PlanLine::type_t::OFFENSIVE || type == PlanLine::type_t::BREAKTHROUGH) {
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
	// A field marshal has one order: a new offensive (a breakthrough) takes the place of the earlier one (the front stays, they go from it); a line of
	// defence or a naval invasion takes the place of everything (the front, the offensive, the breakthrough, the line)
	if (type == PlanLine::type_t::OFFENSIVE || type == PlanLine::type_t::BREAKTHROUGH) {
		std::erase_if(plan_lines, [&country, group_id, type](PlanLine const& other) -> bool {
			return other.country == &country && other.group_id == group_id && (other.type == type || (other.type != PlanLine::type_t::OFFENSIVE
				&& other.type != PlanLine::type_t::BREAKTHROUGH));
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
	if (line->type == PlanLine::type_t::DEFENCE || line->type == PlanLine::type_t::BREAKTHROUGH) {
		// The chain: an end province is taken away, a province outside the line joins the nearer end (over our land, or the land of the others)
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
	if (line == nullptr || line->country != &country || line->provinces.empty() || province.province_definition.is_water()
		|| (line->type != PlanLine::type_t::DEFENCE && line->type != PlanLine::type_t::BREAKTHROUGH)) {
		return false;
	}
	// A line of defence over our land, a breakthrough over the land of the others
	const bool own_land = line->type == PlanLine::type_t::DEFENCE;
	if ((province.get_controller() == &country) != own_land) {
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
	if (!extend_chain(instance_manager, country, chain, at_end, province, own_land)) {
		return extend_chain(instance_manager, country, chain, !at_end, province, own_land);
	}
	return true;
}

bool WarManager::extend_chain(
	InstanceManager& instance_manager, CountryInstance const& country, memory::vector<ProvinceInstance*>& chain, const bool at_end, ProvinceInstance& province,
	const bool own_land
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
			if (steps[index] >= 0 || (neighbour->get_controller() == &country) != own_land || std::find(chain.begin(), chain.end(), neighbour) != chain.end()) {
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
		if (line.type == PlanLine::type_t::OFFENSIVE || line.type == PlanLine::type_t::BREAKTHROUGH) {
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

void WarManager::update_ai_marshals(InstanceManager& instance_manager, CountryInstance& country) {
	MapInstance& map_instance = instance_manager.get_map_instance();
	const std::size_t palette_size = instance_manager.definition_manager.get_military_manager().get_command_palette().size();
	// The enemies it borders, in the order of the countries
	std::vector<CountryInstance const*> enemies;
	for (CountryInstance const& enemy : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (&enemy != &country && country.is_at_war_with(enemy) && !get_border_part(instance_manager, country, enemy, nullptr, nullptr).empty()) {
			enemies.push_back(&enemy);
		}
	}
	// A group for every enemy: the one whose front is against him, else a new one (with a free general as its marshal)
	std::vector<std::uint32_t> group_ids;
	for (CountryInstance const* const enemy : enemies) {
		std::uint32_t group_id = 0;
		for (Front const& front : fronts) {
			if (front.country == &country && front.group_id != 0 && front.enemy == enemy) {
				group_id = front.group_id;
			}
		}
		if (group_id == 0) {
			if (!create_army_group(country, palette_size)) {
				break;
			}
			group_id = country.get_army_groups().back().id;
			for (LeaderInstance const& leader : country.get_leaders<unit_branch_t::LAND>()) {
				if (!leader.is_marshal() && leader.get_can_be_used() && leader.get_unit_instance_group() == nullptr
					&& set_army_group_marshal(instance_manager, country, group_id, leader.unique_id)) {
					break;
				}
			}
			const std::uint32_t front_id = create_border_front(instance_manager, country, group_id, *enemy);
			Front const* const front = get_front_by_id(front_id);
			if (front != nullptr) {
				// The offensive: the strip of the enemy land along the front and the provinces next to it
				const memory::vector<FrontEdge> edges = front->edges;
				std::uint32_t offensive_id = 0;
				for (FrontEdge const& edge : edges) {
					if (offensive_id == 0) {
						offensive_id = create_plan_line(instance_manager, country, group_id, PlanLine::type_t::OFFENSIVE, *edge.other);
					} else {
						toggle_plan_line_province(instance_manager, country, offensive_id, *edge.other);
					}
				}
				if (PlanLine const* const line = get_plan_line_by_id(offensive_id); line != nullptr) {
					const memory::vector<ProvinceInstance*> strip = line->provinces;
					for (ProvinceInstance* province : strip) {
						for (ProvinceInstance* deeper : get_land_neighbours(*province, map_instance)) {
							PlanLine const* const now = get_plan_line_by_id(offensive_id);
							if (deeper->get_controller() == enemy && std::find(now->provinces.begin(), now->provinces.end(), deeper) == now->provinces.end()) {
								toggle_plan_line_province(instance_manager, country, offensive_id, *deeper);
							}
						}
					}
				}
			}
		}
		group_ids.push_back(group_id);
	}
	// The groups of the enemies that are gone (peace) go
	std::vector<std::uint32_t> gone;
	for (ArmyGroup const& group : country.get_army_groups()) {
		if (std::find(group_ids.begin(), group_ids.end(), group.id) == group_ids.end()) {
			gone.push_back(group.id);
		}
	}
	for (const std::uint32_t group_id : gone) {
		delete_army_group(instance_manager, country, group_id);
	}
	if (group_ids.empty()) {
		return;
	}
	// The armies of the generals without a group are shared among the groups in turn; every group executes its plan
	const std::uint32_t max_attached = get_marshal_max_attached(instance_manager, country);
	std::size_t next = 0;
	for (ArmyInstance& army : country.get_armies()) {
		if (army.get_army_group_id() == 0 && army.get_parent_army_id() == 0 && army.get_transport_navy() == nullptr && !army.empty()) {
			set_army_group(country, army, group_ids[next++ % group_ids.size()], max_attached);
		}
	}
	for (const std::uint32_t group_id : group_ids) {
		ArmyGroup const* const group = country.find_army_group(group_id);
		if (group != nullptr && !group->plan_active) {
			set_army_group_plan_active(instance_manager, country, group_id, true);
		}
	}
}

void WarManager::update_auto_plan(InstanceManager& instance_manager) {
	// Fronts are refreshed every 10 days
	if (instance_manager.get_today().get_day() % 10 != 0) {
		return;
	}

	// The field marshals of the AI countries (P11): only with PLAN_AI_ENABLED, their earlier plan otherwise
	if (get_war_defines().plan_ai_enabled != 0) {
		for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
			if (country.exists() && country.is_ai() && country.is_at_war() && !country.is_rebel_country() && country.has_army_command()) {
				update_ai_marshals(instance_manager, country);
			}
		}
	}

	// Countries with the plan switched on, and every AI country that is at war (a minimal war AI: the same plan)
	std::vector<CountryInstance*> planning_countries;
	for (CountryInstance& candidate : instance_manager.get_country_instance_manager().get_country_instances()) {
		const bool marshals = get_war_defines().plan_ai_enabled != 0 && candidate.has_army_command();
		if (auto_plan_countries.contains(&candidate)
			|| (candidate.is_ai() && candidate.is_at_war() && !candidate.is_rebel_country() && !marshals)) {
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
	// The divisions of a breakthrough of the marshal go their own way
	std::vector<unique_id_t> breaking;
	for (PlanLine const& line : plan_lines) {
		if (line.type == PlanLine::type_t::BREAKTHROUGH && line.country == &country && line.group_id == front.group_id) {
			breaking.insert(breaking.end(), line.army_ids.begin(), line.army_ids.end());
		}
	}
	std::erase_if(front.army_ids, [&breaking](const unique_id_t army_id) -> bool {
		return std::find(breaking.begin(), breaking.end(), army_id) != breaking.end();
	});
	for (ArmyInstance& army : country.get_armies()) {
		if (army.get_army_group_id() != front.group_id || army.get_transport_navy() != nullptr || army.empty()
			|| std::find(breaking.begin(), breaking.end(), army.unique_id) != breaking.end()) {
			continue;
		}
		if (std::find(front.army_ids.begin(), front.army_ids.end(), army.unique_id) != front.army_ids.end()) {
			continue;
		}
		if (army.is_moving()) {
			continue;
		}
		army.set_manual_order(false);
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
	// The fallback (P9): the share of the organisation of a division, the enemy next to a province, a province of the country behind the line
	const auto organisation_share = [](ArmyInstance const& army) -> fixed_point_t {
		fixed_point_t organisation = fixed_point_t::_0, maximum = fixed_point_t::_0;
		for (RegimentInstance const& regiment : army.get_regiment_instances()) {
			organisation += regiment.get_organisation();
			maximum += regiment.get_max_organisation();
		}
		return maximum > fixed_point_t::_0 ? organisation / maximum : fixed_point_t::_1;
	};
	const auto enemy_next_to = [&](ProvinceInstance const& province) -> bool {
		for (ProvinceInstance* neighbour : get_land_neighbours(province, map_instance)) {
			CountryInstance const* const controller = neighbour->get_controller();
			if (controller != nullptr && controller != &country && country.is_at_war_with(*controller)) {
				return true;
			}
		}
		return false;
	};
	const auto behind = [&](ProvinceInstance const& from) -> ProvinceInstance* {
		// The nearest province of the country (in steps, the order of the neighbours) off the line with no enemy next to it, at most three steps away
		std::vector<ProvinceInstance*> layer { const_cast<ProvinceInstance*>(&from) };
		std::vector<ProvinceInstance const*> seen { &from };
		for (int step = 0; step < 3 && !layer.empty(); ++step) {
			std::vector<ProvinceInstance*> next_layer;
			for (ProvinceInstance* current : layer) {
				for (ProvinceInstance* neighbour : get_land_neighbours(*current, map_instance)) {
					if (neighbour->get_controller() != &country || std::find(seen.begin(), seen.end(), neighbour) != seen.end()) {
						continue;
					}
					seen.push_back(neighbour);
					if (!enemy_next_to(*neighbour) && std::find(front.provinces.begin(), front.provinces.end(), neighbour) == front.provinces.end()) {
						return neighbour;
					}
					next_layer.push_back(neighbour);
				}
			}
			layer = std::move(next_layer);
		}
		return nullptr;
	};
	const fixed_point_t fall_back_below = defines.fallback_org_threshold.of(fixed_point_t::_1);
	const fixed_point_t return_from = defines.fallback_return_org.of(fixed_point_t::_1);
	std::vector<ArmyInstance*> armies;
	for (const unique_id_t army_id : front.army_ids) {
		UnitInstanceGroup* const group = unit_manager.get_unit_instance_group_by_unique_id(army_id);
		if (group == nullptr || group->branch != unit_branch_t::LAND || &group->get_country() != &country || group->empty()) {
			continue;
		}
		ArmyInstance* const army = static_cast<ArmyInstance*>(group);
		// A division that stands on the land of an enemy (it won the battle there) stays to take it; one that is cut off breaks out by itself
		CountryInstance const* const here = group->get_location().get_controller();
		if ((!group->is_moving() && here != nullptr && here != &country && country.is_at_war_with(*here)) || army->is_cut_off()) {
			continue;
		}
		if (!army->is_moving() && !army->is_in_combat()) {
			const bool on_line = std::find(front.provinces.begin(), front.provinces.end(), &army->get_location()) != front.provinces.end();
			const fixed_point_t share = organisation_share(*army);
			// Worn out on the line next to the enemy: it falls back behind the line
			if (on_line && share < fall_back_below && enemy_next_to(army->get_location())) {
				if (ProvinceInstance* const rear = behind(army->get_location()); rear != nullptr) {
					unit_manager.order_move(*army, *rear, map_instance);
					continue;
				}
			}
			// Off the line it recovers before it goes back
			if (!on_line && share < return_from && army->get_location().get_controller() == &country) {
				continue;
			}
		}
		armies.push_back(army);
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

bool OpenVic::offensive_attacks(const std::uint32_t aggression, BattleEstimate const& estimate, OpenVicWarDefines const& defines) {
	if (!estimate.valid) {
		return false;
	}
	Rate const& needed = aggression <= 1 ? defines.plan_win_careful : aggression == 2 ? defines.plan_win_balanced : defines.plan_win_aggressive;
	return estimate.win_chance >= needed.of(fixed_point_t::_1);
}

void WarManager::execute_offensive(
	InstanceManager& instance_manager, Front& front, std::span<ProvinceInstance* const> zone, std::span<ProvinceInstance* const> targets,
	const std::uint32_t aggression
) {
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	MapInstance& map_instance = instance_manager.get_map_instance();
	OpenVicWarDefines const& defines = get_war_defines();
	CountryInstance& country = *front.country;
	const auto in = [](std::span<ProvinceInstance* const> list, ProvinceInstance const* province) -> bool {
		return std::find(list.begin(), list.end(), province) != list.end();
	};
	// The divisions that stand on the line, free (not moving, not fighting), and how many stand in every province of the line
	std::vector<ArmyInstance*> free;
	std::vector<std::int32_t> standing(front.provinces.size(), 0);
	for (const unique_id_t army_id : front.army_ids) {
		UnitInstanceGroup* const group = unit_manager.get_unit_instance_group_by_unique_id(army_id);
		if (group == nullptr || group->branch != unit_branch_t::LAND || group->empty() || group->is_moving() || group->is_in_combat()) {
			continue;
		}
		ArmyInstance* const army = static_cast<ArmyInstance*>(group);
		const auto at = std::find(front.provinces.begin(), front.provinces.end(), &army->get_location());
		if (at != front.provinces.end()) {
			free.push_back(army);
			standing[static_cast<std::size_t>(at - front.provinces.begin())]++;
		}
	}
	if (free.empty()) {
		return;
	}
	// The targets next to the line: the land of the zone across its pieces that an enemy holds (in the order of the pieces)
	std::vector<ProvinceInstance*> candidates;
	for (FrontEdge const& edge : front.edges) {
		CountryInstance const* const controller = edge.other->get_controller();
		if (controller != nullptr && controller != &country && country.is_at_war_with(*controller) && in(zone, edge.other) && !in(candidates, edge.other)) {
			candidates.push_back(edge.other);
		}
	}
	if (candidates.empty()) {
		return;
	}
	// The steps from every province of the zone to the line of the plan (its targets), over the zone
	std::unordered_map<ProvinceInstance const*, std::int32_t> steps;
	std::deque<ProvinceInstance*> queue;
	for (ProvinceInstance* target : targets) {
		if (in(zone, target) && steps.emplace(target, 0).second) {
			queue.push_back(target);
		}
	}
	while (!queue.empty()) {
		ProvinceInstance* const current = queue.front();
		queue.pop_front();
		for (ProvinceInstance* neighbour : get_land_neighbours(*current, map_instance)) {
			if (in(zone, neighbour) && !steps.contains(neighbour)) {
				steps.emplace(neighbour, steps[current] + 1);
				queue.push_back(neighbour);
			}
		}
	}
	const std::int32_t min_hold = aggression >= 3 ? 0 : defines.plan_min_hold;
	std::vector<bool> used(free.size(), false);
	// The free divisions next to the target that may go (the minimum stays in every province of the line)
	const auto attackers_of = [&](ProvinceInstance const* target, std::vector<std::int32_t>& left) -> std::vector<std::size_t> {
		std::vector<std::size_t> chosen;
		for (std::size_t index = 0; index < free.size(); ++index) {
			if (used[index]) {
				continue;
			}
			ProvinceInstance const* const at = &free[index]->get_location();
			bool next_to = false;
			for (FrontEdge const& edge : front.edges) {
				next_to |= edge.own == at && edge.other == target;
			}
			const std::size_t place = static_cast<std::size_t>(std::find(front.provinces.begin(), front.provinces.end(), at) - front.provinces.begin());
			if (next_to && left[place] > min_hold) {
				left[place]--;
				chosen.push_back(index);
			}
		}
		return chosen;
	};
	const auto estimate_with = [&](ProvinceInstance const& target, std::vector<std::size_t> const& chosen) -> BattleEstimate {
		std::vector<ArmyInstance const*> attacking;
		for (const std::size_t index : chosen) {
			attacking.push_back(free[index]);
		}
		return estimate_battle(instance_manager, attacking, target);
	};
	// The order of the targets: the chance of the victory and nearer to the line of the plan first; ties: the order of the pieces
	struct scored_t {
		ProvinceInstance* target;
		fixed_point_t score;
	};
	// The flank a target opens (the share of its land neighbours the enemy holds) and whether it closes a pocket (an enemy province next to it with no
	// other way to the land of its side)
	const auto enemy_held = [&](ProvinceInstance const& province) -> bool {
		CountryInstance const* const controller = province.get_controller();
		return controller != nullptr && controller != &country && country.is_at_war_with(*controller);
	};
	const auto flank_of = [&](ProvinceInstance const& target) -> fixed_point_t {
		std::int32_t land = 0, enemy = 0;
		for (ProvinceInstance* neighbour : get_land_neighbours(target, map_instance)) {
			land++;
			enemy += enemy_held(*neighbour) && !in(zone, neighbour);
		}
		return land > 0 ? fixed_point_t(enemy) / land : fixed_point_t::_0;
	};
	const auto closes_pocket = [&](ProvinceInstance const& target) -> bool {
		for (ProvinceInstance* neighbour : get_land_neighbours(target, map_instance)) {
			if (!enemy_held(*neighbour) || neighbour->get_armies().empty()) {
				continue;
			}
			std::int32_t ways = 0;
			for (ProvinceInstance* further : get_land_neighbours(*neighbour, map_instance)) {
				ways += further != &target && enemy_held(*further);
			}
			if (ways == 0) {
				return true;
			}
		}
		return false;
	};
	std::vector<scored_t> scored;
	for (ProvinceInstance* target : candidates) {
		// The careful and the balanced mode do not open a wide flank
		if (aggression <= 2 && flank_of(*target) > defines.flank_exposure_max.of(fixed_point_t::_1)) {
			continue;
		}
		std::vector<std::int32_t> left = standing;
		const BattleEstimate estimate = estimate_with(*target, attackers_of(target, left));
		if (!estimate.valid) {
			continue;
		}
		const auto found = steps.find(target);
		const std::int32_t to_line = found != steps.end() ? found->second : static_cast<std::int32_t>(zone.size());
		scored.push_back({
			target, estimate.win_chance * defines.plan_score_chance_weight + defines.plan_target_weight / (1 + to_line)
				+ (closes_pocket(*target) ? defines.plan_pocket_weight : fixed_point_t::_0)
		});
	}
	std::stable_sort(scored.begin(), scored.end(), [](scored_t const& first, scored_t const& second) -> bool {
		return first.score > second.score;
	});
	// Each target in turn with the divisions that are still free
	for (scored_t const& entry : scored) {
		std::vector<std::int32_t> left = standing;
		std::vector<std::size_t> chosen = attackers_of(entry.target, left);
		if (chosen.empty()) {
			continue;
		}
		const BattleEstimate estimate = estimate_with(*entry.target, chosen);
		if (!offensive_attacks(aggression, estimate, defines)) {
			continue;
		}
		// Nobody defends: one division takes the province, the others stay free
		if (estimate.breakthrough_days == 0 && estimate.ratio >= fixed_point_t(1000)) {
			chosen.resize(1);
		}
		for (const std::size_t index : chosen) {
			used[index] = true;
			const std::size_t place = static_cast<std::size_t>(std::find(front.provinces.begin(), front.provinces.end(), &free[index]->get_location()) - front.provinces.begin());
			standing[place]--;
			unit_manager.order_move(*free[index], *entry.target, map_instance);
		}
	}
}

void WarManager::execute_breakthrough(InstanceManager& instance_manager, PlanLine& line, const bool executed) {
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	MapInstance& map_instance = instance_manager.get_map_instance();
	OpenVicWarDefines const& defines = get_war_defines();
	CountryInstance& country = *line.country;
	Front const* const front = get_front_by_id(line.front_id);
	if (front == nullptr || line.provinces.empty()) {
		return;
	}
	// The divisions: the ones kept (still in the group), topped up with the strongest of the group by their attack (ties: the lower id)
	std::erase_if(line.army_ids, [&](const unique_id_t army_id) -> bool {
		UnitInstanceGroup const* const group = unit_manager.get_unit_instance_group_by_unique_id(army_id);
		return group == nullptr || group->branch != unit_branch_t::LAND || group->empty()
			|| static_cast<ArmyInstance const*>(group)->get_army_group_id() != line.group_id;
	});
	std::vector<std::pair<fixed_point_t, ArmyInstance*>> candidates;
	std::size_t divisions = 0;
	for (ArmyInstance& army : country.get_armies()) {
		if (army.get_army_group_id() != line.group_id || army.get_transport_navy() != nullptr || army.empty()) {
			continue;
		}
		divisions += army.get_divisions().size();
		// (a division on its way to the line may go: not one moving by a manual order, not one in a battle)
		if (std::find(line.army_ids.begin(), line.army_ids.end(), army.unique_id) != line.army_ids.end() || army.is_manual_order() || army.is_in_combat()
			|| army.get_divisions().empty()) {
			continue;
		}
		candidates.emplace_back(get_live_division_stats(instance_manager, army, army.get_divisions().front()).attack, &army);
	}
	std::stable_sort(candidates.begin(), candidates.end(), [](auto const& first, auto const& second) -> bool {
		return first.first != second.first ? first.first > second.first : first.second->unique_id < second.second->unique_id;
	});
	// The share counts the divisions (an army not split yet goes whole when its divisions fit)
	const std::size_t wanted = std::max<std::size_t>(1, static_cast<std::size_t>(defines.plan_breakthrough_share.of(fixed_point_t(static_cast<std::int32_t>(divisions))).ceil<std::int32_t>()));
	std::size_t taken = 0;
	for (const unique_id_t army_id : line.army_ids) {
		UnitInstanceGroup const* const group = unit_manager.get_unit_instance_group_by_unique_id(army_id);
		taken += static_cast<ArmyInstance const*>(group)->get_divisions().size();
	}
	for (auto const& [attack, army] : candidates) {
		if (taken >= wanted) {
			break;
		}
		if (taken > 0 && taken + army->get_divisions().size() > wanted) {
			continue;
		}
		taken += army->get_divisions().size();
		line.army_ids.push_back(army->unique_id);
		for (Front& other : fronts) {
			std::erase(other.army_ids, army->unique_id);
		}
		// It leaves the way to its place on the line: to the start of the breakthrough
		if (army->is_moving()) {
			army->clear_path();
		}
	}
	// The next province of the way the enemy holds; none: the divisions stand at its end
	ProvinceInstance* next = nullptr;
	std::size_t next_index = 0;
	for (std::size_t index = 0; index < line.provinces.size(); ++index) {
		if (line.provinces[index]->get_controller() != &country) {
			next = line.provinces[index];
			next_index = index;
			break;
		}
	}
	std::vector<ArmyInstance*> free;
	for (const unique_id_t army_id : line.army_ids) {
		UnitInstanceGroup* const group = unit_manager.get_unit_instance_group_by_unique_id(army_id);
		if (group != nullptr && !group->is_moving() && !group->is_in_combat()) {
			free.push_back(static_cast<ArmyInstance*>(group));
		}
	}
	// Where the divisions wait for the next step: the province of the way before it, or (the first step) the province of the front next to it
	ProvinceInstance* gather = nullptr;
	if (next == nullptr) {
		gather = line.provinces.back();
	} else if (next_index > 0) {
		gather = line.provinces[next_index - 1];
	} else {
		for (FrontEdge const& edge : front->edges) {
			if (edge.other == next) {
				gather = edge.own;
				break;
			}
		}
	}
	const auto next_to = [&](ProvinceInstance const& at, ProvinceInstance const& target) -> bool {
		for (ProvinceDefinition::adjacency_t const& adjacency : at.province_definition.get_adjacencies()) {
			if (&adjacency.get_to() == &target.province_definition && adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND) {
				return true;
			}
		}
		return false;
	};
	std::vector<ArmyInstance const*> attackers;
	for (ArmyInstance* army : free) {
		if (executed && next != nullptr && next_to(army->get_location(), *next) && country.is_at_war_with(*next->get_controller())) {
			attackers.push_back(army);
		} else if (gather != nullptr && &army->get_location() != gather) {
			unit_manager.order_move(*army, *gather, map_instance);
		}
	}
	// The step: all the divisions next to it, at the threshold of the aggressive mode (no minimum, no flanks)
	if (!attackers.empty()) {
		const BattleEstimate estimate = estimate_battle(instance_manager, attackers, *next);
		if (offensive_attacks(3, estimate, defines)) {
			for (ArmyInstance const* army : attackers) {
				unit_manager.order_move(*const_cast<ArmyInstance*>(army), *next, map_instance);
			}
		}
	}
}

void WarManager::break_out_encircled(InstanceManager& instance_manager, ArmyInstance& army) {
	if (army.is_moving() || army.is_in_combat() || army.empty()) {
		return;
	}
	// The nearest province (in steps over land) reached from a source of the supply; ties: the order of the neighbours
	MapInstance& map_instance = instance_manager.get_map_instance();
	CountryInstance const& country = army.get_country();
	std::unordered_map<ProvinceInstance const*, ProvinceInstance*> seen;
	std::deque<ProvinceInstance*> queue;
	seen.emplace(&army.get_location(), nullptr);
	queue.push_back(&army.get_location());
	ProvinceInstance* found = nullptr;
	while (!queue.empty() && found == nullptr && seen.size() < 200) {
		ProvinceInstance* const current = queue.front();
		queue.pop_front();
		for (ProvinceInstance* neighbour : get_land_neighbours(*current, map_instance)) {
			if (seen.emplace(neighbour, current).second) {
				if (is_supply_reached(country, *neighbour)) {
					found = neighbour;
					break;
				}
				queue.push_back(neighbour);
			}
		}
	}
	if (found != nullptr) {
		instance_manager.get_unit_instance_manager().order_move(army, *found, map_instance);
	}
}

void WarManager::execute_naval_invasion(InstanceManager& instance_manager, PlanLine& line, const bool executed) {
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	MapInstance& map_instance = instance_manager.get_map_instance();
	OpenVicWarDefines const& defines = get_war_defines();
	CountryInstance& country = *line.country;
	ArmyGroup const* const group = country.find_army_group(line.group_id);
	if (group == nullptr || line.port == nullptr) {
		return;
	}
	if (!executed) {
		line.prepared_days++;
	}
	const bool go = executed && line.prepared_days >= static_cast<std::uint32_t>(defines.naval_plan_prep_days);
	// The first target the enemy holds and the sea next to it (its port, else the first sea along its coast)
	ProvinceInstance* target = nullptr;
	for (ProvinceInstance* province : line.provinces) {
		CountryInstance const* const controller = province->get_controller();
		if (controller != nullptr && controller != &country && country.is_at_war_with(*controller)) {
			target = province;
			break;
		}
	}
	ProvinceInstance* sea = nullptr;
	if (target != nullptr) {
		if (target->province_definition.has_port() && target->province_definition.get_port_adjacent_province() != nullptr) {
			sea = &map_instance.get_province_instance_by_definition(*target->province_definition.get_port_adjacent_province());
		} else {
			for (ProvinceDefinition::adjacency_t const& adjacency : target->province_definition.get_adjacencies()) {
				if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::COASTAL && adjacency.get_to().is_water()) {
					sea = &map_instance.get_province_instance_by_definition(adjacency.get_to());
					break;
				}
			}
		}
	}
	const fixed_point_t reduction = group->planning * defines.naval_plan_penalty_reduction;
	// The fleets: carrying divisions of the marshal at the sea of the target land them; carrying them in the port sail there
	if (go && target != nullptr && sea != nullptr) {
		for (NavyInstance& navy : country.get_navies()) {
			if (navy.is_moving() || navy.is_in_combat()) {
				continue;
			}
			std::vector<ArmyInstance*> carried;
			for (ArmyInstance& army : navy.get_carried_armies()) {
				if (army.get_army_group_id() == line.group_id) {
					carried.push_back(&army);
				}
			}
			if (carried.empty()) {
				continue;
			}
			if (&navy.get_location() == sea) {
				for (ArmyInstance* army : carried) {
					navy.land_army(*army, *target, reduction);
				}
			} else if (&navy.get_location() == line.port) {
				unit_manager.order_move(navy, *sea, map_instance);
			}
		}
	}
	// The divisions of the marshal: on the land of the enemy they take it (the next target when this one is ours); the others gather in the port and,
	// executed, board a fleet there
	for (ArmyInstance& army : country.get_armies()) {
		if (army.get_army_group_id() != line.group_id || army.get_transport_navy() != nullptr || army.empty() || army.is_moving() || army.is_in_combat()
			|| army.is_manual_order()) {
			continue;
		}
		CountryInstance const* const here = army.get_location().get_controller();
		const bool on_enemy_land = here != nullptr && here != &country && country.is_at_war_with(*here);
		if (on_enemy_land) {
			continue;
		}
		const bool landed = std::find(line.provinces.begin(), line.provinces.end(), &army.get_location()) != line.provinces.end();
		if (landed) {
			// The target is taken: to the next one over land, when there is one next to it
			if (target != nullptr && target->province_definition.is_adjacent_to(army.get_location().province_definition)) {
				unit_manager.order_move(army, *target, map_instance);
			}
			continue;
		}
		if (&army.get_location() != line.port) {
			if (line.port->get_controller() == &country) {
				unit_manager.order_move(army, *line.port, map_instance);
			}
			continue;
		}
		if (go && target != nullptr) {
			for (NavyInstance& navy : country.get_navies()) {
				if (&navy.get_location() == line.port && !navy.is_moving() && !navy.is_in_combat() && navy.load_army(army)) {
					break;
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
	update_front_lines(instance_manager);
	// The divisions of the field marshals that are cut off try to break out to the land reached from the supply (P8)
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || country.get_army_groups().empty() || !country.is_at_war()) {
			continue;
		}
		for (ArmyInstance& army : country.get_armies()) {
			if (army.get_army_group_id() != 0 && army.is_cut_off()) {
				break_out_encircled(instance_manager, army);
			}
		}
	}
	// The breakthroughs take their divisions first (the fronts get the others)
	for (PlanLine& line : plan_lines) {
		if (line.type == PlanLine::type_t::BREAKTHROUGH && line.country->exists()) {
			ArmyGroup const* const group = line.country->find_army_group(line.group_id);
			execute_breakthrough(instance_manager, line, group != nullptr && group->plan_active);
		}
	}
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
		// of the plan, P5 of docs/WAR_ASSUMPTIONS.md, section 6)
		ArmyGroup const* const group = front.country->find_army_group(front.group_id);
		memory::vector<ProvinceInstance*> zone;
		memory::vector<ProvinceInstance*> targets;
		if (group != nullptr && group->plan_active) {
			for (PlanLine const& line : plan_lines) {
				if (line.type == PlanLine::type_t::OFFENSIVE && line.front_id == front.id) {
					for (ProvinceInstance* province : get_offensive_zone(instance_manager, line)) {
						if (std::find(zone.begin(), zone.end(), province) == zone.end()) {
							zone.push_back(province);
						}
					}
					targets.insert(targets.end(), line.provinces.begin(), line.provinces.end());
				}
			}
		}
		front.mode = zone.empty() ? Front::mode_t::DEFEND : Front::mode_t::ATTACK;
		// The divisions stand along the line; while an offensive is executed the free ones next to its targets attack
		distribute_divisions(instance_manager, front, group != nullptr ? group->integrity : 1);
		if (front.mode == Front::mode_t::ATTACK) {
			execute_offensive(instance_manager, front, zone, targets, group->aggression);
		}
	}

	// The line of defence and the naval invasion of a field marshal (his order when he has no front): his divisions go to the provinces of the line
	// and hold them (P9), to the port of the landing (P10)
	for (PlanLine& line : plan_lines) {
		if (line.type == PlanLine::type_t::OFFENSIVE || line.type == PlanLine::type_t::BREAKTHROUGH || !line.country->exists()
			|| line.country->find_army_group(line.group_id) == nullptr) {
			continue;
		}
		Front orders;
		orders.country = line.country;
		orders.group_id = line.group_id;
		orders.mode = Front::mode_t::DEFEND;
		if (line.type == PlanLine::type_t::NAVAL) {
			ArmyGroup const* const naval_group = line.country->find_army_group(line.group_id);
			execute_naval_invasion(instance_manager, line, naval_group != nullptr && naval_group->plan_active);
			continue;
		}
		for (ProvinceInstance* province : line.provinces) {
			if (province->get_controller() == line.country) {
				orders.provinces.push_back(province);
			}
		}
		join_group_armies(orders);
		ArmyGroup const* const group = line.country->find_army_group(line.group_id);
		distribute_divisions(instance_manager, orders, group != nullptr ? group->integrity : 1);
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
