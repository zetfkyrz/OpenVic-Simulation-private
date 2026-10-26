#include "TradeNetwork.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/economy/BuildingInstance.hpp"
#include "openvic-simulation/economy/BuildingType.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceDefinition.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

TradeNetwork::TradeNetwork(OpenVicTradeDefines const& new_defines, const std::size_t new_country_count)
  : defines { new_defines },
	country_count { new_country_count },
	routes((new_country_count + 1) * (new_country_count + 1)),
	sea_capacity(new_country_count + 1, fixed_point_t::_0),
	land_crossings(new_country_count + 1, 0),
	best_rail(new_country_count + 1, fixed_point_t::_0),
	today_stats((new_country_count + 1) * STAT_COUNT),
	yesterday_stats(new_country_count + 1),
	today_pair_flow((new_country_count + 1) * (new_country_count + 1)),
	yesterday_pair_flow((new_country_count + 1) * (new_country_count + 1), fixed_point_t::_0) {}

bool TradeNetwork::is_enabled() const {
	return defines.enabled != 0;
}

fixed_point_t TradeNetwork::get_spot_cost() const {
	return defines.spot_cost.fixed();
}

void TradeNetwork::add_stat(const country_index_t country, const std::size_t stat, const fixed_point_t amount) {
	if (amount == fixed_point_t::_0) {
		return;
	}
	today_stats[type_safe::get(country) * STAT_COUNT + stat].fetch_add(amount.get_raw_value(), std::memory_order_relaxed);
}

void TradeNetwork::add_pair_flow(const country_index_t from, const country_index_t to, const fixed_point_t value) {
	if (value <= fixed_point_t::_0) {
		return;
	}
	today_pair_flow[type_safe::get(from) * (country_count + 1) + type_safe::get(to)].fetch_add(value.get_raw_value(), std::memory_order_relaxed);
}

void TradeNetwork::add_fee(const country_index_t country, const fixed_point_t amount) {
	if (amount <= fixed_point_t::_0 || type_safe::get(country) >= countries.size()) {
		return;
	}
	countries[type_safe::get(country)].add_transport_fee(amount);
}

void TradeNetwork::begin_day() {
	for (std::size_t i = 0; i < today_pair_flow.size(); ++i) {
		yesterday_pair_flow[i] = fixed_point_t::parse_raw(today_pair_flow[i].exchange(0, std::memory_order_relaxed));
	}
	for (std::size_t node = 0; node <= country_count; ++node) {
		const auto take = [this, node](const std::size_t stat) -> fixed_point_t {
			return fixed_point_t::parse_raw(today_stats[node * STAT_COUNT + stat].exchange(0, std::memory_order_relaxed));
		};
		country_stats_t& stats = yesterday_stats[node];
		stats.export_value = take(0);
		stats.import_value = take(1);
		stats.internal_fees = take(2);
		stats.export_fees = take(3);
		stats.unmet_import_value = take(4);
		stats.land_export_value = take(5);
		stats.sea_export_value = take(6);
	}
}

