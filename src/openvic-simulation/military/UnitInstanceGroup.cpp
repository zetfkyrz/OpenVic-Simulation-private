#include "UnitInstanceGroup.hpp"

#include <algorithm>
#include <queue>
#include <unordered_map>

#include <fmt/std.h>

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/defines/MilitaryDefines.hpp"
#include "openvic-simulation/economy/BuildingInstance.hpp"
#include "openvic-simulation/economy/BuildingType.hpp"
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
		// The province keeps the list of the unit groups in it, which is used to draw them and for combat
		new_location.add_unit_instance_group(*this);
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

bool UnitInstanceGroup::can_navy_enter(CountryInstance const& country, ProvinceInstance const& province) {
	if (province.province_definition.is_water()) {
		return true;
	}
	if (!province.province_definition.has_port()) {
		return false;
	}
	// Ships dock only in the ports of the country and of its friends
	CountryInstance const* const owner = province.get_owner();
	if (owner == nullptr) {
		return false;
	}
	return owner == &country || province.get_controller() == &country || country.has_alliance_with(*owner)
		|| country.has_military_access_to(*owner);
}

namespace {
	// Distance used between a port and its sea, they are not connected by a measured adjacency
	constexpr int32_t PORT_STEP_DISTANCE = 10;
}

fixed_point_t UnitInstanceGroup::get_step_distance(ProvinceInstance const& from, ProvinceInstance const& to) const {
	ProvinceDefinition::adjacency_t const* const adjacency = from.province_definition.get_adjacency_to(to.province_definition);
	if (branch == LAND) {
		return adjacency != nullptr ? adjacency->get_distance() : fixed_point_t(-1);
	}

	// Ships: sea to sea, or a port and the sea next to it
	const bool from_water = from.province_definition.is_water();
	const bool to_water = to.province_definition.is_water();
	if (from_water && to_water) {
		return adjacency != nullptr ? adjacency->get_distance() : fixed_point_t(-1);
	}
	ProvinceDefinition const& port = from_water ? to.province_definition : from.province_definition;
	ProvinceDefinition const& sea = from_water ? from.province_definition : to.province_definition;
	if (!port.has_port() || port.get_port_adjacent_province() != &sea) {
		return fixed_point_t(-1);
	}
	return adjacency != nullptr ? adjacency->get_distance() : fixed_point_t(PORT_STEP_DISTANCE);
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

	if (branch == LAND ? !can_country_enter(country.get(), next) : !can_navy_enter(country.get(), next)) {
		// Access was lost (peace, alliance ended...), stop
		clear_path();
		return;
	}

	const fixed_point_t step_distance = get_step_distance(location.get(), next);
	if (OV_unlikely(step_distance < fixed_point_t::_0)) {
		clear_path();
		return;
	}

	const fixed_point_t speed = get_movement_speed();

	movement_progress += speed * MOVEMENT_SPEED_SCALE;

	if (movement_progress >= step_distance) {
		movement_progress -= step_distance;
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

fixed_point_t UnitInstanceGroup::get_movement_speed() const {
	if (units.empty()) {
		return fixed_point_t::_1;
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
	return speed;
}

int32_t UnitInstanceGroup::get_movement_days_left() const {
	if (path.empty()) {
		return 0;
	}

	// The distance along the path left to cover divided by the distance covered each day
	fixed_point_t remaining = -movement_progress;
	ProvinceInstance const* from = &location.get();
	for (ProvinceInstance const& to : path) {
		const fixed_point_t distance = get_step_distance(*from, to);
		if (distance < fixed_point_t::_0) {
			break;
		}
		remaining += distance;
		from = &to;
	}

	const fixed_point_t per_day = get_movement_speed() * MOVEMENT_SPEED_SCALE;
	if (remaining <= fixed_point_t::_0) {
		return 1;
	}
	return std::max<int32_t>(1, (remaining / per_day).ceil<int32_t>());
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

void UnitInstanceGroupBranched<LAND>::reset_dig_in() {
	idle_days = 0;
	dig_in_level = 0;
}

void UnitInstanceGroupBranched<LAND>::advance_dig_in(const int32_t days_per_level, const int32_t cap) {
	if (is_moving() || is_in_combat()) {
		reset_dig_in();
		return;
	}

	if (days_per_level <= 0 || dig_in_level >= cap) {
		return;
	}

	if (++idle_days >= days_per_level) {
		idle_days = 0;
		++dig_in_level;
	}
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
	ProvinceInstance* const old_location = &get_location();
	UnitInstanceGroup::tick();

	// The armies on board travel with the ships
	if (&get_location() != old_location) {
		for (ArmyInstance& army : carried_armies) {
			army.clear_path();
			army.set_location(get_location());
		}
	}
}

size_t UnitInstanceGroupBranched<NAVAL>::get_transport_capacity() const {
	size_t capacity = 0;
	for (ShipInstance const& ship : get_ship_instances()) {
		if (ship.get_ship_type().is_transport) {
			capacity += REGIMENTS_PER_TRANSPORT;
		}
	}
	return capacity;
}

size_t UnitInstanceGroupBranched<NAVAL>::get_carried_regiment_count() const {
	size_t count = 0;
	for (ArmyInstance const& army : carried_armies) {
		count += army.get_unit_count();
	}
	return count;
}

bool UnitInstanceGroupBranched<NAVAL>::load_army(ArmyInstance& army) {
	if (&army.get_country() != &get_country() || &army.get_location() != &get_location() || army.is_moving()
		|| army.is_in_combat() || army.get_transport_navy() != nullptr || get_location().province_definition.is_water()) {
		return false;
	}
	if (get_carried_regiment_count() + army.get_unit_count() > get_transport_capacity()) {
		return false;
	}
	army.set_transport_navy(this);
	army.clear_path();
	carried_armies.emplace_back(army);
	return true;
}

void UnitInstanceGroupBranched<NAVAL>::forget_carried_army(ArmyInstance const& army) {
	carried_armies.erase(
		std::remove_if(carried_armies.begin(), carried_armies.end(), [&army](std::reference_wrapper<ArmyInstance> other) -> bool {
			return &other.get() == &army;
		}),
		carried_armies.end()
	);
}

bool UnitInstanceGroupBranched<NAVAL>::unload_army(ArmyInstance& army) {
	if (army.get_transport_navy() != this || get_location().province_definition.is_water()) {
		return false;
	}
	army.set_transport_navy(nullptr);
	forget_carried_army(army);
	return true;
}

bool UnitInstanceGroupBranched<NAVAL>::land_army(ArmyInstance& army, ProvinceInstance& target) {
	if (army.get_transport_navy() != this || !get_location().province_definition.is_water()
		|| target.province_definition.is_water() || is_in_combat()) {
		return false;
	}
	// The coast must touch the sea the ships are in
	ProvinceDefinition::adjacency_t const* const adjacency =
		get_location().province_definition.get_adjacency_to(target.province_definition);
	const bool touches_coast = adjacency != nullptr && adjacency->get_type() == ProvinceDefinition::adjacency_t::type_t::COASTAL;
	const bool is_port_of_sea = target.province_definition.has_port()
		&& target.province_definition.get_port_adjacent_province() == &get_location().province_definition;
	if (!touches_coast && !is_port_of_sea) {
		return false;
	}
	if (!can_country_enter(get_country(), target)) {
		return false;
	}

	army.set_transport_navy(nullptr);
	forget_carried_army(army);
	army.clear_path();
	army.set_location(target);
	// The men are tired after the landing
	for (RegimentInstance& regiment : army.get_regiment_instances()) {
		regiment.take_damage(regiment.get_organisation() * (LANDING_ORGANISATION_DIVISOR - 1) / LANDING_ORGANISATION_DIVISOR, fixed_point_t::_0);
	}
	army.reset_dig_in();
	return true;
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
		army.advance_dig_in(
			static_cast<int32_t>(military_defines.get_dig_in_increase_each_days().to_int()),
			army.get_country().get_dig_in_cap()
		);
	}

	// Recruitment: the regiment appears in the province when the time is up
	for (size_t i = 0; i < recruitment_orders.size();) {
		RecruitmentOrder& order = recruitment_orders[i];
		if (--order.days_left > 0) {
			++i;
			continue;
		}

		CountryInstance& country = *order.country;
		ProvinceInstance& province = *order.province;
		RegimentType const* const regiment_type = order.regiment_type;
		ShipType const* const ship_type = order.ship_type;
		Pop* const pop = order.pop;
		recruitment_orders.erase(recruitment_orders.begin() + i);

		if (ship_type != nullptr) {
			// A ship joins a fleet standing in the port or forms a new one
			const unique_id_t ship_id = unique_id_counter++;
			ShipInstance& ship = *ships.insert(ShipInstance {
				ship_id, fmt::format("{} {}", ship_type->get_identifier(), ship_id), *ship_type
			});
			unit_instance_map.emplace(ship.unique_id, ship);

			NavyInstance* fleet = nullptr;
			for (NavyInstance& navy : province.get_navies()) {
				if (&navy.get_country() == &country && !navy.is_moving() && !navy.is_in_combat()) {
					fleet = &navy;
					break;
				}
			}
			if (fleet == nullptr) {
				const unique_id_t navy_id = unique_id_counter++;
				fleet = &*navies.emplace(navy_id, fmt::format("Fleet {}", navy_id), country, province);
				unit_instance_group_map.emplace(fleet->unique_id, *fleet);
			}
			fleet->add_unit(ship);
			fleet->update_gamestate();
			continue;
		}

		const unique_id_t regiment_id = unique_id_counter++;
		RegimentInstance& regiment = *regiments.insert(RegimentInstance {
			regiment_id, fmt::format("{} {}", regiment_type->get_identifier(), regiment_id), *regiment_type, pop, false
		});
		unit_instance_map.emplace(regiment.unique_id, regiment);

		// Joins a standing army of the country in the province or forms a new one
		ArmyInstance* target = nullptr;
		for (ArmyInstance& army : province.get_armies()) {
			if (&army.get_country() == &country && !army.is_moving() && !army.is_in_combat()
				&& army.get_transport_navy() == nullptr) {
				target = &army;
				break;
			}
		}
		if (target == nullptr) {
			const unique_id_t army_id = unique_id_counter++;
			target = &*armies.emplace(army_id, fmt::format("Army {}", army_id), country, province);
			unit_instance_group_map.emplace(target->unique_id, *target);
		}
		target->add_unit(regiment);
		target->update_gamestate();
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

bool UnitInstanceManager::order_move(
	UnitInstanceGroup& group, ProvinceInstance& destination, MapInstance& map_instance, const bool append
) {
	if (group.empty()) {
		return false;
	}
	const bool naval = group.branch == NAVAL;
	// Armies carried by a navy move with it
	if (!naval && static_cast<ArmyInstance&>(group).get_transport_navy() != nullptr) {
		return false;
	}
	if (naval ? !(destination.province_definition.is_water() || destination.province_definition.has_port())
		: destination.province_definition.is_water()) {
		return false;
	}

	const bool extend_route = append && group.is_moving();
	ProvinceInstance& start = extend_route ? group.get_path().back().get() : group.get_location();
	if (&start == &destination) {
		if (!extend_route) {
			group.clear_path();
		}
		return true;
	}

	CountryInstance const& country = group.get_country();
	if (naval ? !UnitInstanceGroup::can_navy_enter(country, destination) : !UnitInstanceGroup::can_country_enter(country, destination)) {
		return false;
	}

	// Dijkstra over the adjacencies by distance
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

	const auto relax = [&](ProvinceInstance* current, fixed_point_t current_cost, ProvinceInstance& to) -> void {
		const fixed_point_t step = group.get_step_distance(*current, to);
		if (step < fixed_point_t::_0) {
			return;
		}
		const fixed_point_t new_cost = current_cost + step;
		const auto it = cost.find(&to);
		if (it == cost.end() || new_cost < it->second) {
			cost[&to] = new_cost;
			previous[&to] = current;
			queue.emplace(new_cost, &to);
		}
	};

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

		if (!naval) {
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
				relax(current, current_cost, to);
			}
		} else if (current->province_definition.is_water()) {
			for (ProvinceDefinition::adjacency_t const& adjacency : current->province_definition.get_adjacencies()) {
				ProvinceDefinition const& to_definition = adjacency.get_to();
				if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::IMPASSABLE) {
					continue;
				}
				ProvinceInstance& to = map_instance.get_province_instance_by_definition(to_definition);
				if (to_definition.is_water()) {
					relax(current, current_cost, to);
				} else if (&to == &destination && to_definition.has_port() && to_definition.get_port_adjacent_province() == &current->province_definition
					&& UnitInstanceGroup::can_navy_enter(country, to)) {
					// A port is only a destination, never a way through
					relax(current, current_cost, to);
				}
			}
		} else if (ProvinceDefinition const* sea = current->province_definition.get_port_adjacent_province()) {
			// Leaving the port (only the start of the route can be a port)
			relax(current, current_cost, map_instance.get_province_instance_by_definition(*sea));
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

	if (extend_route) {
		memory::vector<std::reference_wrapper<ProvinceInstance>> full_path;
		for (ProvinceInstance& step : group.get_path()) {
			full_path.emplace_back(step);
		}
		full_path.insert(full_path.end(), path.begin(), path.end());
		path = std::move(full_path);
	}

	group.set_path(std::move(path));
	return true;
}

ProvinceInstance* UnitInstanceManager::find_nearest_port(
	CountryInstance const& country, ProvinceInstance& from, MapInstance& map_instance
) {
	// Breadth first search over the sea (and the sea next to the start) until a port the country can dock in is found
	std::queue<ProvinceInstance*> queue;
	std::unordered_map<ProvinceInstance*, bool> seen;

	ProvinceInstance* start = &from;
	if (!from.province_definition.is_water()) {
		if (from.province_definition.has_port() && UnitInstanceGroup::can_navy_enter(country, from)) {
			return &from;
		}
		if (ProvinceDefinition const* sea = from.province_definition.get_port_adjacent_province()) {
			start = &map_instance.get_province_instance_by_definition(*sea);
		} else {
			return nullptr;
		}
	}
	queue.push(start);
	seen[start] = true;

	while (!queue.empty()) {
		ProvinceInstance* const current = queue.front();
		queue.pop();

		for (ProvinceDefinition::adjacency_t const& adjacency : current->province_definition.get_adjacencies()) {
			ProvinceDefinition const& to_definition = adjacency.get_to();
			if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::IMPASSABLE) {
				continue;
			}
			ProvinceInstance& to = map_instance.get_province_instance_by_definition(to_definition);
			if (to_definition.is_water()) {
				if (!seen[&to]) {
					seen[&to] = true;
					queue.push(&to);
				}
			} else if (to_definition.has_port() && to_definition.get_port_adjacent_province() == &current->province_definition
				&& UnitInstanceGroup::can_navy_enter(country, to)) {
				return &to;
			}
		}
	}
	return nullptr;
}

void UnitInstanceManager::destroy_unit(UnitInstance& unit, UnitInstanceGroup& group) {
	if (unit.get_branch() == LAND) {
		if (Pop* const pop = static_cast<RegimentInstance&>(unit).get_pop()) {
			pop->release_recruit();
		}
	}
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
	// The armies on board of a destroyed navy are lost with it
	if (group.branch == NAVAL) {
		NavyInstance& navy = static_cast<NavyInstance&>(group);
		memory::vector<ArmyInstance*> carried;
		for (ArmyInstance& army : navy.get_carried_armies()) {
			carried.push_back(&army);
		}
		for (ArmyInstance* army : carried) {
			navy.forget_carried_army(*army);
			army->set_transport_navy(nullptr);
			destroy_group(*army);
		}
	} else {
		ArmyInstance& army = static_cast<ArmyInstance&>(group);
		if (army.get_transport_navy() != nullptr) {
			const_cast<NavyInstance*>(army.get_transport_navy())->forget_carried_army(army);
			army.set_transport_navy(nullptr);
		}
	}

	// Delete the units first, copy the list since destroying a unit edits the group
	memory::vector<UnitInstance*> group_units;
	for (UnitInstance& unit : group.get_units()) {
		group_units.push_back(&unit);
	}

	for (UnitInstance* unit : group_units) {
		if (unit->get_branch() == LAND) {
			if (Pop* const pop = static_cast<RegimentInstance&>(*unit).get_pop()) {
				pop->release_recruit();
			}
		}
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
int32_t UnitInstanceManager::get_recruitment_days(CountryInstance const& country, UnitType const& unit_type) {
	// The build time of the unit scaled by the recruitment time modifier of the country, at least a day
	const fixed_point_t days = fixed_point_t::parse_unsafe(static_cast<int32_t>(unit_type.build_time.to_int()))
		* country.get_recruit_time();
	return std::max<int32_t>(1, days.ceil<int32_t>());
}

std::string_view UnitInstanceManager::get_recruitment_block_reason(
	CountryInstance const& country, RegimentType const& regiment_type, ProvinceInstance const& province, Pop const& pop
) {
	if (!country.is_unit_type_unlocked(regiment_type)) {
		return "Тип полка ещё не открыт";
	}
	if (province.get_owner() != &country || province.get_controller() != &country) {
		return "Набор возможен только в своей, не оккупированной провинции";
	}
	if (&pop.get_location() != &province) {
		return "Население не из этой провинции";
	}
	if (!pop.get_type().can_be_recruited) {
		return "Этот тип населения не призывается";
	}
	if (!Pop::is_culture_status_allowed(country.get_allowed_regiment_cultures(), pop.get_culture_status())) {
		return "Культура населения не допускается к службе";
	}
	if (pop.get_regiment_count() >= pop.get_max_supported_regiments()) {
		return "Население не может выставить больше полков";
	}
	for (auto const& [good, quantity] : regiment_type.get_build_cost()) {
		if (country.get_good_data(*good).stockpile_amount < quantity) {
			return "На складе страны не хватает товаров";
		}
	}
	return {};
}

bool UnitInstanceManager::start_recruitment(
	CountryInstance& country, RegimentType const& regiment_type, ProvinceInstance& province, Pop& pop
) {
	if (!get_recruitment_block_reason(country, regiment_type, province, pop).empty()) {
		return false;
	}

	if (!pop.try_recruit()) {
		return false;
	}

	for (auto const& [good, quantity] : regiment_type.get_build_cost()) {
		CountryInstance::good_data_t& good_data = country.get_good_data(*good);
		good_data.stockpile_amount -= quantity;
		good_data.stockpile_change_yesterday -= quantity;
	}

	const int32_t days = get_recruitment_days(country, regiment_type);
	recruitment_orders.push_back({ unique_id_counter++, &country, &regiment_type, nullptr, &province, &pop, days, days });
	return true;
}

bool UnitInstanceManager::cancel_recruitment(CountryInstance& country, const unique_id_t order_id) {
	for (auto it = recruitment_orders.begin(); it != recruitment_orders.end(); ++it) {
		if (it->id != order_id) {
			continue;
		}
		if (it->country != &country) {
			return false;
		}

		UnitType const& unit_type = it->ship_type != nullptr ? static_cast<UnitType const&>(*it->ship_type)
			: static_cast<UnitType const&>(*it->regiment_type);
		for (auto const& [good, quantity] : unit_type.get_build_cost()) {
			CountryInstance::good_data_t& good_data = country.get_good_data(*good);
			good_data.stockpile_amount += quantity;
			good_data.stockpile_change_yesterday += quantity;
		}
		if (it->pop != nullptr) {
			it->pop->release_recruit();
		}
		recruitment_orders.erase(it);
		return true;
	}
	return false;
}

ArmyInstance* UnitInstanceManager::split_regiment(ArmyInstance& army, RegimentInstance& regiment) {
	if (army.get_unit_count() < 2) {
		return nullptr;
	}

	const unique_id_t new_army_id = unique_id_counter++;
	ArmyInstance& new_army = *armies.emplace(
		new_army_id, fmt::format("{} ({})", army.get_name(), new_army_id), army.get_country(), army.get_location()
	);
	unit_instance_group_map.emplace(new_army.unique_id, new_army);

	if (!army.remove_unit(regiment)) {
		destroy_group(new_army);
		return nullptr;
	}
	new_army.add_unit(regiment);
	army.update_gamestate();
	new_army.update_gamestate();
	return &new_army;
}

bool UnitInstanceManager::merge_armies(ArmyInstance& target, ArmyInstance& source) {
	if (&target == &source || &target.get_country() != &source.get_country() ||
		&target.get_location() != &source.get_location() || source.is_moving() || target.is_moving()) {
		return false;
	}

	memory::vector<UnitInstance*> moved;
	for (UnitInstance& unit : source.get_units()) {
		moved.push_back(&unit);
	}
	for (UnitInstance* unit : moved) {
		source.remove_unit(*unit);
		target.add_unit(*unit);
	}

	if (target.get_leader() == nullptr && source.get_leader() != nullptr) {
		LeaderInstance* const leader = source.get_leader();
		source.set_leader(nullptr);
		target.set_leader(leader);
	}

	destroy_group(source);
	target.update_gamestate();
	return true;
}

void UnitInstanceManager::disband_regiment(ArmyInstance& army, RegimentInstance& regiment) {
	destroy_unit(regiment, army);
}

ArmyInstance* UnitInstanceManager::get_army_of_regiment(RegimentInstance const& regiment) {
	for (ArmyInstance& army : armies) {
		for (UnitInstance const& unit : army.get_units()) {
			if (&unit == &regiment) {
				return &army;
			}
		}
	}
	return nullptr;
}

void UnitInstanceManager::destroy_leader(LeaderInstance& leader) {
	if (UnitInstanceGroup* const group = leader.get_unit_instance_group()) {
		group->set_leader(nullptr);
	}
	const_cast<CountryInstance&>(leader.country).remove_leader(leader);
	leader_instance_map.erase(leader.unique_id);
	leaders.erase(leaders.get_iterator(&leader));
}

void UnitInstanceManager::enforce_transport_capacity(NavyInstance& navy) {
	// Armies that do not fit in the remaining transports sink, the last loaded first
	while (navy.get_carried_regiment_count() > navy.get_transport_capacity() && !navy.get_carried_armies().empty()) {
		ArmyInstance& army = navy.get_carried_armies().back().get();
		navy.forget_carried_army(army);
		army.set_transport_navy(nullptr);
		destroy_group(army);
	}
}

std::string_view UnitInstanceManager::get_ship_block_reason(
	CountryInstance const& country, ShipType const& ship_type, ProvinceInstance const& province
) const {
	if (!country.is_unit_type_unlocked(ship_type)) {
		return "Тип корабля ещё не открыт";
	}
	if (province.get_owner() != &country || province.get_controller() != &country) {
		return "Корабли строятся только в своей, не оккупированной провинции";
	}
	if (!province.province_definition.has_port()) {
		return "В провинции нет порта";
	}
	if (!ship_type.can_build_overseas) {
		ProvinceInstance const* const capital = country.get_capital();
		if (capital != nullptr && capital->province_definition.get_continent() != province.province_definition.get_continent()) {
			return "Этот корабль нельзя строить за морем";
		}
	}

	// The level of the naval base limits the ships
	uint32_t port_level = 0;
	for (BuildingInstance const& building : province.get_buildings()) {
		if (building.building_type.get_identifier() == "naval_base") {
			port_level = static_cast<uint32_t>(type_safe::get(building.get_level()));
		}
	}
	if (port_level < ship_type.min_port_level) {
		return "Нужна военно-морская база более высокого уровня";
	}
	if (ship_type.limit_per_port >= 0) {
		int32_t existing = 0;
		for (NavyInstance const& navy : province.get_navies()) {
			for (ShipInstance const& ship : navy.get_ship_instances()) {
				if (&ship.get_ship_type() == &ship_type) {
					++existing;
				}
			}
		}
		for (RecruitmentOrder const& order : recruitment_orders) {
			if (order.ship_type == &ship_type && order.province == &province) {
				++existing;
			}
		}
		if (existing >= ship_type.limit_per_port) {
			return "В порту уже максимум кораблей этого типа";
		}
	}

	for (auto const& [good, quantity] : ship_type.get_build_cost()) {
		if (country.get_good_data(*good).stockpile_amount < quantity) {
			return "На складе страны не хватает товаров";
		}
	}
	return {};
}

bool UnitInstanceManager::start_ship_building(CountryInstance& country, ShipType const& ship_type, ProvinceInstance& province) {
	if (!get_ship_block_reason(country, ship_type, province).empty()) {
		return false;
	}

	for (auto const& [good, quantity] : ship_type.get_build_cost()) {
		CountryInstance::good_data_t& good_data = country.get_good_data(*good);
		good_data.stockpile_amount -= quantity;
		good_data.stockpile_change_yesterday -= quantity;
	}

	const int32_t days = get_recruitment_days(country, ship_type);
	recruitment_orders.push_back({ unique_id_counter++, &country, nullptr, &ship_type, &province, nullptr, days, days });
	return true;
}
