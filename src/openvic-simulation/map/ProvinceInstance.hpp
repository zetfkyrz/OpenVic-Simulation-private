#pragma once

#include <atomic>
#include <cstddef>
#include <functional>

#include "openvic-simulation/core/memory/Colony.hpp"
#include "openvic-simulation/core/memory/FixedVector.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/Timespan.hpp"
#include "openvic-simulation/core/portable/ForwardableSpan.hpp"
#include "openvic-simulation/core/stl/containers/TypedSpan.hpp"
#include "openvic-simulation/economy/BuildingInstance.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/economy/production/ResourceGatheringOperation.hpp"
#include "openvic-simulation/map/LifeRating.hpp"
#include "openvic-simulation/military/UnitBranchedGetterMacro.hpp"
#include "openvic-simulation/modifier/ModifierSum.hpp"
#include "openvic-simulation/population/Pop.hpp"
#include "openvic-simulation/population/PopIdInProvince.hpp"
#include "openvic-simulation/population/PopsAggregate.hpp"
#include "openvic-simulation/types/ColonyStatus.hpp"
#include "openvic-simulation/types/FlagStrings.hpp"
#include "openvic-simulation/types/HasIdentifier.hpp"
#include "openvic-simulation/types/HasIndex.hpp"
#include "openvic-simulation/types/OrderedContainers.hpp"
#include "openvic-simulation/types/TypedIndices.hpp"
#include "openvic-simulation/types/UnitBranchType.hpp"

namespace OpenVic {
	struct BaseIssue;
	struct BuildingType;
	struct CountryInstance;
	struct CountryInstanceManager;
	struct CountryParty;
	struct Crime;
	struct Culture;
	struct GameRulesManager;
	struct GoodDefinition;
	struct Ideology;
	struct InstanceManager;
	struct MapInstance;
	struct MilitaryDefines;
	struct PopDeps;
	struct ProvinceDefinition;
	struct ProvinceHistoryEntry;
	struct ProvinceInstanceDeps;
	struct Religion;
	struct ResourceGatheringOperationDeps;
	struct State;
	struct StaticModifierCache;
	struct Strata;
	struct TerrainType;
	struct UnitInstanceGroup;

