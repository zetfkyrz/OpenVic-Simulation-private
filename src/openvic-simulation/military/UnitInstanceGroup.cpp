#include "UnitInstanceGroup.hpp"

#include <algorithm>
#include <queue>
#include <unordered_map>

#include <fmt/std.h>

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/defines/MilitaryDefines.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/military/Deployment.hpp"
#include "openvic-simulation/military/LeaderTrait.hpp"
#include "openvic-simulation/population/Culture.hpp"
#include "openvic-simulation/population/PopType.hpp"
#include "openvic-simulation/types/OrderedContainersMath.hpp"

using namespace OpenVic;

using enum unit_branch_t;

UnitInstanceGroup::UnitInstanceGroup(
	unique_id_t new_unique_id,
	unit_branch_t new_branch,
	std::string_view new_name,
	CountryInstance& new_country,
	ProvinceInstance& new_location
) : unique_id { new_unique_id },
	branch { new_branch },
	name { new_name },
	country { new_country },
	location { new_location } {
		new_country.add_unit_instance_group(*this);
	}

void UnitInstanceGroup::update_gamestate() {
	total_organisation = 0;
	total_max_organisation = 0;
	total_strength = 0;
	total_max_strength = 0;

	for (UnitInstance const& unit : units) {
		total_organisation += unit.get_organisation();
		total_max_organisation += unit.get_max_organisation();
		total_strength += unit.get_strength();
		total_max_strength += unit.get_max_strength();
	}
}

namespace {
	// Movement points gained per day = slowest unit's maximum_speed * this scale. A typical province-to-province
	// distance is a few tens of map units, so infantry (speed ~4) needs a handful of days per province.
	constexpr int32_t MOVEMENT_SPEED_SCALE = 3;
}

bool UnitInstanceGroup::can_country_enter(CountryInstance const& country, ProvinceInstance const& province) {
	CountryInstance const* const owner = province.get_owner();
	CountryInstance const* const controller = province.get_controller();

	if (owner == nullptr || owner == &country || controller == &country) {
		return true;
	}
	if (country.has_alliance_with(*owner) || country.has_military_access_to(*owner)) {
		return true;
	}
	if (country.is_at_war_with(*owner) || (controller != nullptr && country.is_at_war_with(*controller))) {
		return true;
	}
	return controller != nullptr && country.has_alliance_with(*controller);
}

void UnitInstanceGroup::set_path(memory::vector<std::reference_wrapper<ProvinceInstance>>&& new_path) {
	if (new_path.empty() || path.empty() || &new_path.front().get() != &path.front().get()) {
		movement_progress = fixed_point_t::_0;
	}
	path = std::move(new_path);
}

void UnitInstanceGroup::clear_path() {
	path.clear();
	movement_progress = fixed_point_t::_0;
}

void UnitInstanceGroup::tick() {
	if (path.empty() || units.empty()) {
		return;
	}

	// A group fighting a battle stays where it is
	if (in_combat) {
		return;
	}

	ProvinceInstance& next = path.front();

	if (!can_country_enter(country.get(), next)) {
		// Access was lost (peace, alliance ended...), stop
		clear_path();
		return;
	}

	ProvinceDefinition::adjacency_t const* const adjacency =
		location.get().province_definition.get_adjacency_to(next.province_definition);
	if (OV_unlikely(adjacency == nullptr)) {
		clear_path();
		return;
	}

	fixed_point_t speed = units.front().get().unit_type.maximum_speed;
	for (UnitInstance const& unit : units) {
		if (unit.unit_type.maximum_speed < speed) {
			speed = unit.unit_type.maximum_speed;
		}
	}
	if (speed <= fixed_point_t::_0) {
		speed = fixed_point_t::_1;
	}

	movement_progress += speed * MOVEMENT_SPEED_SCALE;

	if (movement_progress >= adjacency->get_distance()) {
		movement_progress -= adjacency->get_distance();
		set_location(next);
		path.erase(path.begin());
		if (path.empty()) {
			movement_progress = fixed_point_t::_0;
		}
	}
}

size_t UnitInstanceGroup::get_unit_count() const {
	return units.size();
}

bool UnitInstanceGroup::empty() const {
	return units.empty();
}