void TradeNetwork::build_zones(InstanceManager& instance_manager) {
	MapInstance& map_instance = instance_manager.get_map_instance();
	const std::size_t province_count = map_instance.get_province_instances().size();
	zone_of_province.assign(province_count, -1);
	zone_count = 0;
	std::vector<ProvinceInstance const*> zones;
	for (ProvinceInstance const& province : map_instance.get_province_instances()) {
		if (province.province_definition.is_water()) {
			zone_of_province[type_safe::get(province.province_definition.index)] = static_cast<std::int32_t>(zone_count++);
			zones.push_back(&province);
		}
	}

	// Weighted graph of the neighbouring sea zones, the weight is the distance between the centres in pixels
	const auto centre_distance = [](ProvinceDefinition const& a, ProvinceDefinition const& b) -> std::int32_t {
		const double dx = static_cast<double>(a.get_centre().x) - static_cast<double>(b.get_centre().x);
		const double dy = static_cast<double>(a.get_centre().y) - static_cast<double>(b.get_centre().y);
		return std::max<std::int32_t>(1, static_cast<std::int32_t>(std::sqrt(dx * dx + dy * dy)));
	};
	struct edge_t {
		std::int32_t to;
		std::int32_t length;
	};
	std::vector<std::vector<edge_t>> graph(zone_count);
	for (std::size_t z = 0; z < zone_count; ++z) {
		ProvinceDefinition const& from = zones[z]->province_definition;
		for (ProvinceDefinition::adjacency_t const& adjacency : from.get_adjacencies()) {
			if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::WATER) {
				continue;
			}
			const std::int32_t other = zone_of_province[type_safe::get(adjacency.get_to().index)];
			if (other >= 0) {
				graph[z].push_back({ other, centre_distance(from, adjacency.get_to()) });
			}
		}
	}

	// Shortest distances from every zone (the zones are few, a plain search is enough)
	constexpr std::int32_t INF = std::numeric_limits<std::int32_t>::max();
	zone_distance.assign(zone_count * zone_count, -1);
	std::vector<std::int32_t> dist(zone_count);
	std::vector<char> done(zone_count);
	for (std::size_t source = 0; source < zone_count; ++source) {
		std::fill(dist.begin(), dist.end(), INF);
		std::fill(done.begin(), done.end(), 0);
		dist[source] = 0;
		for (;;) {
			std::int32_t best = -1;
			for (std::size_t z = 0; z < zone_count; ++z) {
				if (!done[z] && dist[z] != INF && (best < 0 || dist[z] < dist[best])) {
					best = static_cast<std::int32_t>(z);
				}
			}
			if (best < 0) {
				break;
			}
			done[best] = 1;
			for (edge_t const& edge : graph[best]) {
				if (dist[best] + edge.length < dist[edge.to]) {
					dist[edge.to] = dist[best] + edge.length;
				}
			}
		}
		for (std::size_t z = 0; z < zone_count; ++z) {
			if (dist[z] != INF) {
				zone_distance[source * zone_count + z] = dist[z];
			}
		}
	}
	zones_built = true;
}