	//HasIndex index_t must match ProvinceDefinition's index_t
	struct ProvinceInstance
		: HasIdentifierAndColour,
		HasIndex<ProvinceInstance, province_index_t>,
		FlagStrings,
		PopsAggregate {
		friend struct MapInstance;

		static constexpr std::string_view get_colony_status_string(colony_status_t colony_status) {
			using enum colony_status_t;
			switch (colony_status) {
			case STATE:
				return "state";
			case PROTECTORATE:
				return "protectorate";
			case COLONY:
				return "colony";
			default:
				return "unknown colony status";
			}
		}

	private:
		GameRulesManager const& game_rules_manager;

		TerrainType const* PROPERTY(terrain_type);
		life_rating_t PROPERTY(life_rating, life_rating_t { 0 });
		colony_status_t PROPERTY(colony_status, colony_status_t::STATE);
	public:
		void set_colony_status(const colony_status_t new_status) {
			colony_status = new_status;
		}
	private:
		State* PROPERTY_PTR(state, nullptr);
		// Factories of the province history, they are added to the state when the new game is initialised
		ordered_map<building_type_index_t, building_level_t> PROPERTY_REF(pending_state_buildings);

		CountryInstance* PROPERTY_PTR(owner, nullptr);
		ModifierSum const& get_owner_modifier_sum() const;
		CountryInstance* PROPERTY_PTR(controller, nullptr);
		CountryInstance* PROPERTY_PTR(country_to_report_economy, nullptr);
		ordered_set<CountryInstance*> PROPERTY(cores);

		// The total/resultant modifier of local effects on this province (global effects come from the province's owner)
		ModifierSum PROPERTY(modifier_sum);
		memory::vector<ModifierInstance> SPAN_PROPERTY(event_modifiers);

		bool PROPERTY(slave, false);
		// Used for "minorities = yes/no" condition
		bool PROPERTY(has_unaccepted_pops, false);
		bool PROPERTY_RW(connected_to_capital, false);
		// Set by the trade network every month: the railroad level (0..1) and the share of the price that a seller here loses on the
		// way to the market of its country (the better the railroad, the smaller)
		fixed_point_t PROPERTY(infrastructure_fraction);
		// The supply limit (docs/PLAN_ARMY_SUPPLY_FRONTS.md, section 1), worked out every day: the part that the province itself gives
		// (the modifiers of the terrain, the type of the province, the climate and the buildings) and the levels of its railroad
		fixed_point_t PROPERTY(supply_limit_local);
		fixed_point_t PROPERTY(supply_rail_level);
		fixed_point_t PROPERTY(freight_rate);
		bool PROPERTY_RW(is_overseas, false);
		bool PROPERTY(has_empty_adjacent_province, false);
		memory::vector<std::reference_wrapper<const ProvinceInstance>> SPAN_PROPERTY(adjacent_nonempty_land_provinces);
		Crime const* PROPERTY_RW(crime, nullptr);
		ResourceGatheringOperation PROPERTY(rgo);
		memory::FixedVector<BuildingInstance, province_building_index_t> buildings;
	public:
		constexpr TypedSpan<province_building_index_t, const BuildingInstance> get_buildings() const {
			return buildings;
		}
	private:
		memory::vector<std::reference_wrapper<ArmyInstance>> SPAN_PROPERTY(armies);
		memory::vector<std::reference_wrapper<NavyInstance>> SPAN_PROPERTY(navies);
		// The number of land regiments currently in the province, including those being transported by navies
		size_t PROPERTY(land_regiment_count, 0);
		Timespan PROPERTY(occupation_duration);

	public:
		OV_UNIT_BRANCHED_GETTER(get_unit_instance_groups, armies, navies);
		OV_UNIT_BRANCHED_GETTER_CONST(get_unit_instance_groups, armies, navies);

	private:
		pop_id_in_province_t last_pop_id{0};
		memory::colony<Pop> PROPERTY(pops); // TODO - replace with a more easily vectorisable container?
		// The sums of the political support of the pops (see PopsAggregate) are only added up again when this is set, which happens
		// when the political support of a pop changes (politics, once a month) or when a pop is added; on the other days the
		// province keeps the sums of the day before. Everything that changes the political support of a pop must call
		// mark_political_support_dirty().
		bool political_support_dirty = true;
		inline static std::atomic<bool> verify_political_support_cache = false;
		inline static std::atomic<std::size_t> political_support_cache_mismatch_count = 0;
		void _update_pops(MilitaryDefines const& military_defines);
		bool convert_rgo_worker_pops_to_equivalent(
			TypedSpan<pop_type_index_t, const PopType> pop_types,
			ProductionType const& production_type
		);
		void initialise_rgo();

		memory::FixedVector<
			memory::vector<std::reference_wrapper<Pop>>,
			pop_type_index_t
		> SPAN_PROPERTY(pops_cache_by_type);
	public:
		ProvinceDefinition const& province_definition;

		ProvinceInstance(
			ProvinceDefinition const& new_province_definition,
			ProvinceInstanceDeps const& province_instance_deps
		);
		ProvinceInstance(ProvinceInstance const&) = delete;
		ProvinceInstance& operator=(ProvinceInstance const&) = delete;
		ProvinceInstance(ProvinceInstance&&) = delete;
		ProvinceInstance& operator=(ProvinceInstance&&) = delete;

		inline explicit constexpr operator ProvinceDefinition const&() const {
			return province_definition;
		}

		void set_state(State* new_state);
		void set_trade_data(const fixed_point_t new_infrastructure_fraction, const fixed_point_t new_freight_rate) {
			infrastructure_fraction = new_infrastructure_fraction;
			freight_rate = new_freight_rate;
		}

		// Every part of the supply limit of the province for the armies of a country (the sum of the parts is the capacity) and
		// the load: the supply consumption of the regiments of that country in the province. One function makes both the numbers
		// of the game and the tooltips, so they cannot differ
		struct supply_breakdown_t {
			fixed_point_t local_limit; // what the province gives
			fixed_point_t country_limit; // the global supply_limit of the country
			fixed_point_t limit_multiplier; // SUPPLY_LIMIT_BASE
			fixed_point_t rail_level; // levels of the railroad
			fixed_point_t rail_multiplier; // 1 + bonus * levels
			fixed_point_t technology_multiplier; // percentage change of the country and the technologies
			fixed_point_t home_bonus; // the bonus of the own land (0 on the land of others)
			fixed_point_t capacity; // the supply limit of the province
			fixed_point_t load; // the supply consumption of the regiments of the country in the province
			bool is_home = false; // the province is owned by the country
		};
		static inline OpenVicSupplyDefines const* openvic_supply_defines = nullptr;
		static void set_openvic_supply_defines(OpenVicSupplyDefines const& defines) {
			openvic_supply_defines = &defines;
		}
		// country can be null (the numbers of the province without a country: no global limit, no technologies, no own land)
		[[nodiscard]] supply_breakdown_t get_supply_breakdown(CountryInstance const* country) const;
		[[nodiscard]] fixed_point_t get_supply_load(CountryInstance const* country) const;

		GoodDefinition const* get_rgo_good() const;
		bool set_rgo_production_type_nullable(
			TypedSpan<pop_type_index_t, const PopType> pop_types,
			ProductionType const* rgo_production_type_nullable
		);

		bool set_owner(CountryInstance* new_owner);
		bool set_controller(CountryInstance* new_controller);

		// The warn argument controls whether a log message is emitted when a core already does/doesn't exist, e.g. we may
		// want to know if there are redundant province history instructions setting a core multiple times, but we may not
		// want to be swamped with log messages by effect scripts which use add_core/remove_core very liberally. Regardless
		// of warn's value, both functions will still return true if the core in question already does/doesn't exist.
		bool add_core(CountryInstance& new_core, bool warn);
		bool remove_core(CountryInstance& core_to_remove, bool warn);
		constexpr bool is_owner_core() const {
			return owner != nullptr && cores.contains(owner);
		}
		constexpr bool is_colonial_province() const {
			return is_colonial(colony_status);
		}
		constexpr bool is_occupied() const {
			return owner != controller;
		}
		constexpr bool is_empty() const {
			return owner == nullptr;
		}

		constexpr bool may_build_here() const {
			return !is_occupied();
		}
		bool expand_building(
			ModifierEffectCache const& modifier_effect_cache,
			const province_building_index_t index,
			CountryInstance& actor
		);

		bool add_pop_vec(
			std::span<const PopBase> pop_vec,
			PopDeps const& pop_deps
		);
		size_t get_pop_count() const;
		void mark_political_support_dirty() {
			political_support_dirty = true;
		}
		// For the self tests and the headless option -V: every day the political support is also added up from the pops and compared
		// with the sums that the province kept, a difference is logged and counted
		static void set_verify_political_support_cache(const bool verify) {
			verify_political_support_cache = verify;
		}
		static std::size_t get_political_support_cache_mismatch_count() {
			return political_support_cache_mismatch_count;
		}

		void update_modifier_sum(Date today, StaticModifierCache const& static_modifier_cache);
		void update_country_modifier_sum();
		fixed_point_t get_modifier_effect_value(ModifierEffect const& effect) const;

		void for_each_contributing_modifier(ModifierEffect const& effect, ContributingModifierCallback auto callback) const {
			if (effect.is_local()) {
				// Province-targeted/local effects come from the province itself, only modifiers applied directly to the
				// province contribute to these effects.
				modifier_sum.for_each_contributing_modifier(effect, std::move(callback));
			} else if (owner != nullptr) {
				// Non-province targeted/global effects come from the province's owner, even those applied locally
				// (e.g. via a province event modifier) are passed up to the province's controller and only affect the
				// province if the controller is also the owner.
				get_owner_modifier_sum().for_each_contributing_modifier(effect, std::move(callback));
			}
		}
		void update_gamestate(InstanceManager const& instance_manager);
		static constexpr size_t VECTORS_FOR_PROVINCE_TICK = std::max(
			ResourceGatheringOperation::VECTORS_FOR_RGO_TICK,
			Pop::VECTORS_FOR_POP_TICK
		);
		void province_tick(
			const Date today,
			PopValuesFromProvince& reusable_pop_values,
			RandomU32& random_number_generator,
			TypedSpan<good_index_t, char> reusable_goods_mask,
			forwardable_span<
				memory::vector<fixed_point_t>,
				VECTORS_FOR_PROVINCE_TICK
			> reusable_vectors
		);
		void initialise_for_new_game(
			const Date today,
			PopValuesFromProvince& reusable_pop_values,
			RandomU32& random_number_generator,
			TypedSpan<good_index_t, char> reusable_goods_mask,
			forwardable_span<
				memory::vector<fixed_point_t>,
				VECTORS_FOR_PROVINCE_TICK
			> reusable_vectors
		);

		bool add_unit_instance_group(UnitInstanceGroup& group);
		bool remove_unit_instance_group(UnitInstanceGroup const& group);

		bool apply_history_to_province(ProvinceHistoryEntry const& entry, CountryInstanceManager& country_manager);

		void setup_pop_test_values(TypedSpan<reform_index_t, const Reform> reforms);
		memory::colony<Pop>& get_mutable_pops();
		// A new pop in the province (people moved here by migration, promotion, ...), nullptr in water provinces
		Pop* add_pop(PopBase const& pop_base, PopDeps const& pop_deps);
	private:
		template<typename T>
		static std::conditional_t<std::is_const_v<T>, Pop const*, Pop*> _find_pop_by_id(T& self, const pop_id_in_province_t pop_id);
	public:
		Pop* find_pop_by_id(const pop_id_in_province_t pop_id);
		Pop const* find_pop_by_id(const pop_id_in_province_t pop_id) const;
	};
}