size_t UnitInstanceGroup::get_unit_category_count(UnitType::unit_category_t unit_category) const {
	return std::count_if(units.begin(), units.end(), [unit_category](UnitInstance const& unit) {
		return unit.unit_type.unit_category == unit_category;
	});
}

UnitType const* UnitInstanceGroup::get_display_unit_type() const {
	if (units.empty()) {
		return nullptr;
	}

	fixed_point_map_t<UnitType const*> weighted_unit_types;

	for (UnitInstance const& unit : units) {
		UnitType const& unit_type = unit.unit_type;
		weighted_unit_types[&unit_type] += unit_type.weighted_value;
	}

	return get_largest_item_tie_break(
		weighted_unit_types,
		[](UnitType const* lhs, UnitType const* rhs) -> bool {
			return lhs->weighted_value < rhs->weighted_value;
		}
	)->first;
}

bool UnitInstanceGroup::add_unit(UnitInstance& unit) {
	if (unit.get_branch() == branch) {
		units.emplace_back(unit);
		return true;
	} else {
		spdlog::error_s(
			"Trying to add {} unit \"{}\" to {} unit group \"{}\"",
			get_branch_name(unit.get_branch()), unit.get_name(), get_branch_name(branch), get_name()
		);
		return false;
	}
}

bool UnitInstanceGroup::remove_unit(UnitInstance const& unit) {
	auto it = std::find(units.begin(), units.end(), unit);

	if (it != units.end()) {
		units.erase(it);
		return true;
	} else {
		spdlog::error_s(
			"Trying to remove non-existent unit \"{}\" from unit group \"{}\"",
			unit.get_name(), get_name()
		);
		return false;
	}
}

void UnitInstanceGroup::set_name(std::string_view new_name) {
	name = new_name;
}

bool UnitInstanceGroup::set_location(ProvinceInstance& new_location) {
	bool ret = true;

	if (location != new_location) {
		ret &= location.get().remove_unit_instance_group(*this);
		location = new_location;
		ret &= new_location.add_unit_instance_group(*this);
	}

	return ret;
}

bool UnitInstanceGroup::set_country(CountryInstance& new_country) {
	bool ret = true;

	if (country != new_country) {
		ret &= country.get().remove_unit_instance_group(*this);
		country = new_country;
		ret &= new_country.add_unit_instance_group(*this);
	}

	return ret;
}

bool UnitInstanceGroup::set_leader(LeaderInstance* new_leader) {
	bool ret = true;

	if (leader != new_leader) {
		if (leader != nullptr) {
			if (leader->unit_instance_group == this) {
				leader->unit_instance_group = nullptr;
			} else {
				spdlog::error_s(
					"Mismatch between leader and unit instance group: group {} has leader {} but the leader has group {}",
					name,
					leader->get_name(),
					leader->get_unit_instance_group() != nullptr ? leader->get_unit_instance_group()->get_name() : "NULL"
				);
				ret = false;
			}
		}

		leader = nullptr;

		if (new_leader != nullptr) {
			if (OV_unlikely(new_leader->branch != branch)) {
				spdlog::error_s(
					"Trying to assign {} leader \"{}\" to {} unit group \"{}\"",
					get_branch_name(new_leader->branch), new_leader->get_name(), get_branch_name(branch), name
				);
				return false;
			}

			if (OV_unlikely(new_leader->country != country)) {
				spdlog::error_s(
					"Trying to assign {} \"{}\" of country \"{}\" to {} \"{}\" of country \"{}\"",
					get_branched_leader_name(new_leader->branch),
					new_leader->get_name(),
					new_leader->country,
					get_branched_unit_group_name(branch),
					name,
					country
				);
				return false;
			}

			if (new_leader->unit_instance_group != nullptr) {
				if (new_leader->unit_instance_group != this) {
					ret &= new_leader->unit_instance_group->set_leader(nullptr);
				} else {
					spdlog::error_s(
						"{} {} already leads {} {}!",
						get_branched_leader_name(new_leader->branch), new_leader->get_name(),get_branched_unit_group_name(branch), name
					);
					ret = false;
				}
			}

			leader = new_leader;
			leader->unit_instance_group = this;
		}
	}

	return ret;
}