void TradeNetwork::update(InstanceManager& instance_manager) {
	MapInstance& map_instance = instance_manager.get_map_instance();
	BuildingTypeManager const& building_type_manager =
		instance_manager.definition_manager.get_economy_manager().get_building_type_manager();
	const std::size_t n = country_count + 1;

	if (!zones_built) {
		build_zones(instance_manager);
	}

	BuildingType const* const railroad = building_type_manager.get_infrastructure_building_type();
	BuildingType const* const naval_base = building_type_manager.get_port_building_type();
	const bool has_railroad = railroad != nullptr && railroad->province_building_index.has_value();
	const bool has_naval_base = naval_base != nullptr && naval_base->province_building_index.has_value();
	const fixed_point_t max_rail_level = has_railroad
		? fixed_point_t(std::max<std::int32_t>(1, type_safe::get(railroad->max_level))) : fixed_point_t::_1;

	// ---- Provinces: the railroad, and the freight to the market of the own country. The market works at the level of the region (the state),
	// not of the province: the freight and the share of the output that the workers keep depend on the average railroad of the state.
	// The railroad of the province itself counts only at the border (a railroad carries goods over it only if both sides have one).
	const fixed_point_t freight_range = defines.internal_freight_max.fixed() - defines.internal_freight_min.fixed();
	raw_rail.assign(map_instance.get_province_instances().size(), fixed_point_t::_0);
	std::unordered_map<State const*, std::pair<fixed_point_t, std::int32_t>> state_rail;
	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		fixed_point_t fraction = fixed_point_t::_0;
		if (has_railroad && !province.province_definition.is_water()) {
			const fixed_point_t level = type_safe::get(province.get_buildings()[railroad->province_building_index.value()].get_level());
			fraction = std::min(fixed_point_t::_1, level / max_rail_level);
		}
		raw_rail[type_safe::get(province.province_definition.index)] = fraction;
		if (province.get_state() != nullptr && !province.province_definition.is_water()) {
			auto& entry = state_rail[province.get_state()];
			entry.first += fraction;
			entry.second++;
		}
	}
	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		fixed_point_t fraction = raw_rail[type_safe::get(province.province_definition.index)];
		if (province.get_state() != nullptr) {
			const auto it = state_rail.find(province.get_state());
			if (it != state_rail.end() && it->second.second > 0) {
				fraction = it->second.first / it->second.second;
			}
		}
		province.set_trade_data(
			fraction,
			std::max(fixed_point_t::_0, defines.internal_freight_max.fixed() - freight_range * fraction)
		);
	}

	// ---- Borders (land crossings) and ports (sea)
	std::vector<fixed_point_t> land_cap(n * n, fixed_point_t::_0), land_rail(n * n, fixed_point_t::_0);
	std::fill(sea_capacity.begin(), sea_capacity.end(), fixed_point_t::_0);
	std::fill(land_crossings.begin(), land_crossings.end(), 0);
	std::fill(best_rail.begin(), best_rail.end(), fixed_point_t::_0);
	std::vector<fixed_point_t> port_levels(n, fixed_point_t::_0);
	std::vector<std::int32_t> port_count(n, 0);
	std::vector<std::vector<std::int32_t>> country_zones(n);

	for (ProvinceInstance const& province : map_instance.get_province_instances()) {
		CountryInstance const* const owner = province.get_owner();
		if (owner == nullptr || province.province_definition.is_water()) {
			continue;
		}
		const std::size_t o = type_safe::get(owner->index);
		if (o >= country_count) {
			continue;
		}

		if (has_naval_base && province.province_definition.has_port() && province.province_definition.get_port_adjacent_province() != nullptr) {
			const std::int32_t zone = zone_of_province[type_safe::get(province.province_definition.get_port_adjacent_province()->index)];
			if (zone >= 0) {
				const std::int32_t level = type_safe::get(
					province.get_buildings()[naval_base->province_building_index.value()].get_level()
				);
				sea_capacity[o] += fixed_point_t(defines.sea_port_base_capacity) + fixed_point_t(defines.sea_capacity_per_naval_base_level) * level;
				port_levels[o] += level;
				port_count[o]++;
				if (std::find(country_zones[o].begin(), country_zones[o].end(), zone) == country_zones[o].end()) {
					country_zones[o].push_back(zone);
				}
			}
		}

		for (ProvinceDefinition::adjacency_t const& adjacency : province.province_definition.get_adjacencies()) {
			if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND || adjacency.get_to().is_water()) {
				continue;
			}
			ProvinceInstance const* const other_province = map_instance.get_province_instance_by_index(adjacency.get_to().index);
			CountryInstance const* const other_owner = other_province == nullptr ? nullptr : other_province->get_owner();
			if (other_owner == nullptr || other_owner == owner) {
				continue;
			}
			const std::size_t d = type_safe::get(other_owner->index);
			if (d >= country_count) {
				continue;
			}
			// Both sides must have the railroad for it to carry goods over the border
			const fixed_point_t rail = std::min(
				raw_rail[type_safe::get(province.province_definition.index)], raw_rail[type_safe::get(other_province->province_definition.index)]
			);
			land_cap[o * n + d] += fixed_point_t(defines.land_capacity_base) + fixed_point_t(defines.land_capacity_per_rail_level) * (rail * max_rail_level);
			land_rail[o * n + d] = std::max(land_rail[o * n + d], rail);
			land_crossings[o]++;
			best_rail[o] = std::max(best_rail[o], rail);
		}
	}

	// ---- The direct links
	std::vector<route_t> direct(n * n);
	const fixed_point_t max_route_cost = defines.max_route_cost.fixed();
	const fixed_point_t transit_cost = defines.transit_cost_per_hop.fixed();
	const double map_width = std::max<double>(1.0, static_cast<double>(instance_manager.definition_manager.get_map_definition().get_width()));
	for (std::size_t a = 0; a < country_count; ++a) {
		for (std::size_t b = 0; b < country_count; ++b) {
			if (a == b) {
				continue;
			}
			route_t& link = direct[a * n + b];

			if (land_cap[a * n + b] > fixed_point_t::_0) {
				link.cost = std::max(fixed_point_t::_0, defines.land_cost_base.fixed() * (fixed_point_t::_1 - defines.land_rail_cost_relief.fixed() * land_rail[a * n + b]));
				link.capacity_value = land_cap[a * n + b];
				link.mode = mode_t::LAND;
				link.hops = 1;
			}

			if (sea_capacity[a] > fixed_point_t::_0 && sea_capacity[b] > fixed_point_t::_0) {
				std::int32_t distance = -1;
				for (const std::int32_t za : country_zones[a]) {
					for (const std::int32_t zb : country_zones[b]) {
						const std::int32_t d = zone_distance[static_cast<std::size_t>(za) * zone_count + static_cast<std::size_t>(zb)];
						if (d >= 0 && (distance < 0 || d < distance)) {
							distance = d;
						}
					}
				}
				if (distance >= 0) {
					fixed_point_t cost = defines.sea_cost_base.fixed()
						+ defines.sea_cost_per_map_width.fixed() * fixed_point_t(static_cast<std::int32_t>(distance)) / static_cast<std::int32_t>(map_width);
					const fixed_point_t average_level = (port_levels[a] / std::max(1, port_count[a]) + port_levels[b] / std::max(1, port_count[b])) / 2;
					const fixed_point_t relief = std::min(fixed_point_t::_1, average_level / defines.sea_naval_base_relief_levels);
					cost = cost * (fixed_point_t::_1 - defines.sea_naval_base_cost_relief.fixed() * relief);
					const fixed_point_t capacity = std::min(sea_capacity[a], sea_capacity[b]) * defines.sea_pair_share.fixed();
					if (link.mode == mode_t::NONE) {
						link.cost = cost;
						link.capacity_value = capacity;
						link.mode = mode_t::SEA;
						link.hops = 1;
					} else {
						// Both ways: the cheaper one sets the cost and the mode, the capacities add up
						if (cost < link.cost) {
							link.cost = cost;
							link.mode = mode_t::SEA;
						}
						link.capacity_value += capacity;
					}
				}
			}

			if (link.mode != mode_t::NONE && link.cost > max_route_cost) {
				link = route_t {};
			}
		}
	}

	// ---- Routes: at most MAX_ROUTE_HOPS links, other countries may be passed (layers of the Bellman-Ford search)
	for (route_t& route : routes) {
		route = route_t {};
	}
	std::vector<route_t> previous(n), current(n);
	for (std::size_t s = 0; s < country_count; ++s) {
		for (std::size_t v = 0; v < country_count; ++v) {
			previous[v] = direct[s * n + v];
			routes[s * n + v] = previous[v];
		}
		for (std::int32_t hop = 2; hop <= defines.max_route_hops; ++hop) {
			bool any = false;
			for (std::size_t v = 0; v < country_count; ++v) {
				current[v] = route_t {};
			}
			for (std::size_t u = 0; u < country_count; ++u) {
				if (u == s || previous[u].mode == mode_t::NONE) {
					continue;
				}
				for (std::size_t v = 0; v < country_count; ++v) {
					if (v == s || v == u) {
						continue;
					}
					route_t const& link = direct[u * n + v];
					if (link.mode == mode_t::NONE) {
						continue;
					}
					const fixed_point_t cost = previous[u].cost + link.cost + transit_cost;
					if (cost > max_route_cost) {
						continue;
					}
					route_t& best_here = current[v];
					if (best_here.mode == mode_t::NONE || cost < best_here.cost) {
						best_here.cost = cost;
						best_here.capacity_value = std::min(previous[u].capacity_value, link.capacity_value);
						best_here.hops = static_cast<std::uint8_t>(hop);
						best_here.mode = previous[u].mode == link.mode ? link.mode : mode_t::MIXED;
						any = true;
					}
				}
			}
			if (!any) {
				break;
			}
			for (std::size_t v = 0; v < country_count; ++v) {
				route_t& best = routes[s * n + v];
				if (current[v].mode != mode_t::NONE && (best.mode == mode_t::NONE || current[v].cost < best.cost)) {
					best = current[v];
				}
			}
			previous = current;
		}
	}

	// ---- The outside: provinces that nobody owns trade with everyone at a fixed price, without a limit
	const std::size_t outside = country_count;
	for (std::size_t c = 0; c < country_count; ++c) {
		route_t outside_route;
		outside_route.cost = defines.outside_cost.fixed();
		outside_route.capacity_value = -1;
		outside_route.hops = 1;
		outside_route.mode = mode_t::SEA;
		routes[c * n + outside] = outside_route;
		routes[outside * n + c] = outside_route;
	}

	ready = true;
}
