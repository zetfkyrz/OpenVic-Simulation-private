#include "UnitInstanceGroup.hpp"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include <fmt/std.h>

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/defines/CountryDefines.hpp"
#include "openvic-simulation/defines/MilitaryDefines.hpp"
#include "openvic-simulation/military/UnitModifiers.hpp"
#include "openvic-simulation/military/Unlocks.hpp"
#include "openvic-simulation/modifier/ModifierEffectCache.hpp"
#include "openvic-simulation/economy/BuildingInstance.hpp"
#include "openvic-simulation/economy/BuildingType.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/military/Deployment.hpp"
#include "openvic-simulation/military/Division.hpp"
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
	return adjacency != nullptr ? adjacency->get_distance() : fixed_point_t(war_defines->port_step_distance);
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

	movement_progress += speed * war_defines->movement_speed_scale;

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
		previous_location = &location.get();
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
	// A field marshal does not lead an army (nothing changes)
	if (new_leader != nullptr && new_leader->is_marshal()) {
		return false;
	}
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

	const fixed_point_t per_day = get_movement_speed() * war_defines->movement_speed_scale;
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
		// The room of the type in the data (transport_capacity.txt), else REGIMENTS_PER_TRANSPORT of a transport type
		const std::int32_t room = ship.get_ship_type().transport_capacity;
		if (room >= 0) {
			capacity += static_cast<size_t>(room);
		} else if (ship.get_ship_type().is_transport) {
			capacity += static_cast<size_t>(war_defines->regiments_per_transport);
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
	if (war_defines->transport_unlimited == 0 && get_carried_regiment_count() + army.get_unit_count() > get_transport_capacity()) {
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

bool UnitInstanceGroupBranched<NAVAL>::land_army(ArmyInstance& army, ProvinceInstance& target, const fixed_point_t penalty_reduction) {
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
	// The men are tired after the landing (less after the preparation of a plan)
	const fixed_point_t kept_of_penalty = fixed_point_t::_1 - std::clamp(penalty_reduction, fixed_point_t::_0, fixed_point_t::_1);
	for (RegimentInstance& regiment : army.get_regiment_instances()) {
		regiment.take_damage(
			regiment.get_organisation() * (war_defines->landing_organisation_divisor - 1) / war_defines->landing_organisation_divisor * kept_of_penalty, fixed_point_t::_0
		);
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

void UnitInstanceManager::tick(const Date today) {
	for (ArmyInstance& army : armies) {
		army.tick();
		// The stance (docs/WAR_ASSUMPTIONS.md, section 10): an entrenched army digs in faster and deeper, a resting one does not dig in
		std::int32_t days_per_level = static_cast<int32_t>(military_defines.get_dig_in_increase_each_days().to_int());
		std::int32_t cap = army.get_country().get_dig_in_cap();
		if (army.get_stance() == ArmyInstance::STANCE_ENTRENCH && UnitInstanceGroup::war_defines != nullptr) {
			days_per_level = std::max(1, days_per_level / std::max(1, UnitInstanceGroup::war_defines->stance_entrench_speed));
			cap += UnitInstanceGroup::war_defines->stance_entrench_extra_levels;
		}
		if (army.get_stance() == ArmyInstance::STANCE_REST) {
			army.reset_dig_in();
		} else {
			army.advance_dig_in(days_per_level, cap);
		}
	}

	// (the orders that waited for the monthly limit started at the beginning of the day, InstanceManager::tick)
	(void) today;

	// Recruitment: the regiment appears in the province when the time is up
	for (size_t i = 0; i < recruitment_orders.size();) {
		if (recruitment_orders[i].waiting || --recruitment_orders[i].days_left > 0) {
			++i;
			continue;
		}
		const RecruitmentOrder order = recruitment_orders[i];

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
			regiment_id, fmt::format("{} {}", regiment_type->get_identifier(), regiment_id), *regiment_type, pop, order.mobilisation
		});
		unit_instance_map.emplace(regiment.unique_id, regiment);

		// A regiment of a division recruited by a template: into the army of its division in the province (a new army for the first one)
		ArmyInstance* target = nullptr;
		if (order.mobilisation) {
			// A mobilised regiment joins the mobilised army of the province that stands there (every regiment mobilised) or makes a new one: the
			// divisions (update_all_divisions) are made of the mobilised regiments only, the demobilisation does not tear the regular ones apart
			for (ArmyInstance& army : province.get_armies()) {
				if (&army.get_country() != &country || army.is_moving() || army.is_in_combat() || army.get_transport_navy() != nullptr
					|| army.empty()) {
					continue;
				}
				bool all_mobilised = true;
				for (RegimentInstance const& other : army.get_regiment_instances()) {
					all_mobilised &= other.is_mobilised();
				}
				if (all_mobilised) {
					target = &army;
					break;
				}
			}
			if (target == nullptr) {
				const unique_id_t army_id = unique_id_counter++;
				target = &*armies.emplace(army_id, fmt::format("Мобилизованные {}", army_id), country, province);
				unit_instance_group_map.emplace(target->unique_id, *target);
			}
		} else if (order.division_id != 0) {
			for (ArmyInstance& army : province.get_armies()) {
				if (&army.get_country() == &country && std::any_of(army.get_divisions().begin(), army.get_divisions().end(),
					[&order](DivisionInstance const& division) -> bool { return division.id == order.division_id; })) {
					target = &army;
					break;
				}
			}
			if (target == nullptr) {
				const unique_id_t army_id = unique_id_counter++;
				target = &*armies.emplace(army_id, fmt::format("Army {}", army_id), country, province);
				unit_instance_group_map.emplace(target->unique_id, *target);
				target->get_mutable_divisions().push_back(DivisionInstance { order.division_id, order.template_id });
			}
			regiment.set_division_id(order.division_id);
		}
		// Else it joins a standing army of the country in the province or forms a new one
		for (ArmyInstance& army : province.get_armies()) {
			if (target != nullptr) {
				break;
			}
			if (&army.get_country() == &country && !army.is_moving() && !army.is_in_combat()
				&& army.get_transport_navy() == nullptr) {
				target = &army;
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
	// Armies carried by a navy move with it; an army at sea (its way by sea, P4d) goes on to its destination
	if (!naval && (static_cast<ArmyInstance&>(group).get_transport_navy() != nullptr || static_cast<ArmyInstance&>(group).is_at_sea())) {
		return false;
	}
	// A new order (not added to the route) ends a way by sea that has not started
	if (!naval && !append) {
		static_cast<ArmyInstance&>(group).clear_sea_transfer();
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
				// Over the land, and over a strait (the Bosporus, the Dardanelles...) unless a navy of an enemy at war holds its sea (as in Vic2)
				if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::STRAIT) {
					if (adjacency.get_through() != nullptr) {
						ProvinceInstance const& sea = map_instance.get_province_instance_by_definition(*adjacency.get_through());
						if (std::any_of(sea.get_navies().begin(), sea.get_navies().end(), [&country](NavyInstance const& navy) -> bool {
							return !navy.empty() && navy.get_country().is_at_war_with(country);
						})) {
							continue;
						}
					}
				} else if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND) {
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
		// No way over the land: by sea, through the ports of the country (P4d)
		if (!naval && !extend_route && !planning_sea_transfer && UnitInstanceGroup::war_defines != nullptr && UnitInstanceGroup::war_defines->sea_transfer_enabled != 0) {
			// The way over the land to the boarding port must not try the sea again
			planning_sea_transfer = true;
			const bool by_sea = order_sea_transfer(static_cast<ArmyInstance&>(group), destination, map_instance);
			planning_sea_transfer = false;
			return by_sea;
		}
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

namespace {
	// The distance over the sea from a sea to every sea, not through a sea with a navy of an enemy at war of the country (the step into the first sea
	// from the port counts PORT_STEP_DISTANCE)
	std::unordered_map<ProvinceInstance*, fixed_point_t> sea_costs_from(ProvinceInstance& first_sea, CountryInstance const& country, MapInstance& map_instance) {
		const auto enemy_sea = [&country](ProvinceInstance const& sea) -> bool {
			return std::any_of(sea.get_navies().begin(), sea.get_navies().end(), [&country](NavyInstance const& navy) -> bool {
				return !navy.empty() && navy.get_country().is_at_war_with(country);
			});
		};
		using entry_t = std::pair<fixed_point_t, ProvinceInstance*>;
		struct entry_greater {
			bool operator()(entry_t const& lhs, entry_t const& rhs) const {
				return lhs.first != rhs.first ? lhs.first > rhs.first : type_safe::get(lhs.second->index) > type_safe::get(rhs.second->index);
			}
		};
		std::unordered_map<ProvinceInstance*, fixed_point_t> sea_cost;
		if (enemy_sea(first_sea)) {
			return sea_cost;
		}
		std::priority_queue<entry_t, std::vector<entry_t>, entry_greater> queue;
		sea_cost[&first_sea] = fixed_point_t(UnitInstanceGroup::war_defines->port_step_distance);
		queue.emplace(sea_cost[&first_sea], &first_sea);
		while (!queue.empty()) {
			const auto [current_cost, current] = queue.top();
			queue.pop();
			if (current_cost > sea_cost[current]) {
				continue;
			}
			for (ProvinceDefinition::adjacency_t const& adjacency : current->province_definition.get_adjacencies()) {
				if (!adjacency.get_to().is_water() || adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::IMPASSABLE) {
					continue;
				}
				ProvinceInstance& to = map_instance.get_province_instance_by_definition(adjacency.get_to());
				if (enemy_sea(to)) {
					continue;
				}
				const fixed_point_t next_cost = current_cost + adjacency.get_distance();
				const auto it = sea_cost.find(&to);
				if (it == sea_cost.end() || next_cost < it->second) {
					sea_cost[&to] = next_cost;
					queue.emplace(next_cost, &to);
				}
			}
		}
		return sea_cost;
	}

	// The cost over the land (the distance, the straits as in order_move) from a province to every province an army of the country can reach
	std::unordered_map<ProvinceInstance*, fixed_point_t> land_costs_from(
		ProvinceInstance& start, CountryInstance const& country, MapInstance& map_instance
	) {
		using entry_t = std::pair<fixed_point_t, ProvinceInstance*>;
		struct entry_greater {
			bool operator()(entry_t const& lhs, entry_t const& rhs) const {
				return lhs.first != rhs.first ? lhs.first > rhs.first : type_safe::get(lhs.second->index) > type_safe::get(rhs.second->index);
			}
		};
		std::priority_queue<entry_t, std::vector<entry_t>, entry_greater> queue;
		std::unordered_map<ProvinceInstance*, fixed_point_t> cost;
		cost[&start] = fixed_point_t::_0;
		queue.emplace(fixed_point_t::_0, &start);
		while (!queue.empty()) {
			const auto [current_cost, current] = queue.top();
			queue.pop();
			if (current_cost > cost[current]) {
				continue;
			}
			for (ProvinceDefinition::adjacency_t const& adjacency : current->province_definition.get_adjacencies()) {
				const auto type = adjacency.get_type();
				if ((type != ProvinceDefinition::adjacency_t::type_t::LAND && type != ProvinceDefinition::adjacency_t::type_t::STRAIT)
					|| adjacency.get_to().is_water()) {
					continue;
				}
				ProvinceInstance& to = map_instance.get_province_instance_by_definition(adjacency.get_to());
				if (!UnitInstanceGroup::can_country_enter(country, to)) {
					continue;
				}
				// A strait with a navy of an enemy at war is closed (as in order_move)
				if (type == ProvinceDefinition::adjacency_t::type_t::STRAIT && adjacency.get_through() != nullptr) {
					ProvinceInstance const& strait = map_instance.get_province_instance_by_definition(*adjacency.get_through());
					if (std::any_of(strait.get_navies().begin(), strait.get_navies().end(), [&country](NavyInstance const& navy) -> bool {
						return !navy.empty() && navy.get_country().is_at_war_with(country);
					})) {
						continue;
					}
				}
				const fixed_point_t next_cost = current_cost + adjacency.get_distance();
				const auto it = cost.find(&to);
				if (it == cost.end() || next_cost < it->second) {
					cost[&to] = next_cost;
					queue.emplace(next_cost, &to);
				}
			}
		}
		return cost;
	}
}

bool UnitInstanceManager::order_sea_transfer(ArmyInstance& army, ProvinceInstance& destination, MapInstance& map_instance) {
	CountryInstance const& country = army.get_country();
	// The ports of the country (owned and held) the army reaches over the land, and the ones the destination is reached from
	const auto from_army = land_costs_from(army.get_location(), country, map_instance);
	const auto from_destination = land_costs_from(destination, country, map_instance);
	// The ports the country holds (its own and the ones it took: version 81)
	const auto is_own_port = [&country](ProvinceInstance const* province) -> bool {
		return province->province_definition.has_port() && province->province_definition.get_port_adjacent_province() != nullptr
			&& province->get_controller() == &country;
	};
	ProvinceInstance* boarding = nullptr;
	fixed_point_t boarding_cost = fixed_point_t::_0;
	for (ProvinceInstance* province : country.get_controlled_provinces()) {
		const auto it = from_army.find(province);
		if (it != from_army.end() && is_own_port(province) && (boarding == nullptr || it->second < boarding_cost)) {
			boarding = province;
			boarding_cost = it->second;
		}
	}
	if (boarding == nullptr) {
		return false;
	}
	// The sea from the boarding port: the distance to every water province, not through a sea with a navy of an enemy at war
	ProvinceInstance& first_sea = map_instance.get_province_instance_by_definition(*boarding->province_definition.get_port_adjacent_province());
	const std::unordered_map<ProvinceInstance*, fixed_point_t> sea_cost = sea_costs_from(first_sea, country, map_instance);
	// The landing port: of the country, reached by sea, the destination reached from it over the land; the shortest way (sea and land) wins
	ProvinceInstance* landing = nullptr;
	fixed_point_t landing_sea = fixed_point_t::_0, landing_total = fixed_point_t::_0;
	for (ProvinceInstance* province : country.get_controlled_provinces()) {
		if (province == boarding || !is_own_port(province)) {
			continue;
		}
		const auto land = from_destination.find(province);
		if (land == from_destination.end()) {
			continue;
		}
		ProvinceInstance* const sea = &map_instance.get_province_instance_by_definition(*province->province_definition.get_port_adjacent_province());
		const auto by_sea = sea_cost.find(sea);
		if (by_sea == sea_cost.end()) {
			continue;
		}
		const fixed_point_t sea_distance = by_sea->second + fixed_point_t(UnitInstanceGroup::war_defines->port_step_distance);
		const fixed_point_t total = sea_distance + land->second;
		if (landing == nullptr || total < landing_total) {
			landing = province;
			landing_sea = sea_distance;
			landing_total = total;
		}
	}
	if (landing == nullptr) {
		return false;
	}
	// The ships (SEA_TRANSFER_NEEDS_SHIPS): a navy of the country with room for the whole army comes to the boarding port
	unique_id_t navy_id = 0;
	if (UnitInstanceGroup::war_defines->sea_transfer_needs_ships != 0) {
		NavyInstance* const navy = find_transfer_navy(army, *boarding, map_instance);
		if (navy == nullptr) {
			return false;
		}
		navy_id = navy->unique_id;
	}
	// Over the land to the boarding port first (it may be where the army is)
	if (&army.get_location() != boarding && !order_move(army, *boarding, map_instance)) {
		return false;
	}
	const fixed_point_t per_day = std::max(fixed_point_t::_0_01, UnitInstanceGroup::war_defines->sea_transfer_speed * UnitInstanceGroup::war_defines->movement_speed_scale);
	const std::uint32_t days = static_cast<std::uint32_t>(std::max(1, (landing_sea / per_day).ceil<std::int32_t>()));
	army.set_sea_transfer(*boarding, *landing, destination, days, navy_id);
	return true;
}

bool UnitInstanceManager::order_sea_landing(ArmyInstance& army, ProvinceInstance& coast, MapInstance& map_instance) {
	if (UnitInstanceGroup::war_defines == nullptr || UnitInstanceGroup::war_defines->sea_transfer_needs_ships == 0 || army.empty()
		|| army.get_transport_navy() != nullptr || army.is_in_combat() || coast.province_definition.is_water()) {
		return false;
	}
	CountryInstance const& country = army.get_country();
	// The nearest port the country holds that the army reaches over the land
	const auto from_army = land_costs_from(army.get_location(), country, map_instance);
	ProvinceInstance* boarding = nullptr;
	fixed_point_t boarding_cost = fixed_point_t::_0;
	for (ProvinceInstance* province : country.get_controlled_provinces()) {
		const auto it = from_army.find(province);
		if (it != from_army.end() && province->province_definition.has_port() && province->province_definition.get_port_adjacent_province() != nullptr
			&& (boarding == nullptr || it->second < boarding_cost)) {
			boarding = province;
			boarding_cost = it->second;
		}
	}
	if (boarding == nullptr) {
		return false;
	}
	// The sea next to the coast the ships reach first (the seas the coast touches, and the sea of its port)
	ProvinceInstance& first_sea = map_instance.get_province_instance_by_definition(*boarding->province_definition.get_port_adjacent_province());
	const std::unordered_map<ProvinceInstance*, fixed_point_t> sea_cost = sea_costs_from(first_sea, country, map_instance);
	ProvinceInstance* landing_sea = nullptr;
	fixed_point_t landing_cost = fixed_point_t::_0;
	for (ProvinceDefinition::adjacency_t const& adjacency : coast.province_definition.get_adjacencies()) {
		if (!adjacency.get_to().is_water()) {
			continue;
		}
		ProvinceInstance& sea = map_instance.get_province_instance_by_definition(adjacency.get_to());
		const auto it = sea_cost.find(&sea);
		if (it != sea_cost.end() && (landing_sea == nullptr || it->second < landing_cost)) {
			landing_sea = &sea;
			landing_cost = it->second;
		}
	}
	if (landing_sea == nullptr) {
		return false;
	}
	NavyInstance* const navy = find_transfer_navy(army, *boarding, map_instance);
	if (navy == nullptr) {
		return false;
	}
	if (&army.get_location() != boarding && !order_move(army, *boarding, map_instance)) {
		return false;
	}
	const fixed_point_t per_day = std::max(fixed_point_t::_0_01, UnitInstanceGroup::war_defines->sea_transfer_speed * UnitInstanceGroup::war_defines->movement_speed_scale);
	army.set_sea_transfer(*boarding, coast, coast, static_cast<std::uint32_t>(std::max(1, (landing_cost / per_day).ceil<std::int32_t>())), navy->unique_id);
	army.set_sea_transfer_landing_sea(landing_sea);
	return true;
}

bool UnitInstanceManager::is_navy_booked(NavyInstance const& navy) const {
	for (ArmyInstance const& army : navy.get_country().get_armies()) {
		if (army.get_sea_transfer_navy_id() == navy.unique_id) {
			return true;
		}
	}
	return false;
}

NavyInstance* UnitInstanceManager::find_transfer_navy(ArmyInstance const& army, ProvinceInstance& boarding, MapInstance& map_instance) {
	// A navy of the country that carries nothing, is booked by no other army, does not fight and has room for the whole army (a division is not
	// split): the one that waits in the boarding port, else the first one that stands still and can sail there
	const size_t need = army.get_unit_count();
	const bool unlimited = UnitInstanceGroup::war_defines->transport_unlimited != 0;
	std::vector<NavyInstance*> elsewhere;
	for (NavyInstance& navy : navies) {
		if (&navy.get_country() != &army.get_country() || navy.empty() || navy.is_in_combat() || navy.is_moving()
			|| !navy.get_carried_armies().empty() || is_navy_booked(navy) || (!unlimited && navy.get_transport_capacity() < need)) {
			continue;
		}
		if (&navy.get_location() == &boarding) {
			return &navy;
		}
		elsewhere.push_back(&navy);
	}
	// The nearest first (the distance of the centres; ties: the order of the navies)
	const auto distance_to_boarding = [&boarding](NavyInstance const* navy) -> double {
		const double dx = static_cast<double>(navy->get_location().province_definition.get_centre().x)
			- static_cast<double>(boarding.province_definition.get_centre().x);
		const double dy = static_cast<double>(navy->get_location().province_definition.get_centre().y)
			- static_cast<double>(boarding.province_definition.get_centre().y);
		return dx * dx + dy * dy;
	};
	std::stable_sort(elsewhere.begin(), elsewhere.end(), [&distance_to_boarding](NavyInstance const* first, NavyInstance const* second) -> bool {
		return distance_to_boarding(first) < distance_to_boarding(second);
	});
	for (NavyInstance* navy : elsewhere) {
		if (order_move(*navy, boarding, map_instance)) {
			return navy;
		}
	}
	return nullptr;
}

void UnitInstanceManager::update_ship_transfer(ArmyInstance& army, MapInstance& map_instance) {
	UnitInstanceGroup* const group = get_unit_instance_group_by_unique_id(army.get_sea_transfer_navy_id());
	NavyInstance* const navy = group != nullptr && group->branch == NAVAL ? static_cast<NavyInstance*>(group) : nullptr;
	ProvinceInstance& boarding = *army.get_sea_transfer_from();
	ProvinceInstance& landing = *army.get_sea_transfer_to();
	ProvinceInstance& destination = *army.get_sea_transfer_destination();
	if (navy == nullptr || navy->empty()) {
		// The ships are gone (an army on board went down with them)
		if (army.get_transport_navy() == nullptr) {
			army.clear_sea_transfer();
		}
		return;
	}
	ProvinceInstance* const landing_sea = army.get_sea_transfer_landing_sea();
	ProvinceInstance& sail_to = landing_sea != nullptr ? *landing_sea : landing;
	if (army.get_transport_navy() == navy) {
		// On board: in the landing port it goes ashore and on over the land; a navy that stopped elsewhere (a battle) sails on
		if (navy->is_moving() || navy->is_in_combat()) {
			return;
		}
		if (landing_sea != nullptr && &navy->get_location() == landing_sea) {
			// The landing on the coast; a coast it cannot land on (the land of a country it may not enter now): back to the boarding port
			if (navy->land_army(army, landing, fixed_point_t::_0)) {
				army.clear_sea_transfer();
			} else {
				army.set_sea_transfer(boarding, boarding, boarding, 1, navy->unique_id);
				army.set_sea_transfer_landing_sea(nullptr);
				order_move(*navy, boarding, map_instance);
			}
			return;
		}
		if (landing_sea == nullptr && &navy->get_location() == &landing) {
			navy->unload_army(army);
			army.clear_sea_transfer();
			if (&landing != &destination) {
				order_move(army, destination, map_instance);
			}
		} else if (!order_move(*navy, sail_to, map_instance) && !navy->get_location().province_definition.is_water()) {
			// No way on to the landing port: ashore in the port the navy is in
			navy->unload_army(army);
			army.clear_sea_transfer();
		}
		return;
	}
	if (army.get_transport_navy() != nullptr) {
		army.clear_sea_transfer();
		return;
	}
	// The navy took other armies on board (an order of the player, a field marshal): it is not ours any more, it is not led away
	if (!navy->get_carried_armies().empty()) {
		army.clear_sea_transfer();
		return;
	}
	if (army.is_moving() || army.is_in_combat() || navy->is_moving() || navy->is_in_combat()) {
		return;
	}
	if (&army.get_location() != &boarding) {
		army.clear_sea_transfer(); // stopped elsewhere: the way by sea is given up
		return;
	}
	if (&navy->get_location() == &boarding) {
		// Both in the port: on board and away
		if (!navy->load_army(army) || !order_move(*navy, sail_to, map_instance)) {
			if (army.get_transport_navy() == navy) {
				navy->unload_army(army);
			}
			army.clear_sea_transfer();
		}
	} else if (!order_move(*navy, boarding, map_instance)) {
		army.clear_sea_transfer(); // the ships cannot come
	}
}

void UnitInstanceManager::update_sea_transfers(MapInstance& map_instance) {
	for (ArmyInstance& army : armies) {
		if (!army.has_sea_transfer()) {
			continue;
		}
		// On ships (SEA_TRANSFER_NEEDS_SHIPS)
		if (army.get_sea_transfer_navy_id() != 0) {
			update_ship_transfer(army, map_instance);
			continue;
		}
		if (army.is_at_sea()) {
			army.set_sea_transfer_days_left(army.get_sea_transfer_days_left() - 1);
			if (army.get_sea_transfer_days_left() == 0) {
				// Landed in the port: on over the land
				ProvinceInstance& landing = *army.get_sea_transfer_to();
				ProvinceInstance& destination = *army.get_sea_transfer_destination();
				army.clear_sea_transfer();
				army.set_location(landing);
				if (&landing != &destination) {
					order_move(army, destination, map_instance);
				}
			}
			continue;
		}
		if (army.is_moving()) {
			continue;
		}
		if (&army.get_location() == army.get_sea_transfer_from() && !army.is_in_combat()) {
			// In the boarding port: it sails
			army.set_sea_transfer_days_left(army.get_sea_transfer_days_total());
		} else if (!army.is_in_combat()) {
			army.clear_sea_transfer(); // stopped elsewhere: the way by sea is given up
		}
	}
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

ProvinceInstance* UnitInstanceManager::find_nearest_army_port(ArmyInstance const& army, MapInstance& map_instance) const {
	CountryInstance const& country = army.get_country();
	BuildingType const* const naval_base = unit_type_manager_naval_base;
	std::vector<ProvinceInstance*> queue { const_cast<ProvinceInstance*>(&army.get_location()) };
	std::unordered_set<ProvinceInstance const*> seen { &army.get_location() };
	for (std::size_t head = 0; head < queue.size(); ++head) {
		ProvinceInstance* const current = queue[head];
		if (current->get_owner() == &country && current->get_controller() == &country && current->province_definition.has_port()
			&& (naval_base == nullptr || !naval_base->province_building_index.has_value()
				|| type_safe::get(current->get_buildings()[naval_base->province_building_index.value()].get_level()) > 0)) {
			return current;
		}
		for (ProvinceDefinition::adjacency_t const& adjacency : current->province_definition.get_adjacencies()) {
			if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND || adjacency.get_to().is_water()) {
				continue;
			}
			ProvinceInstance& next = map_instance.get_province_instance_by_definition(adjacency.get_to());
			if (UnitInstanceGroup::can_country_enter(country, next) && seen.insert(&next).second) {
				queue.push_back(&next);
			}
		}
	}
	return nullptr;
}

void UnitInstanceManager::update_follows(MapInstance& map_instance) {
	std::vector<ArmyInstance*> followers;
	for (ArmyInstance& army : armies) {
		if (army.get_follow_army_id() != 0) {
			followers.push_back(&army);
		}
	}
	for (ArmyInstance* army : followers) {
		UnitInstanceGroup* const target = get_unit_instance_group_by_unique_id(army->get_follow_army_id());
		CountryInstance const& country = army->get_country();
		if (target == nullptr || target->branch != LAND || target == army
			|| (&target->get_country() != &country && !country.has_alliance_with(target->get_country()))) {
			army->set_follow_army_id(0);
			continue;
		}
		ProvinceInstance& where = target->get_location();
		const bool heading_there = army->is_moving() ? &army->get_path().back().get() == &where : &army->get_location() == &where;
		if (!heading_there && !army->is_in_combat() && !static_cast<ArmyInstance*>(target)->is_at_sea()) {
			order_move(*army, where, map_instance);
		}
	}
}

void UnitInstanceManager::destroy_unit(UnitInstance& unit, UnitInstanceGroup& group) {
	if (unit.get_branch() == LAND) {
		apply_losses(static_cast<RegimentInstance&>(unit), group.get_country());
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
			kill_group(*army);
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
			apply_losses(static_cast<RegimentInstance&>(*unit), group.get_country());
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
namespace {
	// A pop whose men can serve in the regiments of its country (the manpower of the country decides how many)
	bool pop_gives_regiment(CountryInstance const& country, Pop const& pop) {
		return pop.get_type().can_be_recruited && Pop::is_culture_status_allowed(country.get_allowed_regiment_cultures(), pop.get_culture_status())
			&& pop.get_size() > pop_size_t(0);
	}

	std::int64_t strength_men(const fixed_point_t strength) {
		return (strength * UnitInstanceGroup::war_defines->template_men_per_strength).floor<std::int64_t>();
	}
}

std::int64_t UnitInstanceManager::get_regiment_men(RegimentType const& regiment_type) const {
	return strength_men(regiment_type.max_strength);
}

Manpower UnitInstanceManager::get_manpower(CountryInstance const& country) const {
	Manpower manpower;
	manpower.men_per_regiment = static_cast<std::int32_t>(type_safe::get(military_defines.get_pop_size_per_regiment()));
	for (ProvinceInstance const* province : country.get_owned_provinces()) {
		if (province->get_controller() != &country) {
			continue;
		}
		for (Pop const& pop : province->get_pops()) {
			if (pop_gives_regiment(country, pop)) {
				manpower.pop_men += type_safe::get(pop.get_size());
			}
		}
	}
	Rate const& base_bonus = UnitInstanceGroup::war_defines->manpower_base_bonus;
	manpower.multiplier += base_bonus.fixed();
	manpower.max_men = manpower.pop_men + base_bonus.of_int(manpower.pop_men);
	// The model of the army (the laws): the national militia +50 %
	if (const fixed_point_t law_manpower = country.get_law_effect(law_effect_t::manpower); law_manpower != fixed_point_t::_0) {
		manpower.multiplier += law_manpower;
		const std::int64_t per_mille = (law_manpower * 1000).ceil<std::int32_t>();
		manpower.max_men = std::max<std::int64_t>(0, manpower.max_men + manpower.pop_men * per_mille / 1000);
	}
	if (country.is_at_war()) {
		Rate const& bonus = UnitInstanceGroup::war_defines->manpower_war_bonus;
		manpower.multiplier += bonus.fixed();
		manpower.max_men += bonus.of_int(manpower.pop_men);
	}
	// The mobilised regiments are out of the manpower of the soldiers (their men come from the mobilisation pool), the waiting orders took no men yet
	for (ArmyInstance const& army : country.get_armies()) {
		for (RegimentInstance const& regiment : army.get_regiment_instances()) {
			if (regiment.is_mobilised()) {
				continue;
			}
			manpower.army_men += strength_men(regiment.get_strength());
			manpower.army_full_men += strength_men(regiment.get_max_strength());
			++manpower.regiments;
		}
	}
	for (RecruitmentOrder const& order : recruitment_orders) {
		if (order.country == &country && order.regiment_type != nullptr && !order.mobilisation && !order.waiting) {
			manpower.queued_men += get_regiment_men(*order.regiment_type);
		}
	}
	manpower.free_men = manpower.max_men - manpower.army_men - manpower.queued_men;
	if (manpower.men_per_regiment > 0) {
		manpower.max_regiments = static_cast<std::uint32_t>(manpower.max_men / manpower.men_per_regiment);
		manpower.available_regiments = static_cast<std::uint32_t>(std::max<std::int64_t>(0, manpower.free_men) / manpower.men_per_regiment);
	}
	const std::int64_t over = manpower.army_men - manpower.max_men;
	if (over > 0) {
		OpenVicWarDefines const& wd = *UnitInstanceGroup::war_defines;
		// In ten-thousandths, so that the share of millions of men fits
		manpower.over_share = manpower.max_men > 0
			? fixed_point_t(static_cast<int32_t>(std::min<std::int64_t>(over * 10000 / manpower.max_men, 1000000))) / 10000
			: fixed_point_t::_1;
		// A little over (the pops of soldiers change from month to month, the armies of the start are at the maximum) is no penalty yet
		if (manpower.over_share > wd.over_manpower_tolerance.fixed()) {
			manpower.combat_multiplier = std::max(
				wd.over_manpower_min_mult.fixed(), fixed_point_t::_1 - wd.over_manpower_penalty.of(manpower.over_share)
			);
		}
	}
	return manpower;
}

void UnitInstanceManager::apply_losses(RegimentInstance& regiment, CountryInstance& country) {
	const std::int64_t men = strength_men(regiment.get_unapplied_losses());
	if (men <= 0) {
		return;
	}
	// The whole men are taken off, the rest of a man waits for the next losses
	regiment.set_unapplied_losses(
		regiment.get_unapplied_losses() - fixed_point_t(static_cast<int32_t>(men)) / UnitInstanceGroup::war_defines->template_men_per_strength
	);
	country.war_losses_today += men;
	std::int64_t left = men;
	// A pop is never emptied by the losses (it keeps a man), what it does not have dies in the other pops of soldiers of the country (of a
	// mobilised regiment: in the other poor pops of the mobilisation)
	const bool mobilised = regiment.is_mobilised();
	const auto take = [this, &left](Pop& pop) -> void {
		const std::int64_t can = std::min<std::int64_t>(left, type_safe::get(pop.get_size()) - 1);
		if (can > 0) {
			pop.change_size(-static_cast<std::int32_t>(can), 6);
			pop.get_location().mark_political_support_dirty();
			left -= can;
			men_killed_total += can;
		}
	};
	if (Pop* const pop = regiment.get_pop()) {
		take(*pop);
	}
	for (ProvinceInstance* province : country.get_owned_provinces()) {
		if (left <= 0) {
			break;
		}
		for (Pop const& pop : province->get_pops()) {
			if (left <= 0) {
				break;
			}
			if (mobilised ? is_mobilisation_pop_type(pop.get_type()) : pop.get_type().can_be_recruited) {
				take(*province->find_pop_by_id(pop.id_in_province));
			}
		}
	}
}

void UnitInstanceManager::apply_all_losses() {
	for (ArmyInstance& army : armies) {
		for (RegimentInstance& regiment : army.get_regiment_instances()) {
			if (regiment.get_unapplied_losses() > fixed_point_t::_0) {
				apply_losses(regiment, army.get_country());
			}
		}
	}
}

void UnitInstanceManager::kill_group(UnitInstanceGroup& group) {
	if (group.branch == LAND) {
		for (RegimentInstance& regiment : static_cast<ArmyInstance&>(group).get_regiment_instances()) {
			regiment.take_damage(fixed_point_t::_0, regiment.get_strength());
		}
	}
	// The armies on board of a navy die with it in destroy_group
	destroy_group(group);
}

namespace {
	// The regiment types of the cells of a template (in the order of the cells)
	std::vector<RegimentType const*> template_regiment_types(UnitTypeManager const& unit_type_manager, DivisionTemplate const& division_template) {
		std::vector<RegimentType const*> types;
		for (const std::uint8_t cell : division_template.cells) {
			if (cell != 0) {
				if (RegimentType const* const type = unit_type_manager.get_regiment_type_by_index(regiment_type_index_t(cell - 1)); type != nullptr) {
					types.push_back(type);
				}
			}
		}
		return types;
	}
}

std::string UnitInstanceManager::get_division_recruitment_block_reason(
	CountryInstance const& country, UnitTypeManager const& unit_type_manager, DivisionTemplate const& division_template, ProvinceInstance const& province
) const {
	if (province.get_owner() != &country || province.get_controller() != &country) {
		return "Набор возможен только в своей, не оккупированной провинции";
	}
	const std::vector<RegimentType const*> types = template_regiment_types(unit_type_manager, division_template);
	if (types.empty()) {
		return "В шаблоне нет полков";
	}
	for (RegimentType const* type : types) {
		if (!country.is_unit_type_unlocked(*type)) {
			return "Тип полка ещё не открыт";
		}
	}
	const Manpower manpower = get_manpower(country);
	std::int64_t needed_men = 0;
	for (RegimentType const* type : types) {
		needed_men += get_regiment_men(*type);
	}
	if (manpower.free_men < needed_men) {
		return fmt::format(
			"Не хватает людских ресурсов: нужно {} полков ({} чел.), свободно {} чел.", types.size(), needed_men,
			std::max<std::int64_t>(0, manpower.free_men)
		);
	}
	std::vector<std::pair<GoodDefinition const*, fixed_point_t>> needed;
	for (RegimentType const* type : types) {
		for (auto const& [good, quantity] : type->get_build_cost()) {
			auto it = std::find_if(needed.begin(), needed.end(), [good](auto const& entry) -> bool { return entry.first == good; });
			if (it == needed.end()) {
				needed.emplace_back(good, quantity);
			} else {
				it->second += quantity;
			}
		}
	}
	for (auto const& [good, quantity] : needed) {
		if (country.get_good_data(*good).stockpile_amount < quantity) {
			return "На складе страны не хватает товаров";
		}
	}
	return {};
}

namespace {
	// The pops that give the soldiers of a division: of the province first, then of the other provinces of the country (owned, not occupied) in their order
	std::vector<Pop*> division_pops(CountryInstance& country, ProvinceInstance& province) {
		std::vector<Pop*> pops;
		for (Pop const& pop : province.get_pops()) {
			if (pop_gives_regiment(country, pop)) {
				pops.push_back(province.find_pop_by_id(pop.id_in_province));
			}
		}
		for (ProvinceInstance* other : country.get_owned_provinces()) {
			if (other == &province || other->get_controller() != &country) {
				continue;
			}
			for (Pop const& pop : other->get_pops()) {
				if (pop_gives_regiment(country, pop)) {
					pops.push_back(other->find_pop_by_id(pop.id_in_province));
				}
			}
		}
		return pops;
	}

	// The pop with the most men not yet in regiments (of the province first): the losses of the regiment are taken off it
	Pop* pick_division_pop(std::vector<Pop*> const& pops, ProvinceInstance const& province, const std::int64_t men_per_regiment) {
		Pop* best = nullptr;
		std::int64_t best_free = 0;
		bool best_local = false;
		for (Pop* pop : pops) {
			const std::int64_t free = type_safe::get(pop->get_size()) - static_cast<std::int64_t>(pop->get_regiment_count()) * men_per_regiment;
			const bool local = &pop->get_location() == &province && free > 0;
			if (best == nullptr || (local && !best_local) || (local == best_local && free > best_free)) {
				best = pop;
				best_free = free;
				best_local = local;
			}
		}
		return best;
	}

	void take_build_cost(CountryInstance& country, RegimentType const& type) {
		for (auto const& [good, quantity] : type.get_build_cost()) {
			CountryInstance::good_data_t& good_data = country.get_good_data(*good);
			good_data.stockpile_amount -= quantity;
			good_data.stockpile_change_yesterday -= quantity;
		}
	}
}

bool UnitInstanceManager::start_division_recruitment(
	CountryInstance& country, UnitTypeManager const& unit_type_manager, DivisionTemplate const& division_template, ProvinceInstance& province
) {
	if (!get_division_recruitment_block_reason(country, unit_type_manager, division_template, province).empty()) {
		return false;
	}
	const std::vector<RegimentType const*> types = template_regiment_types(unit_type_manager, division_template);
	const std::vector<Pop*> pops = division_pops(country, province);
	int32_t days = 1;
	for (RegimentType const* type : types) {
		days = std::max(days, get_recruitment_days(country, *type));
	}
	const std::uint32_t division_id = next_division_id++;
	if (pops.empty()) {
		return false;
	}
	// Over the monthly limit the whole division waits: no men, no goods, no pops yet (start_waiting_order). A division bigger than the limit
	// starts alone at the beginning of a month
	const std::int64_t limit_left = get_recruit_limit_left(country);
	const bool waiting = limit_left >= 0
		&& (limit_left == 0 || (static_cast<std::int64_t>(types.size()) > limit_left && country.recruited_this_month > 0));
	const std::int64_t men_per_regiment = static_cast<std::int64_t>(type_safe::get(military_defines.get_pop_size_per_regiment()));
	for (RegimentType const* type : types) {
		Pop* pop = nullptr;
		if (!waiting) {
			pop = pick_division_pop(pops, province, men_per_regiment);
			pop->add_recruit();
			take_build_cost(country, *type);
		}
		RecruitmentOrder order { unique_id_counter++, &country, type, nullptr, &province, pop, days, days };
		order.division_id = division_id;
		order.template_id = division_template.id;
		order.waiting = waiting;
		recruitment_orders.push_back(order);
	}
	if (!waiting) {
		count_recruited(country, static_cast<std::uint32_t>(types.size()));
	}
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
) const {
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
	if (get_manpower(country).free_men < get_regiment_men(regiment_type)) {
		return "Не хватает людских ресурсов";
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

	const int32_t days = get_recruitment_days(country, regiment_type);
	RecruitmentOrder order { unique_id_counter++, &country, &regiment_type, nullptr, &province, &pop, days, days };
	// Over the monthly limit the order waits: the men and the goods are taken when it starts (start_waiting_order)
	if (get_recruit_limit_left(country) == 0) {
		order.waiting = true;
		recruitment_orders.push_back(order);
		return true;
	}
	pop.add_recruit();
	take_build_cost(country, regiment_type);
	count_recruited(country, 1);
	recruitment_orders.push_back(order);
	return true;
}

bool UnitInstanceManager::cancel_recruitment(CountryInstance& country, const unique_id_t order_id) {
	// A mobilised regiment is cancelled only by the demobilisation; a regiment of a division: the whole division is cancelled
	for (RecruitmentOrder const& order : recruitment_orders) {
		if (order.id == order_id && order.mobilisation) {
			return false;
		}
		if (order.id == order_id && order.division_id != 0 && order.country == &country) {
			const std::uint32_t division_id = order.division_id;
			std::vector<unique_id_t> ids;
			for (RecruitmentOrder const& other : recruitment_orders) {
				if (other.division_id == division_id && other.country == &country) {
					ids.push_back(other.id);
				}
			}
			for (const unique_id_t id : ids) {
				for (auto it = recruitment_orders.begin(); it != recruitment_orders.end(); ++it) {
					if (it->id == id) {
						it->division_id = 0;
						break;
					}
				}
				cancel_recruitment(country, id);
			}
			return true;
		}
	}
	for (auto it = recruitment_orders.begin(); it != recruitment_orders.end(); ++it) {
		if (it->id != order_id) {
			continue;
		}
		if (it->country != &country) {
			return false;
		}
		// A waiting order took nothing yet
		if (it->waiting) {
			recruitment_orders.erase(it);
			return true;
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
	// The new army keeps the supplies, the priority and the province it came from of the one it was split from
	new_army.set_previous_location(army.get_previous_location());
	new_army.set_supply_level(army.get_supply_level());
	new_army.set_supply_priority(army.get_supply_priority());
	new_army.set_supply_priority_custom(army.is_supply_priority_custom());
	// The regiment leaves its division (the next day the new army puts it into its reserve)
	regiment.set_division_id(0);
	// A division split from an army of a group stays in the group, under the general of that army
	if (army.get_army_group_id() != 0) {
		new_army.set_army_group_id(army.get_army_group_id());
		new_army.set_parent_army_id(army.get_parent_army_id() != 0 ? army.get_parent_army_id() : army.unique_id);
	}

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

	// The supply level of the joined army is the average of the two (by the number of regiments)
	const size_t target_count = target.get_unit_count();
	const size_t source_count = source.get_unit_count();
	if (target_count + source_count > 0) {
		target.set_supply_level(
			(target.get_supply_level() * static_cast<std::int32_t>(target_count)
				+ source.get_supply_level() * static_cast<std::int32_t>(source_count))
				/ static_cast<std::int32_t>(target_count + source_count)
		);
	}

	// The divisions of the source come with their regiments (the ids are unique in the game)
	for (DivisionInstance const& division : source.get_divisions()) {
		target.get_mutable_divisions().push_back(division);
	}
	source.get_mutable_divisions().clear();

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

RegimentType const* UnitInstanceManager::get_upgrade_target(UnitTypeManager const& unit_types, RegimentType const& regiment_type) {
	return regiment_type.ov.upgrades_to.empty() ? nullptr : unit_types.get_regiment_type_by_identifier(regiment_type.ov.upgrades_to);
}

std::string_view UnitInstanceManager::get_upgrade_block_reason(
	CountryInstance const& country, ArmyInstance const& army, RegimentInstance const& regiment, RegimentType const* target
) {
	RegimentType const& regiment_type = regiment.get_regiment_type();
	if (&army.get_country() != &country) {
		return "Полк другой страны";
	}
	if (target == nullptr) {
		return "Этот тип полка не улучшается";
	}
	if (!country.is_unit_type_unlocked(*target)) {
		return "Тип улучшения ещё не открыт";
	}
	if (army.is_in_combat()) {
		return "Армия в бою";
	}
	for (auto const& [good, quantity] : regiment_type.ov.upgrade_cost) {
		if (country.get_good_data(*good).stockpile_amount < quantity) {
			return "На складе страны не хватает товаров";
		}
	}
	return {};
}

bool UnitInstanceManager::upgrade_regiment(CountryInstance& country, ArmyInstance& army, RegimentInstance& regiment, RegimentType const& target_type) {
	if (!get_upgrade_block_reason(country, army, regiment, &target_type).empty()) {
		return false;
	}
	RegimentType const& old_type = regiment.get_regiment_type();
	RegimentType const* const target = &target_type;

	for (auto const& [good, quantity] : old_type.ov.upgrade_cost) {
		CountryInstance::good_data_t& good_data = country.get_good_data(*good);
		good_data.stockpile_amount -= quantity;
		good_data.stockpile_change_yesterday -= quantity;
	}

	// The unit type of an instance is a reference that cannot change: the regiment is made again under the same id, with the same pop (the slot of
	// the pop is not freed), name and division, and the same share of strength and organisation
	const unique_id_t id = regiment.unique_id;
	const memory::string name { regiment.get_name() };
	Pop* const pop = regiment.get_pop();
	const bool mobilised = regiment.is_mobilised();
	const std::uint32_t division_id = regiment.get_division_id();
	const fixed_point_t supply_reserve = regiment.get_supply_reserve();
	const fixed_point_t unapplied_losses = regiment.get_unapplied_losses();
	const fixed_point_t strength_share = old_type.max_strength > fixed_point_t::_0 ? regiment.get_strength() / old_type.max_strength : fixed_point_t::_1;
	const fixed_point_t organisation_share = regiment.get_max_organisation() > fixed_point_t::_0 ? regiment.get_organisation() / regiment.get_max_organisation() : fixed_point_t::_1;

	army.remove_unit(regiment);
	unit_instance_map.erase(id);
	regiments.erase(regiments.get_iterator(&regiment));

	RegimentInstance& upgraded = *regiments.insert(RegimentInstance { id, name, *target, pop, mobilised });
	unit_instance_map.emplace(upgraded.unique_id, upgraded);
	upgraded.set_division_id(division_id);
	upgraded.set_supply_reserve(supply_reserve);
	upgraded.take_damage(
		upgraded.get_organisation() - upgraded.get_max_organisation() * organisation_share, upgraded.get_strength() - target->max_strength * strength_share
	);
	upgraded.set_unapplied_losses(unapplied_losses);
	army.add_unit(upgraded);
	army.update_gamestate();
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
	while (UnitInstanceGroup::war_defines->transport_unlimited == 0 && navy.get_carried_regiment_count() > navy.get_transport_capacity() && !navy.get_carried_armies().empty()) {
		ArmyInstance& army = navy.get_carried_armies().back().get();
		navy.forget_carried_army(army);
		army.set_transport_navy(nullptr);
		kill_group(army);
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

ArmyInstance* UnitInstanceManager::create_army_with_regiments(
	CountryInstance& country, ProvinceInstance& province, RegimentType const& regiment_type, const size_t count,
	std::string_view army_name
) {
	if (count == 0) {
		return nullptr;
	}
	const unique_id_t army_id = unique_id_counter++;
	ArmyInstance& army = *armies.emplace(army_id, fmt::format("{}", army_name), country, province);
	unit_instance_group_map.emplace(army.unique_id, army);

	for (size_t i = 0; i < count; ++i) {
		const unique_id_t regiment_id = unique_id_counter++;
		RegimentInstance& regiment = *regiments.insert(RegimentInstance {
			regiment_id, fmt::format("{} {}", regiment_type.get_identifier(), regiment_id), regiment_type, nullptr, false
		});
		unit_instance_map.emplace(regiment.unique_id, regiment);
		army.add_unit(regiment);
	}
	army.update_gamestate();
	return &army;
}

// ---- The monthly limit of the recruitment and the priorities of the orders (docs/RECRUITMENT.md)

std::int64_t UnitInstanceManager::get_recruit_limit_left(CountryInstance const& country) const {
	const std::int32_t limit = UnitInstanceGroup::war_defines != nullptr ? UnitInstanceGroup::war_defines->recruit_max_regiments_per_month : 0;
	if (limit <= 0) {
		return -1;
	}
	const std::uint32_t done = country.recruit_month == current_month ? country.recruited_this_month : 0;
	return std::max<std::int64_t>(0, static_cast<std::int64_t>(limit) - static_cast<std::int64_t>(done));
}

void UnitInstanceManager::count_recruited(CountryInstance& country, const std::uint32_t regiment_count) const {
	if (UnitInstanceGroup::war_defines == nullptr || UnitInstanceGroup::war_defines->recruit_max_regiments_per_month <= 0) {
		return;
	}
	if (country.recruit_month != current_month) {
		country.recruit_month = current_month;
		country.recruited_this_month = 0;
	}
	country.recruited_this_month += regiment_count;
}

bool UnitInstanceManager::start_waiting_order(const std::size_t index) {
	RecruitmentOrder& first = recruitment_orders[index];
	CountryInstance& country = *first.country;
	ProvinceInstance& province = *first.province;
	if (first.regiment_type == nullptr) {
		first.waiting = false;
		return true;
	}
	if (first.division_id == 0) {
		// A regiment of its own: the same checks as when it was ordered (the men are free, the goods are there)
		if (first.pop == nullptr || !get_recruitment_block_reason(country, *first.regiment_type, province, *first.pop).empty()) {
			return false;
		}
		first.pop->add_recruit();
		take_build_cost(country, *first.regiment_type);
		first.waiting = false;
		first.days_left = first.days_total;
		return true;
	}
	// A division: all its waiting regiments start together, or none
	const std::uint32_t division_id = first.division_id;
	std::vector<std::size_t> members;
	for (std::size_t i = 0; i < recruitment_orders.size(); ++i) {
		if (recruitment_orders[i].division_id == division_id && recruitment_orders[i].country == &country && recruitment_orders[i].waiting) {
			members.push_back(i);
		}
	}
	if (province.get_owner() != &country || province.get_controller() != &country) {
		return false;
	}
	std::int64_t needed_men = 0;
	std::vector<std::pair<GoodDefinition const*, fixed_point_t>> needed;
	for (const std::size_t i : members) {
		RegimentType const& type = *recruitment_orders[i].regiment_type;
		if (!country.is_unit_type_unlocked(type)) {
			return false;
		}
		needed_men += get_regiment_men(type);
		for (auto const& [good, quantity] : type.get_build_cost()) {
			auto it = std::find_if(needed.begin(), needed.end(), [good](auto const& entry) -> bool { return entry.first == good; });
			if (it == needed.end()) {
				needed.emplace_back(good, quantity);
			} else {
				it->second += quantity;
			}
		}
	}
	if (get_manpower(country).free_men < needed_men) {
		return false;
	}
	for (auto const& [good, quantity] : needed) {
		if (country.get_good_data(*good).stockpile_amount < quantity) {
			return false;
		}
	}
	const std::vector<Pop*> pops = division_pops(country, province);
	if (pops.empty()) {
		return false;
	}
	const std::int64_t men_per_regiment = static_cast<std::int64_t>(type_safe::get(military_defines.get_pop_size_per_regiment()));
	for (const std::size_t i : members) {
		RecruitmentOrder& order = recruitment_orders[i];
		Pop* const pop = pick_division_pop(pops, province, men_per_regiment);
		pop->add_recruit();
		take_build_cost(country, *order.regiment_type);
		order.pop = pop;
		order.waiting = false;
		order.days_left = order.days_total;
	}
	return true;
}

void UnitInstanceManager::start_waiting_orders(const Date today) {
	current_month = static_cast<std::int32_t>(today.get_year()) * 12 + static_cast<std::int32_t>(today.get_month()) - 1;
	// The waiting orders (one entry for a division): the higher priority first, then the older
	std::vector<std::size_t> waiting;
	for (std::size_t i = 0; i < recruitment_orders.size(); ++i) {
		RecruitmentOrder const& order = recruitment_orders[i];
		if (!order.waiting) {
			continue;
		}
		if (order.division_id != 0 && std::any_of(waiting.begin(), waiting.end(), [this, &order](const std::size_t other) -> bool {
			return recruitment_orders[other].division_id == order.division_id && recruitment_orders[other].country == order.country;
		})) {
			continue;
		}
		waiting.push_back(i);
	}
	if (waiting.empty()) {
		return;
	}
	std::stable_sort(waiting.begin(), waiting.end(), [this](const std::size_t a, const std::size_t b) -> bool {
		RecruitmentOrder const& first = recruitment_orders[a];
		RecruitmentOrder const& second = recruitment_orders[b];
		return first.priority != second.priority ? first.priority > second.priority : first.id < second.id;
	});
	for (const std::size_t index : waiting) {
		CountryInstance& country = *recruitment_orders[index].country;
		const std::int64_t left = get_recruit_limit_left(country);
		std::uint32_t regiment_count = 1;
		if (recruitment_orders[index].division_id != 0) {
			regiment_count = 0;
			for (RecruitmentOrder const& order : recruitment_orders) {
				regiment_count += order.waiting && order.division_id == recruitment_orders[index].division_id && order.country == &country;
			}
		}
		const std::uint32_t done = country.recruit_month == current_month ? country.recruited_this_month : 0;
		// A division bigger than what is left waits for a month of its own (it starts alone when nothing was started yet)
		if (left == 0 || (left > 0 && static_cast<std::int64_t>(regiment_count) > left && done > 0)) {
			continue;
		}
		if (start_waiting_order(index)) {
			count_recruited(country, regiment_count);
		}
	}
}

bool UnitInstanceManager::set_recruitment_priority(CountryInstance& country, const unique_id_t order_id, const std::uint32_t priority) {
	const std::uint32_t new_priority = std::min(priority, RECRUITMENT_PRIORITY_MAX);
	std::uint32_t division_id = 0;
	bool found = false;
	for (RecruitmentOrder const& order : recruitment_orders) {
		if (order.id == order_id && order.country == &country && !order.mobilisation) {
			division_id = order.division_id;
			found = true;
			break;
		}
	}
	if (!found) {
		return false;
	}
	bool changed = false;
	for (RecruitmentOrder& order : recruitment_orders) {
		if (order.country == &country && (order.id == order_id || (division_id != 0 && order.division_id == division_id))
			&& order.priority != new_priority) {
			order.priority = new_priority;
			changed = true;
		}
	}
	return changed;
}

// ---- The mobilisation (docs/RECRUITMENT.md, "Мобилизация")

bool UnitInstanceManager::is_mobilisation_pop_type(PopType const& pop_type) {
	return !pop_type.can_be_recruited && !pop_type.is_slave && pop_type.strata.get_identifier() == "poor";
}

bool UnitInstanceManager::is_mobilisation_pop(CountryInstance const& country, Pop const& pop) {
	return is_mobilisation_pop_type(pop.get_type()) && pop.get_size() > pop_size_t(0)
		&& Pop::is_culture_status_allowed(country.get_allowed_regiment_cultures(), pop.get_culture_status());
}

std::int32_t UnitInstanceManager::get_mobilisation_days(CountryInstance const& country, ProvinceInstance const& province) const {
	OpenVicWarDefines const& wd = *UnitInstanceGroup::war_defines;
	const fixed_point_t reference_speed = wd.mobilisation_reference_speed.fixed();
	fixed_point_t speed = reference_speed;
	fixed_point_t rails_mult = fixed_point_t::_0;
	if (country_defines != nullptr) {
		if (country_defines->get_mobilization_speed_base() > fixed_point_t::_0) {
			speed = country_defines->get_mobilization_speed_base();
		}
		rails_mult = std::max(fixed_point_t::_0, country_defines->get_mobilization_speed_rails_mult());
	}
	speed *= fixed_point_t::_1 + rails_mult * std::clamp(province.get_infrastructure_fraction(), fixed_point_t::_0, fixed_point_t::_1);
	if (modifier_effect_cache != nullptr) {
		if (modifier_effect_cache->get_mobilisation_speed_pct() != nullptr) {
			speed *= std::max(fixed_point_t::_0, fixed_point_t::_1 + country.get_modifier_effect_value(*modifier_effect_cache->get_mobilisation_speed_pct()));
		}
		speed *= std::max(fixed_point_t::_0, get_effect_product(country, modifier_effect_cache->get_mobilisation_speed_mult()));
	}
	if (speed <= fixed_point_t::_0) {
		return 3650;
	}
	const fixed_point_t days = fixed_point_t(wd.mobilisation_days) * reference_speed / speed;
	return std::clamp<std::int32_t>(days.ceil<std::int32_t>(), 1, 3650);
}

std::vector<std::pair<Pop*, std::int64_t>> UnitInstanceManager::get_mobilisation_men(CountryInstance const& country, const fixed_point_t share) const {
	OpenVicWarDefines const& wd = *UnitInstanceGroup::war_defines;
	struct RegionSum {
		std::int64_t men = 0;
		std::int64_t cap = 0;
	};
	std::vector<std::pair<Pop*, std::int64_t>> result;
	std::vector<State const*> result_states;
	std::vector<std::pair<State const*, RegionSum>> regions;
	const auto region_of = [&regions](State const* state) -> RegionSum& {
		for (auto& entry : regions) {
			if (entry.first == state) {
				return entry.second;
			}
		}
		return regions.emplace_back(state, RegionSum {}).second;
	};
	for (ProvinceInstance* province : country.get_owned_provinces()) {
		if (province->get_controller() != &country) {
			continue;
		}
		// The national land (the cores of the country) gives the share, the rest MOBILISATION_NON_NATIONAL_FACTOR of it; a region gives at most
		// MOBILISATION_REGION_MAX of its population (MOBILISATION_REGION_MAX_NON_NATIONAL of the population of its land that is not national)
		const bool national = province->is_owner_core();
		const fixed_point_t province_share = national ? share : wd.mobilisation_non_national_factor.of(share);
		RegionSum& region = region_of(province->get_state());
		region.cap += (national ? wd.mobilisation_region_max : wd.mobilisation_region_max_non_national)
			.of_int(static_cast<std::int64_t>(type_safe::get(province->get_total_population())));
		for (Pop const& pop : province->get_pops()) {
			if (!is_mobilisation_pop(country, pop)) {
				continue;
			}
			// A pop that gives no man still takes the regiments of the minimum of the mobilisation (MIN_MOBILIZE_LIMIT) by the largest remainder
			const std::int64_t men = (static_cast<std::int64_t>(province_share.get_raw_value()) * type_safe::get(pop.get_size())) >> fixed_point_t::PRECISION;
			region.men += men;
			result.emplace_back(province->find_pop_by_id(pop.id_in_province), men);
			result_states.push_back(province->get_state());
		}
	}
	// The regions over their cap give the same share of what their pops would give
	for (std::size_t i = 0; i < result.size(); ++i) {
		RegionSum const& region = region_of(result_states[i]);
		if (region.men > region.cap) {
			result[i].second = region.cap > 0 ? result[i].second * region.cap / region.men : 0;
		}
	}
	return result;
}

Mobilisation UnitInstanceManager::get_mobilisation(CountryInstance const& country) const {
	Mobilisation mobilisation;
	OpenVicWarDefines const& wd = *UnitInstanceGroup::war_defines;
	mobilisation.men_per_regiment = static_cast<std::int32_t>(type_safe::get(military_defines.get_pop_size_per_regiment()));
	// The share: the base of OpenVic and the mobilisation_size of the modifiers (the national values, the technologies of Vic2), times the
	// mobilisation_size_pct of the technologies of OpenVic
	fixed_point_t share = wd.mobilisation_pop_share.fixed();
	fixed_point_t share_pct = fixed_point_t::_0;
	if (modifier_effect_cache != nullptr) {
		if (modifier_effect_cache->get_mobilisation_size_country() != nullptr) {
			share += country.get_modifier_effect_value(*modifier_effect_cache->get_mobilisation_size_country());
		}
		if (modifier_effect_cache->get_mobilisation_size_tech() != nullptr) {
			share += country.get_modifier_effect_value(*modifier_effect_cache->get_mobilisation_size_tech());
		}
		if (modifier_effect_cache->get_mobilisation_size_pct() != nullptr) {
			share_pct = country.get_modifier_effect_value(*modifier_effect_cache->get_mobilisation_size_pct());
		}
	}
	mobilisation.share = std::max(fixed_point_t::_0, share * std::max(fixed_point_t::_0, fixed_point_t::_1 + share_pct));
	// The model of the army (the laws: professional -50 %, mobilisation army +100 %) and the kind of the mobilisation the country chose (limited,
	// standard, extended: the share of the pool it raises)
	mobilisation.share = mobilisation.share * std::max(fixed_point_t::_0, fixed_point_t::_1 + country.get_law_effect(law_effect_t::mobilisation_size));
	{
		Rate const& level = country.get_mobilisation_level() == 0 ? wd.mobilisation_level_limited
			: country.get_mobilisation_level() == 2 ? wd.mobilisation_level_extended : wd.mobilisation_level_standard;
		mobilisation.share = mobilisation.share * level.fixed();
	}
	bool first_province = true;
	for (ProvinceInstance const* province : country.get_owned_provinces()) {
		if (province->get_controller() != &country) {
			continue;
		}
		std::int64_t province_men = 0;
		for (Pop const& pop : province->get_pops()) {
			if (is_mobilisation_pop(country, pop)) {
				province_men += type_safe::get(pop.get_size());
			}
		}
		if (province_men <= 0) {
			continue;
		}
		mobilisation.pop_men += province_men;
		const std::int32_t days = get_mobilisation_days(country, *province);
		mobilisation.days_min = first_province ? days : std::min(mobilisation.days_min, days);
		mobilisation.days_max = first_province ? days : std::max(mobilisation.days_max, days);
		first_province = false;
	}
	for (auto const& [pop, men] : get_mobilisation_men(country, mobilisation.share)) {
		mobilisation.pool_men += men;
	}
	if (mobilisation.pop_men > 0 && mobilisation.men_per_regiment > 0) {
		const std::uint32_t min_regiments = country_defines != nullptr ? static_cast<std::uint32_t>(country_defines->get_min_mobilize_limit()) : 0;
		mobilisation.pool_regiments = std::max(
			min_regiments, static_cast<std::uint32_t>(mobilisation.pool_men / mobilisation.men_per_regiment)
		);
	}
	for (ArmyInstance const& army : country.get_armies()) {
		for (RegimentInstance const& regiment : army.get_regiment_instances()) {
			if (regiment.is_mobilised()) {
				++mobilisation.regiments;
				mobilisation.men += strength_men(regiment.get_strength());
				mobilisation.full_men += strength_men(regiment.get_max_strength());
			}
		}
	}
	for (RecruitmentOrder const& order : recruitment_orders) {
		if (order.country == &country && order.mobilisation && order.regiment_type != nullptr) {
			++mobilisation.pending_regiments;
			mobilisation.pending_men += get_regiment_men(*order.regiment_type);
			mobilisation.days_left = std::max(mobilisation.days_left, order.days_left);
		}
	}
	mobilisation.free_men = mobilisation.pool_men - mobilisation.men - mobilisation.pending_men;
	mobilisation.economy_impact = std::max(
		fixed_point_t::_0, wd.mobilisation_economy_impact_base.fixed() + country.get_mobilisation_economy_impact()
	) * std::max(fixed_point_t::_0, fixed_point_t::_1 + country.get_law_effect(law_effect_t::mobilisation_impact));
	// In ten-thousandths, so that the share of millions of men fits
	const auto output_of = [&mobilisation, &wd](const std::int64_t taken_men) -> std::pair<fixed_point_t, fixed_point_t> {
		if (mobilisation.pop_men <= 0 || taken_men <= 0) {
			return { fixed_point_t::_0, fixed_point_t::_1 };
		}
		const fixed_point_t taken = fixed_point_t(static_cast<int32_t>(std::min<std::int64_t>(taken_men * 10000 / mobilisation.pop_men, 10000))) / 10000;
		return { taken, std::max(wd.mobilisation_output_min_mult.fixed(), fixed_point_t::_1 - mobilisation.economy_impact * taken) };
	};
	const auto [taken_share, output_multiplier] = output_of(mobilisation.men + mobilisation.pending_men);
	mobilisation.taken_share = taken_share;
	mobilisation.output_multiplier = output_multiplier;
	mobilisation.planned_output_multiplier =
		output_of(static_cast<std::int64_t>(mobilisation.pool_regiments) * mobilisation.men_per_regiment).second;
	return mobilisation;
}

std::string_view UnitInstanceManager::get_mobilise_block_reason(CountryInstance const& country) const {
	if (country.is_rebel_country() || !country.exists()) {
		return "Страна не может мобилизоваться";
	}
	if (country.is_mobilised()) {
		return "Страна уже мобилизована";
	}
	if (!country.has_unlock(UNLOCK_MECHANIC_MOBILISATION)) {
		return "Мобилизация ещё не открыта (технология «Генеральный штаб»)";
	}
	if (get_mobilisation(country).pool_regiments == 0) {
		return "Нет бедного населения, из которого можно мобилизовать полки";
	}
	return {};
}

std::string_view UnitInstanceManager::get_demobilise_block_reason(CountryInstance const& country) const {
	if (!country.is_mobilised()) {
		return "Страна не мобилизована";
	}
	for (ArmyInstance const& army : country.get_armies()) {
		if (!army.is_in_combat()) {
			continue;
		}
		for (RegimentInstance const& regiment : army.get_regiment_instances()) {
			if (regiment.is_mobilised()) {
				return "Мобилизованные полки в бою";
			}
		}
	}
	return {};
}

bool UnitInstanceManager::mobilise(CountryInstance& country) {
	if (!get_mobilise_block_reason(country).empty() || unit_type_manager == nullptr) {
		return false;
	}
	const Mobilisation mobilisation = get_mobilisation(country);
	// The types: the cells of the first template of the country in their order (the regiments take them in turn), else the infantry
	std::vector<RegimentType const*> types;
	if (!country.get_division_templates().empty()) {
		for (RegimentType const* type : template_regiment_types(*unit_type_manager, country.get_division_templates().front())) {
			if (country.is_unit_type_unlocked(*type)) {
				types.push_back(type);
			}
		}
	}
	if (types.empty()) {
		RegimentType const* fallback = unit_type_manager->get_regiment_type_by_identifier("line_infantry");
		if (fallback == nullptr || !country.is_unit_type_unlocked(*fallback)) {
			fallback = nullptr;
			RegimentType const* any = nullptr;
			for (RegimentType const& type : unit_type_manager->get_regiment_types()) {
				if (!country.is_unit_type_unlocked(type)) {
					continue;
				}
				if (any == nullptr) {
					any = &type;
				}
				if (fallback == nullptr && get_unit_category_scope(type) == "infantry") {
					fallback = &type;
				}
			}
			if (fallback == nullptr) {
				fallback = any;
			}
		}
		if (fallback == nullptr) {
			return false;
		}
		types.push_back(fallback);
	}
	// Every pop of the pool gets the regiments of its share: the whole ones first, the rest by the largest remainder (the order of the pops on a tie)
	struct Quota {
		Pop* pop;
		std::int64_t men;
		std::uint32_t regiments;
	};
	std::vector<Quota> quotas;
	const std::int64_t men_per_regiment = std::max<std::int64_t>(1, mobilisation.men_per_regiment);
	std::uint32_t assigned = 0;
	for (auto const& [pop, men] : get_mobilisation_men(country, mobilisation.share)) {
		Quota quota { pop, men, static_cast<std::uint32_t>(men / men_per_regiment) };
		assigned += quota.regiments;
		quotas.push_back(quota);
	}
	if (quotas.empty()) {
		return false;
	}
	if (assigned < mobilisation.pool_regiments) {
		std::vector<std::size_t> order(quotas.size());
		for (std::size_t i = 0; i < order.size(); ++i) {
			order[i] = i;
		}
		std::stable_sort(order.begin(), order.end(), [&quotas, men_per_regiment](const std::size_t a, const std::size_t b) -> bool {
			return quotas[a].men % men_per_regiment > quotas[b].men % men_per_regiment;
		});
		while (assigned < mobilisation.pool_regiments) {
			for (const std::size_t i : order) {
				if (assigned >= mobilisation.pool_regiments) {
					break;
				}
				++quotas[i].regiments;
				++assigned;
			}
		}
	}
	std::size_t next_type = 0;
	for (Quota const& quota : quotas) {
		ProvinceInstance& province = quota.pop->get_location();
		const std::int32_t days = get_mobilisation_days(country, province);
		for (std::uint32_t i = 0; i < quota.regiments; ++i) {
			RegimentType const* const type = types[next_type++ % types.size()];
			quota.pop->add_recruit();
			RecruitmentOrder order { unique_id_counter++, &country, type, nullptr, &province, quota.pop, days, days };
			order.mobilisation = true;
			recruitment_orders.push_back(order);
		}
	}
	country.set_mobilised(true);
	country.mobilisation_war_seen = country.is_at_war();
	country.mobilisation_peace_days = 0;
	country.mobilisation_output_multiplier = get_mobilisation(country).output_multiplier;
	return true;
}

bool UnitInstanceManager::demobilise(CountryInstance& country) {
	if (!get_demobilise_block_reason(country).empty()) {
		return false;
	}
	// The orders that are not ready: the pops get their men back
	for (auto it = recruitment_orders.begin(); it != recruitment_orders.end();) {
		if (it->country == &country && it->mobilisation) {
			if (it->pop != nullptr) {
				it->pop->release_recruit();
			}
			it = recruitment_orders.erase(it);
		} else {
			++it;
		}
	}
	// The mobilised regiments are disbanded: their men go home (not a loss; what died today is already taken off the pops)
	std::vector<ArmyInstance*> mobilised_armies;
	for (ArmyInstance& army : country.get_armies()) {
		for (RegimentInstance const& regiment : army.get_regiment_instances()) {
			if (regiment.is_mobilised()) {
				mobilised_armies.push_back(&army);
				break;
			}
		}
	}
	for (ArmyInstance* army : mobilised_armies) {
		std::vector<RegimentInstance*> mobilised;
		for (RegimentInstance& regiment : army->get_regiment_instances()) {
			if (regiment.is_mobilised()) {
				mobilised.push_back(&regiment);
			}
		}
		// destroy_unit deletes the army with its last regiment
		for (RegimentInstance* regiment : mobilised) {
			destroy_unit(*regiment, *army);
		}
	}
	country.set_mobilised(false);
	country.mobilisation_war_seen = false;
	country.mobilisation_peace_days = 0;
	country.mobilisation_output_multiplier = fixed_point_t::_1;
	return true;
}

void UnitInstanceManager::update_mobilisation(CountryInstance& country) {
	if (!country.is_mobilised()) {
		country.mobilisation_output_multiplier = fixed_point_t::_1;
		return;
	}
	if (country.is_at_war()) {
		country.mobilisation_war_seen = true;
		country.mobilisation_peace_days = 0;
	} else if (country.mobilisation_war_seen) {
		// The peace: the men go home DEMOBILISE_DAYS after the last war (later if the mobilised regiments are still in a battle, with the rebels)
		++country.mobilisation_peace_days;
		if (country.mobilisation_peace_days >= UnitInstanceGroup::war_defines->demobilise_days && demobilise(country)) {
			return;
		}
	}
	country.mobilisation_output_multiplier = get_mobilisation(country).output_multiplier;
}