// These values are only needed for the UI, so there's no need to pre-calculate and cache them for every unit on every update.
fixed_point_t UnitInstanceGroup::get_organisation_proportion() const {
	return total_max_organisation != 0 ? total_organisation / total_max_organisation : fixed_point_t::_0;
}

fixed_point_t UnitInstanceGroup::get_strength_proportion() const {
	return total_max_strength != 0 ? total_strength / total_max_strength : fixed_point_t::_0;
}

fixed_point_t UnitInstanceGroup::get_average_organisation() const {
	return !units.empty() ? total_organisation / static_cast<int32_t>(units.size()) : fixed_point_t::_0;
}

fixed_point_t UnitInstanceGroup::get_average_max_organisation() const {
	return !units.empty() ? total_max_organisation / static_cast<int32_t>(units.size()) : fixed_point_t::_0;
}

bool UnitInstanceGroup::is_moving() const {
	return !path.empty();
}

ProvinceInstance const* UnitInstanceGroup::get_movement_destination_province() const {
	return !path.empty() ? &path.back().get() : nullptr;
}

Date UnitInstanceGroup::get_movement_arrival_date() const {
	// TODO - calculate today + remaining movement cost / unit speed
	return {};
}

UnitInstanceGroupBranched<LAND>::UnitInstanceGroupBranched(
	unique_id_t new_unique_id,
	std::string_view new_name,
	CountryInstance& new_country,
	ProvinceInstance& new_location
) : UnitInstanceGroup { new_unique_id, LAND, new_name, new_country, new_location } {}

void UnitInstanceGroupBranched<LAND>::update_gamestate() {
	UnitInstanceGroup::update_gamestate();
}

void UnitInstanceGroupBranched<LAND>::tick() {
	UnitInstanceGroup::tick();
}

UnitInstanceGroupBranched<NAVAL>::UnitInstanceGroupBranched(
	unique_id_t new_unique_id,
	std::string_view new_name,
	CountryInstance& new_country,
	ProvinceInstance& new_location
) : UnitInstanceGroup { new_unique_id, NAVAL, new_name, new_country, new_location } {}

void UnitInstanceGroupBranched<NAVAL>::update_gamestate() {
	UnitInstanceGroup::update_gamestate();
}

void UnitInstanceGroupBranched<NAVAL>::tick() {
	UnitInstanceGroup::tick();
}

fixed_point_t UnitInstanceGroupBranched<NAVAL>::get_total_consumed_supply() const {
	fixed_point_t total_consumed_supply = 0;

	for (ShipInstance const& ship : get_ship_instances()) {
		total_consumed_supply += ship.get_ship_type().supply_consumption_score;
	}

	return total_consumed_supply;
}

Pop* UnitInstanceManager::recruit_pop_in(ProvinceInstance& province, const bool is_rebel) const {
	if (is_rebel) {
		for (auto& pop : province.get_mutable_pops()) {
			if (pop.get_rebel_type() != nullptr && pop.try_recruit()) {
				return &pop;
			}
		}
	} else {
		for (auto& pop : province.get_mutable_pops()) {
			if (pop.get_type().can_be_recruited && pop.try_recruit()) {
				/*
				Victoria 2 does not respect cultural restrictions when applying history.
				*/
				return &pop;
			}
		}
	}

	//fallback to understrength pops
	if (is_rebel) {
		for (auto& pop : province.get_mutable_pops()) {
			if (pop.get_rebel_type() != nullptr && pop.try_recruit_understrength()) {
				return &pop;
			}
		}
	} else {
		for (auto& pop : province.get_mutable_pops()) {
			if (pop.get_type().can_be_recruited && pop.try_recruit_understrength()) {
				/*
				Victoria 2 does not respect cultural restrictions when applying history.
				*/
				return &pop;
			}
		}
	}

	return nullptr;
}

