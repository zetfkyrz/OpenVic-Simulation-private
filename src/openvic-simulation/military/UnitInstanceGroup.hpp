#pragma once

#include <functional>
#include <string_view>

#include "openvic-simulation/core/memory/Colony.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/military/Leader.hpp"
#include "openvic-simulation/military/UnitInstance.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/military/UnitType.hpp"
#include "openvic-simulation/types/UnitBranchType.hpp"
#include "openvic-simulation/utility/Getters.hpp"

#include "openvic-simulation/military/UnitBranchedGetterMacro.hpp" //below other imports that undef the macros

namespace OpenVic {
	struct ProvinceInstance;
	struct CountryInstance;
	struct MapInstance;

	struct UnitInstanceGroup {
		// Movement, transports and landings read their parameters from the group ov_war of the defines; the pointer is set
		// by the InstanceManager (the defines belong to the definitions and outlive all instances)
		static inline OpenVicWarDefines const* war_defines = nullptr;
		static void set_war_defines(OpenVicWarDefines const& defines) {
			war_defines = &defines;
		}

	private:
		memory::string PROPERTY(name);
		memory::vector<std::reference_wrapper<UnitInstance>> SPAN_PROPERTY(units);
		LeaderInstance* PROPERTY_PTR(leader, nullptr);
		std::reference_wrapper<ProvinceInstance> location;
		std::reference_wrapper<CountryInstance> country;

		fixed_point_t PROPERTY(total_organisation);
		fixed_point_t PROPERTY(total_max_organisation);
		fixed_point_t PROPERTY(total_strength);
		fixed_point_t PROPERTY(total_max_strength);

		// Movement attributes
		// Ordered list of provinces making up the path the unit is trying to move along,
		// the front province should always be adjacent to the unit's current location.
		memory::vector<std::reference_wrapper<ProvinceInstance>> SPAN_PROPERTY(path);
		// Measured in distance travelled, increases each day by unit speed (after modifiers have been applied) until
		// it reaches the required distance/movement cost to move to the next province in the path.
		fixed_point_t PROPERTY(movement_progress);

		// Set every day by the war manager, a group in combat does not move
		bool PROPERTY_RW_CUSTOM_NAME(in_combat, is_in_combat, set_in_combat, false);

	protected:
		UnitInstanceGroup(
			unique_id_t new_unique_id,
			unit_branch_t new_branch,
			std::string_view new_name,
			CountryInstance& new_country,
			ProvinceInstance& new_location
		);

		void update_gamestate();
		void tick();

	public:
		const unique_id_t unique_id;
		const unit_branch_t branch;

		UnitInstanceGroup(UnitInstanceGroup&&) = default;
		UnitInstanceGroup(UnitInstanceGroup const&) = delete;

		// Mutable access to the references held by the group
		ProvinceInstance& get_location() {
			return location.get();
		}
		ProvinceInstance const& get_location() const {
			return location.get();
		}
		CountryInstance& get_country() {
			return country.get();
		}
		CountryInstance const& get_country() const {
			return country.get();
		}

		size_t get_unit_count() const;
		bool empty() const;
		size_t get_unit_category_count(UnitType::unit_category_t unit_category) const;
		UnitType const* get_display_unit_type() const;

		bool add_unit(UnitInstance& unit);
		bool remove_unit(UnitInstance const& unit);

		void set_name(std::string_view new_name);
		bool set_location(ProvinceInstance& new_location);
		bool set_country(CountryInstance& new_country);
		bool set_leader(LeaderInstance* new_leader);

		fixed_point_t get_organisation_proportion() const;
		fixed_point_t get_strength_proportion() const;

		fixed_point_t get_average_organisation() const;
		fixed_point_t get_average_max_organisation() const;

		// Replaces the path (front province must be adjacent to the current location), resets progress if the first step changed
		void set_path(memory::vector<std::reference_wrapper<ProvinceInstance>>&& new_path);
		void clear_path();

		// Whether a group of the given country may enter the province (own/allied/access/hostile land)
		static bool can_country_enter(CountryInstance const& country, ProvinceInstance const& province);
		// Whether a navy of the given country may enter the province: any sea, or a port of the country or its friends
		static bool can_navy_enter(CountryInstance const& country, ProvinceInstance const& province);
		// Distance of the step between two provinces on the way of the group (a port and its sea have no measured
		// distance), negative if the step is not possible
		fixed_point_t get_step_distance(ProvinceInstance const& from, ProvinceInstance const& to) const;

		// Distance covered each day by the group (the slowest unit decides)
		fixed_point_t get_movement_speed() const;
		// Days until the group reaches the end of its path
		int32_t get_movement_days_left() const;
		bool is_moving() const;
		// The adjacent province that the unit will arrive in next, not necessarily the final destination of its current path
		ProvinceInstance const* get_movement_destination_province() const;
		Date get_movement_arrival_date() const;

		constexpr bool operator==(UnitInstanceGroup const& rhs) const {
			return unique_id == rhs.unique_id;
		}
	};

