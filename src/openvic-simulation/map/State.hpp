#pragma once

#include <functional>

#include <fmt/base.h>

#include "openvic-simulation/misc/MoneyLedger.hpp"
#include "openvic-simulation/economy/credit/Credit.hpp"
#include "openvic-simulation/core/memory/Colony.hpp"
#include "openvic-simulation/core/memory/FixedVector.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/core/portable/ForwardableSpan.hpp"
#include "openvic-simulation/economy/BuildingLevel.hpp"
#include "openvic-simulation/economy/production/FactoryInstance.hpp"
#include "openvic-simulation/population/PopsAggregate.hpp"
#include "openvic-simulation/types/ColonyStatus.hpp"
#include "openvic-simulation/types/TypedIndices.hpp"
#include "openvic-simulation/utility/Getters.hpp"

namespace OpenVic {
	struct BaseIssue;
	struct BuildingType;
	struct CountryInstance;
	struct CountryParty;
	struct Culture;
	struct MapDefinition;
	struct Pop;
	struct PopsAggregateDeps;
	struct PopType;
	struct ProvinceInstance;
	struct Religion;
	struct CreditManager;
	struct StateManager;
	struct StateSet;
	struct Strata;

	struct State : PopsAggregate {
		friend struct StateManager;

	private:
		CountryInstance* previous_country_ptr = nullptr;

		ProvinceInstance* PROPERTY_PTR(capital);
		memory::vector<std::reference_wrapper<ProvinceInstance>> SPAN_PROPERTY(provinces);
		colony_status_t PROPERTY(colony_status);
		fixed_point_t PROPERTY(industrial_power);
		bool PROPERTY_CUSTOM_PREFIX(coastal, is, false);

		memory::FixedVector<
			memory::vector<std::reference_wrapper<Pop>>,
			pop_type_index_t
		> SPAN_PROPERTY(pops_cache_by_type);

		memory::colony<FactoryInstance> PROPERTY(factories);
		// Savings of the capitalists of the state: part of the dividends they get, used to cover the losses of the
		// factories they own (and, later, to invest)
		fixed_point_t PROPERTY(capitalist_fund);
		CreditManager* credit_manager = nullptr;

		void _update_country();

	public:
		StateSet const& state_set;

		State(
			StateSet const& new_state_set,
			ProvinceInstance* new_capital,
			memory::vector<std::reference_wrapper<ProvinceInstance>>&& new_provinces,
			colony_status_t new_colony_status,
			PopsAggregateDeps const& pops_aggregate_deps
		);
		State(State&&) = delete;
		State(State const&) = delete;
		State& operator=(State&&) = delete;
		State& operator=(State const&) = delete;

		memory::string get_identifier() const;
		CountryInstance* get_owner() const;

		constexpr bool is_colonial_state() const {
			return is_colonial(colony_status);
		}

		void update_gamestate();

		// Set by the credit manager when the first loan of the state is given
		void set_credit_manager(CreditManager* new_credit_manager) {
			credit_manager = new_credit_manager;
		}
		CreditManager* get_credit_manager() const {
			return credit_manager;
		}
		void add_to_capitalist_fund(const fixed_point_t amount) {
			capitalist_fund += amount;
			MoneyLedger::record(MoneyLedger::label_t::capitalist_fund_in, amount);
		}
		// The money the capitalists can spend on a purpose: savings plus the unspent money of the loans that allow it
		fixed_point_t get_capitalist_money(Loan::purpose_t purpose, std::int32_t construction_days) const;
		fixed_point_t take_capitalist_money(fixed_point_t amount, Loan::purpose_t purpose, std::int32_t construction_days);
		// Takes at most the amount out of the fund, returns what was taken
		fixed_point_t take_from_capitalist_fund(const fixed_point_t amount) {
			const fixed_point_t taken = amount < capitalist_fund ? amount : capitalist_fund;
			capitalist_fund -= taken;
			MoneyLedger::record(MoneyLedger::label_t::capitalist_fund_out, -taken);
			return taken > fixed_point_t::_0 ? taken : fixed_point_t::_0;
		}

		memory::colony<FactoryInstance>& get_mutable_factories() {
			return factories;
		}
		// The factory of the given type, nullptr if the state has none
		FactoryInstance* find_factory(BuildingType const& building_type);
		FactoryInstance const* find_factory(BuildingType const& building_type) const;
		bool has_free_factory_slot() const;
		// Adds a factory of the given type with the given completed level,
		// nullptr if there is no free slot or the state already has a factory of that type
		FactoryInstance* add_factory(BuildingType const& building_type, building_level_t level);
		// Removes a factory (the player deletes it). Its money (budget and expansion fund) goes to its owners: the state's share to the
		// treasury of the owner country, the rest to the savings of the capitalists. Returns false if it is not a factory of this state.
		bool remove_factory(FactoryInstance& factory);
	};

	struct Region;

	struct StateSet {
		friend struct StateManager;

		using states_t = memory::colony<State>;

	private:
		states_t PROPERTY(states);

	public:
		Region const& region;

		StateSet(Region const& new_region);

		size_t get_state_count() const;

		void update_gamestate();

		states_t& get_mutable_states() {
			return states;
		}
	};

	struct MapInstance;

	/* Contains all current states.*/
	struct StateManager {
	private:
		memory::vector<StateSet> SPAN_PROPERTY(state_sets);

		bool add_state_set(
			MapInstance& map_instance, Region const& region,
			PopsAggregateDeps const& pops_aggregate_deps,
			forwardable_span<const Strata> strata_keys,
			forwardable_span<const PopType> pop_type_keys
		);

	public:
		/* Creates states from current province gamestate & regions, sets province state value.
		 * After this function, the `regions` property is unmanaged and must be carefully updated and
		 * validated by functions that modify it. */
		bool generate_states(
			MapDefinition const& map_definition,
			MapInstance& map_instance,
			PopsAggregateDeps const& pops_aggregate_deps,
			forwardable_span<const Strata> strata_keys,
			forwardable_span<const PopType> pop_type_keys
		);

		void reset();

		void update_gamestate();

		memory::vector<StateSet>& get_mutable_state_sets() {
			return state_sets;
		}
	};
}

template<>
struct fmt::formatter<OpenVic::State> : fmt::formatter<string_view> {
	format_context::iterator format(OpenVic::State const& state, fmt::format_context& ctx) const;
};