template<unit_branch_t Branch>
UnitInstanceBranched<Branch>& UnitInstanceManager::generate_unit_instance(
	UnitDeployment<Branch> const& unit_deployment,
	MapInstance& map_instance,
	const bool is_rebel
) {
	UnitInstanceBranched<Branch>& unit_instance = *get_unit_instances<Branch>().insert(
		[this, &unit_deployment, &map_instance, is_rebel]() -> UnitInstanceBranched<Branch> {
			if constexpr (Branch == LAND) {
				RegimentDeployment const& regiment_deployment = unit_deployment;
				ProvinceInstance& province = map_instance.get_province_instance_by_definition(*regiment_deployment.get_home());
				Pop* const pop_ptr = recruit_pop_in(province, is_rebel);
				if (pop_ptr == nullptr) {		
					spdlog::warn_s(
						"Regiment {} in province {} lacks backing pop.", regiment_deployment.get_name(), province.get_identifier()
					);
				}

				return {
					unique_id_counter++,
					unit_deployment.get_name(),
					unit_deployment.type,
					pop_ptr,
					false
				};
			} else if constexpr (Branch == NAVAL) {
				return {
					unique_id_counter++,
					unit_deployment.get_name(),
					unit_deployment.type
				};
			}
		}()
	);

	unit_instance_map.emplace(unit_instance.unique_id, unit_instance);

	return unit_instance;
}

template<unit_branch_t Branch>
bool UnitInstanceManager::generate_unit_instance_group(
	MapInstance& map_instance, CountryInstance& country, UnitDeploymentGroup<Branch> const& unit_deployment_group
) {
	if (unit_deployment_group.get_units().empty()) {
		spdlog::error_s(
			"Trying to generate unit group \"{}\" with no units for country \"{}\"",
			unit_deployment_group.get_name(), country
		);
		return false;
	}
	
	ProvinceInstance& location = map_instance.get_province_instance_by_definition(unit_deployment_group.get_location());
	UnitInstanceGroupBranched<Branch>& unit_instance_group = *get_unit_instance_groups<Branch>().emplace(
		unique_id_counter++,
		unit_deployment_group.get_name(),
		country,
		location
	);
	unit_instance_group_map.emplace(unit_instance_group.unique_id, unit_instance_group);

	bool ret = true;

	for (UnitDeployment<Branch> const& unit_deployment : unit_deployment_group.get_units()) {
		ret &= unit_instance_group.add_unit(
			generate_unit_instance(unit_deployment, map_instance, country.is_rebel_country())
		);
	}

	ret &= unit_instance_group.set_country(country);

	if (unit_deployment_group.get_leader_index().has_value()) {
		memory::vector<std::reference_wrapper<LeaderInstance>>& leaders = country.get_leaders<Branch>();
		auto it = leaders.begin();

		advance(it, *unit_deployment_group.get_leader_index());

		if (it < leaders.end()) {
			ret &= unit_instance_group.set_leader(&it->get());
		} else {
			spdlog::error_s(
				"Invalid leader index {} for unit group \"{}\" for country \"{}\"",
				*unit_deployment_group.get_leader_index(), unit_deployment_group.get_name(), country
			);
			ret = false;
		}
	}

	return ret;
}

template<typename T>
void UnitInstanceManager::generate_leader(CountryInstance& country, T&& leader_base) {
	LeaderInstance& leader_instance = *leaders.emplace(
		unique_id_counter++,
		std::forward<T>(leader_base),
		country
	);
	leader_instance_map.emplace(leader_instance.unique_id, leader_instance);
	country.add_leader(leader_instance);

	if (leader_instance.get_picture().empty() && country.get_primary_culture() != nullptr) {
		leader_instance.set_picture(culture_manager.get_leader_picture_name(
			country.get_primary_culture()->group.get_leader(), leader_instance.branch
		));
	}
}

UnitInstanceManager::UnitInstanceManager(
	CultureManager const& new_culture_manager,
	LeaderTraitManager const& new_leader_trait_manager,
	MilitaryDefines const& new_military_defines
) : culture_manager { new_culture_manager },
	leader_trait_manager { new_leader_trait_manager },
	military_defines { new_military_defines } {}

bool UnitInstanceManager::generate_deployment(
	MapInstance& map_instance, CountryInstance& country, Deployment const& deployment
) {
	bool ret = true;

	for (LeaderBase const& leader : deployment.get_leaders()) {
		generate_leader(country, leader);
	}

	const auto generate_group = [this, &map_instance, &country, &ret, &deployment]<unit_branch_t Branch>() -> void {
		for (UnitDeploymentGroup<Branch> const& unit_deployment_group : deployment.get_unit_deployment_groups<Branch>()) {
			ret &= generate_unit_instance_group(map_instance, country, unit_deployment_group);
		}
	};

	generate_group.template operator()<LAND>();
	generate_group.template operator()<NAVAL>();

	return ret;
}