	template<>
	struct UnitInstanceGroupBranched<unit_branch_t::LAND> : UnitInstanceGroup {
		using dig_in_level_t = uint8_t;

	private:
		NavyInstance* PROPERTY_PTR(transport_navy, nullptr);
		dig_in_level_t PROPERTY(dig_in_level, 0);
		uint16_t idle_days = 0; // Days spent without moving or fighting, dig in grows with them

		bool PROPERTY_RW_CUSTOM_NAME(exiled, is_exiled, set_is_exiled, false);

	public:
		UnitInstanceGroupBranched(
			unique_id_t new_unique_id,
			std::string_view new_name,
			CountryInstance& new_country,
			ProvinceInstance& new_location
		);
		UnitInstanceGroupBranched(UnitInstanceGroupBranched&&) = default;

		void update_gamestate();
		void tick();

		void set_transport_navy(NavyInstance* navy) {
			transport_navy = navy;
		}

		// Called daily: an army that stays still gains a dig in level every days_per_level days (up to cap),
		// moving or fighting resets it
		void advance_dig_in(const int32_t days_per_level, const int32_t cap);
		void reset_dig_in();

		// TODO - do these work fine when units is empty?
		std::span<const std::reference_wrapper<RegimentInstance>> get_regiment_instances() {
			return { reinterpret_cast<std::reference_wrapper<RegimentInstance> const*>(get_units().data()), get_units().size() };
		}
		std::span<const std::reference_wrapper<const RegimentInstance>> get_regiment_instances() const {
			return { reinterpret_cast<std::reference_wrapper<const RegimentInstance> const*>(get_units().data()), get_units().size() };
		}
	};

	template<>
	struct UnitInstanceGroupBranched<unit_branch_t::NAVAL> : UnitInstanceGroup {
	private:
		memory::vector<std::reference_wrapper<ArmyInstance>> SPAN_PROPERTY(carried_armies);

	public:
		UnitInstanceGroupBranched(
			unique_id_t new_unique_id,
			std::string_view new_name,
			CountryInstance& new_country,
			ProvinceInstance& new_location
		);
		UnitInstanceGroupBranched(UnitInstanceGroupBranched&&) = default;

		void update_gamestate();
		void tick();

		size_t get_transport_capacity() const;
		size_t get_carried_regiment_count() const;
		// Loads an army that is in the same port as the navy
		bool load_army(ArmyInstance& army);
		// Unloads a carried army in a port
		bool unload_army(ArmyInstance& army);
		// Lands a carried army from the sea next to the coastal province
		bool land_army(ArmyInstance& army, ProvinceInstance& target);
		// Forgets an army (it was destroyed or left)
		void forget_carried_army(ArmyInstance const& army);

		std::span<const std::reference_wrapper<ShipInstance>> get_ship_instances() {
			return { reinterpret_cast<std::reference_wrapper<ShipInstance> const*>(get_units().data()), get_units().size() };
		}
		std::span<const std::reference_wrapper<const ShipInstance>> get_ship_instances() const {
			return { reinterpret_cast<std::reference_wrapper<const ShipInstance> const*>(get_units().data()), get_units().size() };
		}

		fixed_point_t get_total_consumed_supply() const;
	};

	template<unit_branch_t>
	struct UnitDeployment;

	template<unit_branch_t>
	struct UnitDeploymentGroup;

	struct MapInstance;
	struct Deployment;
	struct CultureManager;
	struct LeaderTraitManager;
	struct MilitaryDefines;
	struct Pop;

	// A regiment that is being recruited from a pop, it appears in the province when the days run out
	struct RecruitmentOrder {
		unique_id_t id;
		CountryInstance* country;
		RegimentType const* regiment_type; // set for a regiment, nullptr for a ship
		ShipType const* ship_type; // set for a ship
		ProvinceInstance* province;
		Pop* pop; // the pop that gives the soldiers, nullptr for a ship
		int32_t days_total;
		int32_t days_left;
	};

	struct UnitInstanceManager {
	private:
		// Used for leader pictures and names
		CultureManager const& culture_manager;
		LeaderTraitManager const& leader_trait_manager;
		MilitaryDefines const& military_defines;

		// TODO - use single counter or separate for leaders vs units vs unit groups? (even separate for branches?)
		// Starts at 1, so ID 0 represents an invalid value
		unique_id_t unique_id_counter = 1;

		// TODO - maps from unique_ids to leader/unit/unit group pointers (one big map or multiple maps?)

		memory::colony<LeaderInstance> PROPERTY(leaders);
		ordered_map<unique_id_t, std::reference_wrapper<LeaderInstance>> PROPERTY(leader_instance_map);

		memory::colony<RegimentInstance> PROPERTY(regiments);
		memory::colony<ShipInstance> PROPERTY(ships);
		ordered_map<unique_id_t, std::reference_wrapper<UnitInstance>> PROPERTY(unit_instance_map);

		OV_UNIT_BRANCHED_GETTER(get_unit_instances, regiments, ships);

		memory::colony<ArmyInstance> PROPERTY(armies);
		memory::colony<NavyInstance> PROPERTY(navies);
		ordered_map<unique_id_t, std::reference_wrapper<UnitInstanceGroup>> PROPERTY(unit_instance_group_map);

