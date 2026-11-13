#pragma once

#include <string>

#include <functional>
#include <string_view>

#include "openvic-simulation/military/Division.hpp"
#include "openvic-simulation/military/SupplyPriority.hpp"
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
	struct BuildingType;
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
		ProvinceInstance const* previous_location = nullptr;
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
		// The province the group was in before its present one (nullptr: none, a new group): an army that attacks over a river (BattleConditions)
		[[nodiscard]] ProvinceInstance const* get_previous_location() const {
			return previous_location;
		}
		void set_previous_location(ProvinceInstance const* province) {
			previous_location = province;
		}
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
		using supply_priority_t = OpenVic::supply_priority_t;

	private:
		// The own priority of the army; while supply_priority_custom is false the priority of the country (for all the armies) is used
		supply_priority_t PROPERTY_RW(supply_priority, supply_priority_t::STANDARD);
		bool PROPERTY_RW_CUSTOM_NAME(supply_priority_custom, is_supply_priority_custom, set_supply_priority_custom, false);
		// The supply level (0..1) of the army: the attack and defence of its regiments are multiplied by it
		fixed_point_t PROPERTY_RW(supply_level, fixed_point_t::_1);
		// Worked out every day: the share of the need that the supply limit of the province lets through, how many times more than the need is
		// sent (the priority), the share of the need that arrived (after the stockpile and the budget) and the average reserve in days
		fixed_point_t PROPERTY_RW(supply_delivery, fixed_point_t::_1);
		fixed_point_t PROPERTY_RW(supply_send_multiplier, fixed_point_t::_1);
		fixed_point_t PROPERTY_RW(supply_ratio, fixed_point_t::_1);
		fixed_point_t PROPERTY_RW(supply_reserve_average, fixed_point_t::_0);
		// The load of the province (the supply consumption of the regiments of the country and its friends) over its supply limit with what it
		// takes from the neighbours: above 1 the army suffers the attrition of the overload (ATTRITION_OVERLOAD)
		fixed_point_t PROPERTY_RW(supply_load_ratio, fixed_point_t::_0);
		// The army is below SUPPLY_ALERT_LEVEL of the supply (the message "without supplies" was given)
		bool PROPERTY_RW_CUSTOM_NAME(supply_alert, is_supply_alert, set_supply_alert, false);
		// The divisions of the army (see military/Division.hpp); empty for a country without the technology of the organisation of the army
		memory::vector<DivisionInstance> divisions;
		// The group of armies the army is in (military/ArmyGroup.hpp), 0 if it is in none
		std::uint32_t PROPERTY_RW(army_group_id, 0);
		// A division split from the army of a general of a group (spread along the line of the order of the field marshal) stays his: the unique id of
		// that army, 0 for the army of a general itself (docs/WAR_ASSUMPTIONS.md, section 6). The limit of the marshal counts only the armies with 0
		unique_id_t PROPERTY_RW(parent_army_id, 0);
		// The army of a group moves by a manual order (it is off the order of the marshal, without the bonus of the planning, until it joins it again)
		bool PROPERTY_RW_CUSTOM_NAME(manual_order, is_manual_order, set_manual_order, false);
		// The stance (docs/WAR_ASSUMPTIONS.md, section 10): 0 normal, 1 entrench, 2 raid, 3 rest
		std::uint8_t PROPERTY_RW(stance, 0);
		// The army it follows (0: none): every day it goes where that army stands
		unique_id_t PROPERTY_RW(follow_army_id, 0);
		// The army is cut off from every source of the supply (P8, WarManager::update_encirclements: no supply, no new men); encircled: its pocket touches the
		// land of an enemy too (it loses organisation and gives itself up); how many days it has had no organisation in a pocket
		bool PROPERTY_RW_CUSTOM_NAME(cut_off, is_cut_off, set_cut_off, false);
		bool PROPERTY_RW_CUSTOM_NAME(encircled, is_encircled, set_encircled, false);
		std::uint32_t PROPERTY_RW(encircled_days, 0);
		// The supply reaches the army only by the sea (WarManager::update_encirclements): ov_supply.SUPPLY_SEA_LOSS of it is lost on the way
		bool PROPERTY_RW_CUSTOM_NAME(supply_by_sea, is_supply_by_sea, set_supply_by_sea, false);
		// The bonus of the general of the army and the field marshal of its group to its supply, worked out every day
		fixed_point_t PROPERTY_RW(supply_leader_bonus, fixed_point_t::_0);
		// The way by sea (P4d): the army goes to the port sea_transfer_from, sails for sea_transfer_days_total days (sea_transfer_days_left > 0: at
		// sea, it stays in the list of the port and does not fight) to the port sea_transfer_to and goes on to sea_transfer_destination over the land
		ProvinceInstance* PROPERTY_PTR(sea_transfer_from, nullptr);
		ProvinceInstance* PROPERTY_PTR(sea_transfer_to, nullptr);
		ProvinceInstance* PROPERTY_PTR(sea_transfer_destination, nullptr);
		std::uint32_t PROPERTY(sea_transfer_days_total, 0);
		std::uint32_t PROPERTY_RW(sea_transfer_days_left, 0);
		// The navy that carries the army on its way by sea (SEA_TRANSFER_NEEDS_SHIPS, version 80); 0: the army sails alone
		unique_id_t PROPERTY(sea_transfer_navy_id, 0);
		// A landing on a coast (version 81): the ships sail to this sea and put the army ashore in sea_transfer_to; nullptr: to a port
		ProvinceInstance* PROPERTY_PTR(sea_transfer_landing_sea, nullptr);

	public:
		[[nodiscard]] bool is_at_sea() const {
			return sea_transfer_days_left > 0;
		}
		[[nodiscard]] bool has_sea_transfer() const {
			return sea_transfer_to != nullptr;
		}
		void set_sea_transfer(ProvinceInstance& from, ProvinceInstance& to, ProvinceInstance& destination, const std::uint32_t days,
			const unique_id_t navy_id = 0) {
			sea_transfer_navy_id = navy_id;
			sea_transfer_from = &from;
			sea_transfer_to = &to;
			sea_transfer_destination = &destination;
			sea_transfer_days_total = days;
			sea_transfer_days_left = 0;
		}
		void set_sea_transfer_landing_sea(ProvinceInstance* sea) {
			sea_transfer_landing_sea = sea;
		}
		void clear_sea_transfer() {
			sea_transfer_navy_id = 0;
			sea_transfer_landing_sea = nullptr;
			sea_transfer_from = nullptr;
			sea_transfer_to = nullptr;
			sea_transfer_destination = nullptr;
			sea_transfer_days_total = 0;
			sea_transfer_days_left = 0;
		}

		UnitInstanceGroupBranched(
			unique_id_t new_unique_id,
			std::string_view new_name,
			CountryInstance& new_country,
			ProvinceInstance& new_location
		);
		UnitInstanceGroupBranched(UnitInstanceGroupBranched&&) = default;

		void update_gamestate();
		void tick();

		std::span<const DivisionInstance> get_divisions() const {
			return divisions;
		}
		memory::vector<DivisionInstance>& get_mutable_divisions() {
			return divisions;
		}

		void set_transport_navy(NavyInstance* navy) {
			transport_navy = navy;
		}

		// Called daily: an army that stays still gains a dig in level every days_per_level days (up to cap),
		// moving or fighting resets it
		void advance_dig_in(const int32_t days_per_level, const int32_t cap);
		static constexpr std::uint8_t STANCE_NORMAL = 0, STANCE_ENTRENCH = 1, STANCE_RAID = 2, STANCE_REST = 3;
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
		// Far from the naval bases of the country and its allies today (WarManager::update_navy_range): the ships wear out
		bool PROPERTY_RW_CUSTOM_NAME(out_of_range, is_out_of_range, set_out_of_range, false);
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
		// Lands a carried army from the sea next to the coastal province; the share of the organisation the landing takes is reduced by penalty_reduction
		// (0..1: the naval invasion of a plan, P10)
		bool land_army(ArmyInstance& army, ProvinceInstance& target, fixed_point_t penalty_reduction = fixed_point_t::_0);
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
	struct ModifierEffectCache;
	struct CountryDefines;
	struct PopType;
	struct CultureManager;
	struct LeaderTraitManager;
	struct MilitaryDefines;
	struct Pop;
	struct DivisionTemplate;
	struct UnitTypeManager;

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
		// A regiment of a division recruited by a template (docs/RECRUITMENT.md): all its regiments are ready on the same day and form the division
		// division_id of the template template_id in a new army of the province (0: a regiment of its own)
		std::uint32_t division_id = 0;
		std::uint32_t template_id = 0;
		// A regiment raised by the mobilisation (docs/RECRUITMENT.md, "Мобилизация"): from a poor pop, without goods, out of the manpower of the
		// soldiers and of the monthly limit; it cannot be cancelled alone (the demobilisation cancels them all)
		bool mobilisation = false;
		// Over the monthly limit of the recruitment (ov_war.RECRUIT_MAX_REGIMENTS_PER_MONTH): the order waits, nothing is taken yet (no goods, no
		// men; the pops of a division are picked when it starts), its days do not run
		bool waiting = false;
		// The priority of the order (0 normal, 1 high, 2 urgent): the waiting orders of the higher priority start first
		std::uint32_t priority = 0;
	};

	inline constexpr std::uint32_t RECRUITMENT_PRIORITY_MAX = 2;

	// The mobilisation of a country (docs/RECRUITMENT.md, "Мобилизация"): the pool is the poor pops that are not soldiers nor slaves of the allowed
	// cultures in its provinces (owned and not occupied) times the share; the mobilised regiments are out of the manpower of the soldiers
	struct Mobilisation {
		std::int64_t pop_men = 0; // the men of the poor pops the pool is made of
		fixed_point_t share; // MOBILISATION_POP_SHARE + mobilisation_size, times 1 + mobilisation_size_pct
		std::int64_t pool_men = 0; // pop_men x share (less on the land that is not national, at most a share of every region, get_mobilisation_men)
		std::uint32_t pool_regiments = 0; // pool_men / men_per_regiment, at least MIN_MOBILIZE_LIMIT (0 without poor pops)
		std::int32_t men_per_regiment = 0;
		std::uint32_t regiments = 0; // the mobilised regiments in the armies
		std::int64_t men = 0; // their men now
		std::int64_t full_men = 0; // their men at full strength
		std::uint32_t pending_regiments = 0; // the mobilisation orders that are not ready yet
		std::int64_t pending_men = 0;
		std::int32_t days_left = 0; // the longest wait of the pending orders
		std::int32_t days_min = 0; // for a new mobilisation: the days of the fastest and the slowest province of the pool
		std::int32_t days_max = 0;
		std::int64_t free_men = 0; // pool_men - men - pending_men: the men for the reinforcement of the mobilised regiments
		fixed_point_t economy_impact; // MOBILISATION_ECONOMY_IMPACT_BASE + mobilisation_economy_impact of the modifiers (not below 0)
		fixed_point_t taken_share; // (men + pending_men) / pop_men
		fixed_point_t output_multiplier = fixed_point_t::_1; // of the RGOs and the factories while mobilised
		// The output multiplier a mobilisation of the whole pool would give (for the button)
		fixed_point_t planned_output_multiplier = fixed_point_t::_1;
	};

	// The manpower of a country in men (docs/RECRUITMENT.md): the maximum is the size of the pops of soldiers of the allowed cultures in its provinces
	// (owned and not occupied) times the multiplier (1 + MANPOWER_WAR_BONUS at war); in the army are the men of its regiments (strength times
	// TEMPLATE_MEN_PER_STRENGTH) and of its recruitment orders; free is the maximum less both (below zero when the army is bigger: the penalty)
	struct Manpower {
		std::int64_t pop_men = 0; // the pops of soldiers
		fixed_point_t multiplier = fixed_point_t::_1;
		std::int64_t max_men = 0;
		std::int64_t army_men = 0; // the regiments as they are now
		std::int64_t army_full_men = 0; // the regiments at full strength
		std::int64_t queued_men = 0;
		std::int64_t free_men = 0;
		std::uint32_t regiments = 0;
		std::int32_t men_per_regiment = 0; // POP_SIZE_PER_REGIMENT
		std::uint32_t max_regiments = 0; // max_men / men_per_regiment
		std::uint32_t available_regiments = 0; // free_men / men_per_regiment (0 when nothing is free)
		// The share of the army over the maximum (0 when it is within it) and the multiplier of the attack and the defence it gives
		fixed_point_t over_share;
		fixed_point_t combat_multiplier = fixed_point_t::_1;
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

	public:
		// The ids of the divisions (0 is no division)
		std::uint32_t next_division_id = 1;
		// The building of the naval base (the order "to the port" looks for one), set by the InstanceManager
		BuildingType const* unit_type_manager_naval_base = nullptr;
		// The men who died in the armies since the start (taken off the pops by apply_losses)
		std::int64_t men_killed_total = 0;
		// The month of today (year * 12 + month - 1), set at the start of tick: the monthly limit of the recruitment counts in it
		std::int32_t current_month = -1;

	private:

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

		// The definitions the mobilisation reads (set by the InstanceManager, they outlive all instances)
		ModifierEffectCache const* modifier_effect_cache = nullptr;
		CountryDefines const* country_defines = nullptr;
		UnitTypeManager const* unit_type_manager = nullptr;

		// The recruitment that waits for the monthly limit: the regiments the country may still start this month (-1: no limit)
		[[nodiscard]] std::int64_t get_recruit_limit_left(CountryInstance const& country) const;
		void count_recruited(CountryInstance& country, std::uint32_t regiments) const;
		// Takes the goods and the men of a waiting order (of all the regiments of its division) and starts its days; false if they are missing
		bool start_waiting_order(std::size_t index);

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
		void tick(const Date today);

		// Finds the shortest path (by land for armies, by sea between ports for navies) and starts moving the group,
		// false if the destination is unreachable. With append = true the route starts at the end of the current route
		// of the group and is added to it
		bool order_move(UnitInstanceGroup& group, ProvinceInstance& destination, MapInstance& map_instance, bool append = false);
		// The way by sea of an army that has no way over the land (P4d): to the nearest port of its country, by sea to the port of its country nearest
		// to the destination (no sea with a navy of an enemy at war), then over the land; false if there is none
		bool order_sea_transfer(ArmyInstance& army, ProvinceInstance& destination, MapInstance& map_instance);
		// The navy that carries an army on its way by sea: not busy, room for the whole army; it is ordered to the boarding port (nullptr: none)
		NavyInstance* find_transfer_navy(ArmyInstance const& army, ProvinceInstance& boarding, MapInstance& map_instance);
		// The navy waits for or carries an army on its way by sea: the AI and the field marshals leave it alone
		[[nodiscard]] bool is_navy_booked(NavyInstance const& navy) const;
		void update_ship_transfer(ArmyInstance& army, MapInstance& map_instance);
		// A landing on a coast (version 81): the army goes to the nearest port it controls, a navy with room for it takes it to the sea next to the
		// coast and puts it ashore there (the penalty of a landing). False: no port, no sea next to the coast it reaches, no ships
		bool order_sea_landing(ArmyInstance& army, ProvinceInstance& coast, MapInstance& map_instance);
		// Set while a way by sea is planned
		bool planning_sea_transfer = false;
		// Every day: an army that reached the port of its way by sea sails, one at sea counts its days and lands, then goes on
		void update_sea_transfers(MapInstance& map_instance);
		// The nearest port the country's navy can dock in, from the province (nullptr if there is none)
		ProvinceInstance* find_nearest_port(CountryInstance const& country, ProvinceInstance& from, MapInstance& map_instance);
		// The nearest province over the land with a naval base of the country (owned and held), nullptr if there is none (the order "to the port")
		ProvinceInstance* find_nearest_army_port(ArmyInstance const& army, MapInstance& map_instance) const;
		// The armies that follow another one go where it stands (every day)
		void update_follows(MapInstance& map_instance);

		// Empty if the regiment can be recruited from the pop, otherwise the reason it cannot (shown to the player); the men come out of the free
		// manpower of the country
		std::string_view get_recruitment_block_reason(
			CountryInstance const& country, RegimentType const& regiment_type, ProvinceInstance const& province, Pop const& pop
		) const;
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

		// The manpower of the country (the soldiers: the mobilised regiments are not in it)
		[[nodiscard]] Manpower get_manpower(CountryInstance const& country) const;

		void set_mobilisation_definitions(
			ModifierEffectCache const& new_modifier_effect_cache, CountryDefines const& new_country_defines,
			UnitTypeManager const& new_unit_type_manager
		) {
			modifier_effect_cache = &new_modifier_effect_cache;
			country_defines = &new_country_defines;
			unit_type_manager = &new_unit_type_manager;
		}
		// The pops a mobilisation takes the men from: poor (the strata "poor" of Vic2), not soldiers, not slaves, with men
		[[nodiscard]] static bool is_mobilisation_pop_type(PopType const& pop_type);
		[[nodiscard]] static bool is_mobilisation_pop(CountryInstance const& country, Pop const& pop);
		// The men every poor pop of the pool gives at the share: less on the land that is not national, each region at most a share of its population
		[[nodiscard]] std::vector<std::pair<Pop*, std::int64_t>> get_mobilisation_men(CountryInstance const& country, fixed_point_t share) const;
		// The mobilisation pool and the mobilised regiments of the country
		[[nodiscard]] Mobilisation get_mobilisation(CountryInstance const& country) const;
		// Days a province of the country takes to raise its mobilised regiments (MOBILISATION_DAYS, faster with the railroad)
		[[nodiscard]] std::int32_t get_mobilisation_days(CountryInstance const& country, ProvinceInstance const& province) const;
		// Empty if the country can mobilise (demobilise), otherwise the reason (Russian text for the player)
		[[nodiscard]] std::string_view get_mobilise_block_reason(CountryInstance const& country) const;
		[[nodiscard]] std::string_view get_demobilise_block_reason(CountryInstance const& country) const;
		// The mobilisation: orders of mobilised regiments (the types of the first template of the country, else the infantry) linked to the poor pops of
		// the pool, ready in the days of their provinces; no goods. The demobilisation disbands every mobilised regiment and cancels the orders: the men
		// go back to their pops, nobody dies
		bool mobilise(CountryInstance& country);
		bool demobilise(CountryInstance& country);
		// Every day: the output multiplier of the economy, the demobilisation DEMOBILISE_DAYS after the last war (one thread)
		void update_mobilisation(CountryInstance& country);
		// Sets the priority of an order (of all the regiments of its division); false if nothing changed
		bool set_recruitment_priority(CountryInstance& country, unique_id_t order_id, std::uint32_t priority);
		// Every day, first in InstanceManager::tick: the month of the limit, the waiting orders that may start (by their priority)
		void start_waiting_orders(const Date today);
		// The regiments the country may still start this month: -1 without a limit
		[[nodiscard]] std::int64_t get_recruitment_limit_left(CountryInstance const& country) const {
			return get_recruit_limit_left(country);
		}
		// The men of a regiment of the type at full strength
		[[nodiscard]] std::int64_t get_regiment_men(RegimentType const& regiment_type) const;
		// Takes the strength the regiment lost (unapplied_losses) off its pop, what the pop does not have off the other pops of soldiers of the
		// country (they die: the population falls)
		void apply_losses(RegimentInstance& regiment, CountryInstance& country);
		void apply_all_losses();
		// The maximum organisation of every regiment by the literacy of its pop (version 89, docs/LITERACY.md), monthly
		void update_literacy_organisation(Rate const& per_literacy);
		// The men of the group die with it (destroyed in a battle, given up in a pocket, sunk with its transports)
		void kill_group(UnitInstanceGroup& group);
		// A division by a template: empty if it can be recruited in the province, otherwise the reason (the types, the manpower, the goods)
		[[nodiscard]] std::string get_division_recruitment_block_reason(
			CountryInstance const& country, UnitTypeManager const& unit_type_manager, DivisionTemplate const& division_template,
			ProvinceInstance const& province
		) const;
		// Recruits every regiment of the template: the soldiers from the pops of the province first, then of the other provinces of the country, the
		// goods from the national stockpile; the regiments are ready together (the longest days of their types) as one division of the template
		bool start_division_recruitment(
			CountryInstance& country, UnitTypeManager const& unit_type_manager, DivisionTemplate const& division_template, ProvinceInstance& province
		);

		// Moves a regiment out of its army into a new army in the same province
		ArmyInstance* split_regiment(ArmyInstance& army, RegimentInstance& regiment);
		// Moves all the regiments of the source army into the target army (same country and province), deletes the source
		bool merge_armies(ArmyInstance& target, ArmyInstance& source);
		// A paid upgrade of one regiment to the type its type upgrades to (upgrades_to, upgrade_cost in the unit file): the goods are taken from the
		// national stockpile, the regiment keeps its id, its pop, its division and its share of strength and organisation. The reason is empty if possible.
		[[nodiscard]] static RegimentType const* get_upgrade_target(UnitTypeManager const& unit_types, RegimentType const& regiment_type);
		[[nodiscard]] static std::string_view get_upgrade_block_reason(
			CountryInstance const& country, ArmyInstance const& army, RegimentInstance const& regiment, RegimentType const* target
		);
		bool upgrade_regiment(CountryInstance& country, ArmyInstance& army, RegimentInstance& regiment, RegimentType const& target);
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