void UnitInstanceManager::update_gamestate() {
	for (ArmyInstance& army : armies) {
		army.update_gamestate();
	}
	for (NavyInstance& navy : navies) {
		navy.update_gamestate();
	}
}

void UnitInstanceManager::tick() {
	for (ArmyInstance& army : armies) {
		army.tick();
	}
	for (NavyInstance& navy : navies) {
		navy.tick();
	}
}

LeaderInstance* UnitInstanceManager::get_leader_instance_by_unique_id(unique_id_t unique_id) {
	const decltype(leader_instance_map)::const_iterator it = leader_instance_map.find(unique_id);

	if (it != leader_instance_map.end()) {
		return &it->second.get();
	} else {
		return nullptr;
	}
}

UnitInstance* UnitInstanceManager::get_unit_instance_by_unique_id(unique_id_t unique_id) {
	const decltype(unit_instance_map)::const_iterator it = unit_instance_map.find(unique_id);

	if (it != unit_instance_map.end()) {
		return &it->second.get();
	} else {
		return nullptr;
	}
}

UnitInstanceGroup* UnitInstanceManager::get_unit_instance_group_by_unique_id(unique_id_t unique_id) {
	const decltype(unit_instance_group_map)::const_iterator it = unit_instance_group_map.find(unique_id);

	if (it != unit_instance_group_map.end()) {
		return &it->second.get();
	} else {
		return nullptr;
	}
}

bool UnitInstanceManager::order_move(ArmyInstance& army, ProvinceInstance& destination, MapInstance& map_instance) {
	if (destination.province_definition.is_water() || army.empty()) {
		return false;
	}

	ProvinceInstance& start = army.get_location();
	if (&start == &destination) {
		army.clear_path();
		return true;
	}

	CountryInstance const& country = army.get_country();
	if (!UnitInstanceGroup::can_country_enter(country, destination)) {
		return false;
	}

	// Dijkstra over land adjacencies by distance
	using entry_t = std::pair<fixed_point_t, ProvinceInstance*>;
	struct entry_greater {
		bool operator()(entry_t const& lhs, entry_t const& rhs) const {
			return lhs.first > rhs.first;
		}
	};
	std::priority_queue<entry_t, std::vector<entry_t>, entry_greater> queue;
	std::unordered_map<ProvinceInstance*, fixed_point_t> cost;
	std::unordered_map<ProvinceInstance*, ProvinceInstance*> previous;

	cost[&start] = fixed_point_t::_0;
	queue.emplace(fixed_point_t::_0, &start);

	while (!queue.empty()) {
		const auto [current_cost, current] = queue.top();
		queue.pop();

		if (current == &destination) {
			break;
		}

		const auto cost_it = cost.find(current);
		if (cost_it != cost.end() && current_cost > cost_it->second) {
			continue;
		}

		for (ProvinceDefinition::adjacency_t const& adjacency : current->province_definition.get_adjacencies()) {
			if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND) {
				continue;
			}
			ProvinceDefinition const& to_definition = adjacency.get_to();
			if (to_definition.is_water()) {
				continue;
			}
			ProvinceInstance& to = map_instance.get_province_instance_by_definition(to_definition);
			if (!UnitInstanceGroup::can_country_enter(country, to)) {
				continue;
			}
			const fixed_point_t new_cost = current_cost + adjacency.get_distance();
			const auto it = cost.find(&to);
			if (it == cost.end() || new_cost < it->second) {
				cost[&to] = new_cost;
				previous[&to] = current;
				queue.emplace(new_cost, &to);
			}
		}
	}

	if (!previous.contains(&destination)) {
		return false;
	}

	memory::vector<std::reference_wrapper<ProvinceInstance>> path;
	for (ProvinceInstance* step = &destination; step != &start; step = previous.at(step)) {
		path.emplace_back(*step);
	}
	std::reverse(path.begin(), path.end());

	army.set_path(std::move(path));
	return true;
}