		OV_UNIT_BRANCHED_GETTER(get_unit_instance_groups, armies, navies);

		memory::vector<RecruitmentOrder> PROPERTY(recruitment_orders);

		Pop* recruit_pop_in(ProvinceInstance& province, const bool is_rebel) const;
		template<unit_branch_t Branch>
		UnitInstanceBranched<Branch>& generate_unit_instance(
			UnitDeployment<Branch> const& unit_deployment,
			MapInstance& map_instance,
			const bool is_rebel
		);
		template<unit_branch_t Branch>
		bool generate_unit_instance_group(
			MapInstance& map_instance, CountryInstance& country, UnitDeploymentGroup<Branch> const& unit_deployment_group
		);
		template<typename T>
		void generate_leader(CountryInstance& country, T&& leader_base);

	public:
		UnitInstanceManager(
			CultureManager const& new_culture_manager,
			LeaderTraitManager const& new_leader_trait_manager,
			MilitaryDefines const& new_military_defines
		);

		bool generate_deployment(MapInstance& map_instance, CountryInstance& country, Deployment const& deployment);

		void update_gamestate();
		void tick();

		// Finds the shortest path (by land for armies, by sea between ports for navies) and starts moving the group,
		// false if the destination is unreachable. With append = true the route starts at the end of the current route
		// of the group and is added to it
		bool order_move(UnitInstanceGroup& group, ProvinceInstance& destination, MapInstance& map_instance, bool append = false);
		// The nearest port the country's navy can dock in, from the province (nullptr if there is none)
		ProvinceInstance* find_nearest_port(CountryInstance const& country, ProvinceInstance& from, MapInstance& map_instance);

		// Empty if the regiment can be recruited from the pop, otherwise the reason it cannot (shown to the player)
		static std::string_view get_recruitment_block_reason(
			CountryInstance const& country, RegimentType const& regiment_type, ProvinceInstance const& province, Pop const& pop
		);
		// Starts recruiting a regiment from the pop. Takes the build cost from the national stockpile, false if
		// get_recruitment_block_reason is not empty.
		bool start_recruitment(
			CountryInstance& country, RegimentType const& regiment_type, ProvinceInstance& province, Pop& pop
		);
		// The same for building a ship in a port of the province
		std::string_view get_ship_block_reason(
			CountryInstance const& country, ShipType const& ship_type, ProvinceInstance const& province
		) const;
		bool start_ship_building(CountryInstance& country, ShipType const& ship_type, ProvinceInstance& province);
		// Drowns the carried armies that do not fit in the transport ships that are left
		void enforce_transport_capacity(NavyInstance& navy);
		// An army of regiments that need no pop (rebels), appears in the province at once
		ArmyInstance* create_army_with_regiments(
			CountryInstance& country, ProvinceInstance& province, RegimentType const& regiment_type, size_t count,
			std::string_view army_name
		);
		// Cancels an order of the country, the goods are returned
		bool cancel_recruitment(CountryInstance& country, unique_id_t order_id);
		// Days the regiment takes to recruit for the country
		static int32_t get_recruitment_days(CountryInstance const& country, UnitType const& unit_type);

		// Moves a regiment out of its army into a new army in the same province
		ArmyInstance* split_regiment(ArmyInstance& army, RegimentInstance& regiment);
		// Moves all the regiments of the source army into the target army (same country and province), deletes the source
		bool merge_armies(ArmyInstance& target, ArmyInstance& source);
		// Disbands one regiment (the army is deleted with its last regiment), frees the pop slot
		void disband_regiment(ArmyInstance& army, RegimentInstance& regiment);

		// Removes a unit from its group and deletes it, the group is deleted too if it becomes empty.
		// The unit and, possibly, its group must not be used after this call.
		void destroy_unit(UnitInstance& unit, UnitInstanceGroup& group);
		// Deletes a group and all of its units, the group must not be used after this call.
		void destroy_group(UnitInstanceGroup& group);

		// Deletes a leader (died in a battle), takes it off its army
		void destroy_leader(LeaderInstance& leader);

		LeaderInstance* get_leader_instance_by_unique_id(unique_id_t unique_id);
		UnitInstance* get_unit_instance_by_unique_id(unique_id_t unique_id);
		UnitInstanceGroup* get_unit_instance_group_by_unique_id(unique_id_t unique_id);
		// The army that the regiment is in
		ArmyInstance* get_army_of_regiment(RegimentInstance const& regiment);

		// Creates a new leader of the specified branch and adds it to the specified country. The leader's name and traits
		// can be specified, but if they are not, the leader will be generated with a random name and traits. The country's
		// leadership points will be checked and, if there are enough, have the leader creation cost subtracted from them.
		// If the country does not have enough leadership points, the function will return false and no leader will be created.
		bool create_leader(
			CountryInstance& country,
			unit_branch_t branch,
			Date creation_date,
			std::string_view name = {},
			LeaderTrait const* personality = nullptr,
			LeaderTrait const* background = nullptr
		);
	};
}