void UnitInstanceManager::destroy_unit(UnitInstance& unit, UnitInstanceGroup& group) {
	group.remove_unit(unit);
	unit_instance_map.erase(unit.unique_id);

	if (unit.get_branch() == LAND) {
		RegimentInstance& regiment = static_cast<RegimentInstance&>(unit);
		regiments.erase(regiments.get_iterator(&regiment));
	} else {
		ShipInstance& ship = static_cast<ShipInstance&>(unit);
		ships.erase(ships.get_iterator(&ship));
	}

	if (group.empty()) {
		destroy_group(group);
	}
}

void UnitInstanceManager::destroy_group(UnitInstanceGroup& group) {
	// Delete the units first, copy the list since destroying a unit edits the group
	memory::vector<UnitInstance*> group_units;
	for (UnitInstance& unit : group.get_units()) {
		group_units.push_back(&unit);
	}

	for (UnitInstance* unit : group_units) {
		group.remove_unit(*unit);
		unit_instance_map.erase(unit->unique_id);
		if (unit->get_branch() == LAND) {
			RegimentInstance& regiment = static_cast<RegimentInstance&>(*unit);
			regiments.erase(regiments.get_iterator(&regiment));
		} else {
			ShipInstance& ship = static_cast<ShipInstance&>(*unit);
			ships.erase(ships.get_iterator(&ship));
		}
	}

	group.set_leader(nullptr);
	group.get_location().remove_unit_instance_group(group);
	group.get_country().remove_unit_instance_group(group);
	unit_instance_group_map.erase(group.unique_id);

	if (group.branch == LAND) {
		ArmyInstance& army = static_cast<ArmyInstance&>(group);
		armies.erase(armies.get_iterator(&army));
	} else {
		NavyInstance& navy = static_cast<NavyInstance&>(group);
		navies.erase(navies.get_iterator(&navy));
	}
}

bool UnitInstanceManager::create_leader(
	CountryInstance& country,
	unit_branch_t branch,
	Date creation_date,
	std::string_view name,
	LeaderTrait const* personality,
	LeaderTrait const* background
) {
	const fixed_point_t leader_creation_cost = military_defines.get_leader_recruit_cost();
	if (country.get_leadership_point_stockpile() < leader_creation_cost) {
		spdlog::error_s(
			"Country \"{}\" does not have enough leadership points ({:.2}) to create a {} (cost: {:.2})",
			country,
			country.get_leadership_point_stockpile(),
			get_branched_leader_name(branch),
			leader_creation_cost
		);
		return false;
	}

	country.set_leadership_point_stockpile(country.get_leadership_point_stockpile() - leader_creation_cost);

	// TODO - replace with RNG
	static size_t item_selection_counter = 0;

	// Variable for storing a generated name if none is provided
	memory::string name_storage;
	if (name.empty()) {
		Culture const* culture = country.get_primary_culture();

		if (culture != nullptr) {
			std::string_view first_name, connector, last_name;

			if (!culture->get_first_names().empty()) {
				first_name = culture->get_first_names()[item_selection_counter++ % culture->get_first_names().size()];
			}

			if (!culture->get_last_names().empty()) {
				last_name = culture->get_last_names()[item_selection_counter++ % culture->get_last_names().size()];
			}

			if (!first_name.empty() && !last_name.empty()) {
				connector = " ";
			}

			name_storage = append_string_views(first_name, connector, last_name);
			name = name_storage;
		}
	}

	// TODO - use "noTrait" or "noPersonality" or "noBackground" if either is nullptr

	if (personality == nullptr && !leader_trait_manager.get_personality_traits().empty()) {
		personality = leader_trait_manager.get_personality_traits()[
			item_selection_counter++ % leader_trait_manager.get_personality_traits().size()
		];
	}

	if (background == nullptr && !leader_trait_manager.get_background_traits().empty()) {
		background = leader_trait_manager.get_background_traits()[
			item_selection_counter++ % leader_trait_manager.get_background_traits().size()
		];
	}

	// TODO - make starting prestige a random proportion of the maximum random prestige
	const fixed_point_t starting_prestige =
		military_defines.get_leader_max_random_prestige() * static_cast<int32_t>(item_selection_counter++ % 11) / 10;

	generate_leader(country, LeaderBase{
		name,
		branch,
		creation_date,
		personality,
		background,
		starting_prestige,
		{} // No picture, will be set up by generate_leader
	});

	return true;
}